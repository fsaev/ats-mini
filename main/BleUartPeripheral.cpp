#include "BleUartPeripheral.h"
#include "Platform.h"

#include <string.h>
#include "esp_log.h"
#include "host/ble_hs.h"

static const char* TAG = "BleUart";

// Nordic UART service, the UUIDs are stored in little endian order
static const ble_uuid128_t uartServiceUuid = BLE_UUID128_INIT(
  0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t uartRxUuid = BLE_UUID128_INIT(
  0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t uartTxUuid = BLE_UUID128_INIT(
  0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static uint16_t uartTxValHandle = 0;

std::atomic<BleUartPeripheral*> BleUartPeripheral::activeInstance{nullptr};

static int uartAccess(uint16_t connHandle, uint16_t attrHandle, struct ble_gatt_access_ctxt* ctxt, void* arg)
{
  (void)connHandle;
  (void)attrHandle;
  (void)arg;

  // Only RX (write without response) is writable, TX is notify only
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    return BLE_ATT_ERR_UNLIKELY;

  // The data may be spread over a chain of mbufs, copy it out in pieces
  uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
  uint8_t piece[64];
  uint16_t offset = 0;
  while (offset < length)
  {
    uint16_t n = length - offset;
    if (n > sizeof(piece)) n = sizeof(piece);
    if (os_mbuf_copydata(ctxt->om, offset, n, piece) != 0)
      return BLE_ATT_ERR_UNLIKELY;
    BleUartPeripheral::rxReceived(piece, n);
    offset += n;
  }
  return 0;
}

static struct ble_gatt_chr_def uartCharacteristics[3];
static struct ble_gatt_svc_def uartServices[2];

int bleUartGattInit()
{
  memset(uartCharacteristics, 0, sizeof(uartCharacteristics));
  memset(uartServices, 0, sizeof(uartServices));

  uartCharacteristics[0].uuid = &uartTxUuid.u;
  uartCharacteristics[0].access_cb = uartAccess;
  uartCharacteristics[0].flags = BLE_GATT_CHR_F_NOTIFY;
  uartCharacteristics[0].val_handle = &uartTxValHandle;

  uartCharacteristics[1].uuid = &uartRxUuid.u;
  uartCharacteristics[1].access_cb = uartAccess;
  uartCharacteristics[1].flags = BLE_GATT_CHR_F_WRITE_NO_RSP;
  // uartCharacteristics[2] stays zeroed, it terminates the list

  uartServices[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  uartServices[0].uuid = &uartServiceUuid.u;
  uartServices[0].characteristics = uartCharacteristics;
  // uartServices[1] stays zeroed, it terminates the list

  int rc = ble_gatts_count_cfg(uartServices);
  if (rc != 0) return rc;
  return ble_gatts_add_svcs(uartServices);
}

void BleUartPeripheral::rxReceived(const uint8_t* data, size_t length)
{
  BleUartPeripheral* self = activeInstance;
  if (self == nullptr || length == 0) return;

  self->abortPending = true;
  // Bytes which do not fit are dropped
  self->rxBuf.write(data, length);
}

bool BleUartPeripheral::consumeAbortPending()
{
  bool pending = abortPending.exchange(false);
  if (pending)
    rxBuf.flush();
  return pending;
}

bool BleUartPeripheral::canSend() const
{
  return connHandle() != BLE_HS_CONN_HANDLE_NONE && txSubscribed;
}

size_t BleUartPeripheral::txPayloadSize() const
{
  size_t mtu = txPeerMtu;
  if (mtu < BLE_MIN_MTU) mtu = BLE_MIN_MTU;
  if (mtu > BLE_MAX_MTU) mtu = BLE_MAX_MTU;
  size_t payload = mtu - 3;
  if (payload > BLE_TX_PAYLOAD_CAP)
    payload = BLE_TX_PAYLOAD_CAP;
  return payload;
}

// It is hard to achieve max throughput witout using delays...
//
// https://github.com/nkolban/esp32-snippets/issues/773
// https://github.com/espressif/arduino-esp32/issues/8413
// https://github.com/espressif/esp-idf/issues/9097
// https://github.com/espressif/esp-idf/issues/16889
// https://github.com/espressif/esp-nimble/issues/75
// https://github.com/espressif/esp-nimble/issues/106
// https://github.com/h2zero/esp-nimble-cpp/issues/347
//
// Current workaround:
// - leave the payload in txBuf until the stack accepted the notification
// - defer only the start of a new burst to batch small writes
// - retry the same payload after the stack ran out of buffers
// - send one notification per call so that the mbuf pool is not drained
//
// ATT allows up to negotiated_mtu - 3 bytes of value payload, but using the
// full payload repeatedly caused BLE_HS_ENOMEM / BLE_HS_EAPP on this
// NimBLE/ESP32 path. BLE_TX_PAYLOAD_CAP is therefore an empirical notify cap:
// it limits outbound notifications without changing the negotiated ATT MTU.
//
// ble_gatts_notify_custom() is synchronous: when it returns 0 the packet has
// been queued to the controller, there is no completion callback to wait for.
size_t BleUartPeripheral::pumpTx()
{
  if (txResetPending.exchange(false))
  {
    txBuf.flush();
    txDeferUntilMs = 0;
    txRetryAfterMs = 0;
  }

  if (!canSend() || txBuf.empty()) return 0;

  uint32_t now = nowMs();
  if ((txRetryAfterMs != 0) && ((int32_t)(now - txRetryAfterMs) < 0)) return 0;
  txRetryAfterMs = 0;
  if ((txDeferUntilMs != 0) && ((int32_t)(now - txDeferUntilMs) < 0)) return 0;
  txDeferUntilMs = 0;

  // One notification per call. A queued notification holds on to an mbuf
  // until the controller has sent it, a burst could drain the mbuf pool and
  // starve the other ATT and SMP traffic. We are called again soon anyway.
  size_t chunkSize = txBuf.peek(txChunk, txPayloadSize());
  if (chunkSize == 0) return 0;

  // The notify call takes over the mbuf, also if it fails
  struct os_mbuf* om = ble_hs_mbuf_from_flat(txChunk, chunkSize);
  int rc = om ? ble_gatts_notify_custom(connHandle(), uartTxValHandle, om) : BLE_HS_ENOMEM;
  if (rc == 0)
  {
    txBuf.discard(chunkSize);
    return chunkSize;
  }

  switch (rc)
  {
    case BLE_HS_ENOMEM:
    case BLE_HS_EAPP:
    case BLE_HS_EAGAIN:
    case BLE_HS_EBUSY:
    case BLE_HS_ESTALLED:
      // Keep the payload and retry it later
      txRetryAfterMs = nowMs() + BLE_NOTIFY_RETRY_DELAY_MS;
      break;

    default:
      ESP_LOGW(TAG, "Notify failed, rc=%d", rc);
      txBuf.flush();
      break;
  }
  return 0;
}

void BleUartPeripheral::resetSession()
{
  abortPending = false;
  txResetPending = true;
  txPeerMtu = BLE_MIN_MTU;
  txSubscribed = false;
}

void BleUartPeripheral::configureDefaults()
{
  activeInstance = this;
  ble_att_set_preferred_mtu(BLE_MAX_MTU);
  ble_gap_set_prefered_default_le_phy(BLE_GAP_LE_PHY_ANY_MASK, BLE_GAP_LE_PHY_ANY_MASK);
}

void BleUartPeripheral::configureAdvertising(struct ble_hs_adv_fields& fields)
{
  fields.uuids128 = const_cast<ble_uuid128_t*>(&uartServiceUuid);
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
}

void BleUartPeripheral::destroyServices()
{
  resetSession();
  rxBuf.flush();
  // The session reset is processed by the next pump, do it now as nobody
  // will call it while stopped
  txBuf.flush();
  txResetPending = false;
  txDeferUntilMs = 0;
  txRetryAfterMs = 0;
  activeInstance = nullptr;
}

void BleUartPeripheral::onConnect(uint16_t connHandle)
{
  txPeerMtu = BLE_MIN_MTU;
  txSubscribed = false;
  txResetPending = true;
  ble_gap_set_prefered_le_phy(connHandle, BLE_GAP_LE_PHY_ANY_MASK, BLE_GAP_LE_PHY_ANY_MASK, BLE_GAP_LE_PHY_CODED_ANY);
}

void BleUartPeripheral::onDisconnect(uint16_t connHandle)
{
  (void)connHandle;
  resetSession();
  rxBuf.flush();
}

int BleUartPeripheral::onGapEvent(struct ble_gap_event* event)
{
  switch (event->type)
  {
    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle != uartTxValHandle ||
          event->subscribe.conn_handle != connHandle())
        return 0;

      txSubscribed = !!event->subscribe.cur_notify;
      if (txSubscribed)
      {
        uint16_t mtu = ble_att_mtu(event->subscribe.conn_handle);
        if (mtu >= BLE_MIN_MTU)
          txPeerMtu = mtu;
      }
      else
        txResetPending = true;
      return 0;

    case BLE_GAP_EVENT_MTU:
      if (event->mtu.conn_handle == connHandle() && event->mtu.value >= BLE_MIN_MTU)
        txPeerMtu = event->mtu.value;
      return 0;

    default:
      return 0;
  }
}

int BleUartPeripheral::available()
{
  pumpTx();
  return (int)rxBuf.available();
}

int BleUartPeripheral::peek()
{
  pumpTx();
  return rxBuf.peekByte();
}

int BleUartPeripheral::read()
{
  pumpTx();
  int value = rxBuf.readByte();
  if (value >= 0)
    abortPending = false;
  return value;
}

void BleUartPeripheral::flush()
{
  // Give up if the peer does not take the data, rather than blocking forever
  uint32_t start = nowMs();
  while (!txBuf.empty())
  {
    if (!canSend()) break;
    if ((uint32_t)(nowMs() - start) > 2000) break;
    pumpTx();
    // Pace the notifications, see pumpTx()
    delayMs(1);
  }
}

size_t BleUartPeripheral::write(const uint8_t* data, size_t size)
{
  if (!canSend()) return 0;

  size_t writtenByteCount = 0;
  while (writtenByteCount < size)
  {
    pumpTx();

    bool wasEmpty = txBuf.empty();
    size_t queuedByteCount = txBuf.write(data + writtenByteCount, size - writtenByteCount);
    writtenByteCount += queuedByteCount;
    if ((queuedByteCount > 0) && wasEmpty)
    {
      uint32_t deferUntilMs = nowMs() + BLE_TX_DEFER_MS;
      if ((txDeferUntilMs == 0) || ((int32_t)(deferUntilMs - txDeferUntilMs) > 0))
        txDeferUntilMs = deferUntilMs;
    }
    if (writtenByteCount >= size)
      break;

    if (!canSend())
      return writtenByteCount;

    delayMs(1);
  }

  if (!canSend())
    return writtenByteCount;

  pumpTx();
  return writtenByteCount;
}
