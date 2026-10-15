#include "BlePeripheral.h"
#include "BleStack.h"
#include "Platform.h"

#include <string.h>
#include "esp_log.h"
#include "services/gap/ble_svc_gap.h"

static const char* TAG = "BlePeripheral";

static constexpr uint32_t BLE_DISCONNECT_WAIT_MS = 500;
// Legacy scan response: 31 bytes minus the 2 bytes of AD structure header
static constexpr size_t BLE_SCAN_RSP_NAME_MAX = 29;

void BlePeripheral::begin(const char* deviceName)
{
  if (started) return;
  if (!bleStackIsReady())
  {
    ESP_LOGE(TAG, "BLE stack is not ready");
    return;
  }

  if (deviceName && *deviceName)
    ble_svc_gap_device_name_set(deviceName);

  configureDefaults();
  configureSecurity();

  std::lock_guard<std::mutex> lock(advMutex);
  connHandle_ = BLE_HS_CONN_HANDLE_NONE;
  started = true;
  advertise();
}

void BlePeripheral::end()
{
  if (!started) return;

  {
    // After this advertising is not restarted by the host task anymore
    std::lock_guard<std::mutex> lock(advMutex);
    started = false;
    ble_gap_adv_stop();
  }

  uint16_t handle = connHandle_;
  if (handle != BLE_HS_CONN_HANDLE_NONE)
  {
    ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    for (uint32_t waited = 0; connHandle_ != BLE_HS_CONN_HANDLE_NONE && waited < BLE_DISCONNECT_WAIT_MS; waited += 10)
      delayMs(10);
    if (connHandle_ != BLE_HS_CONN_HANDLE_NONE)
      ESP_LOGW(TAG, "Peer did not disconnect in time");
  }

  destroyServices();
  connHandle_ = BLE_HS_CONN_HANDLE_NONE;
}

bool BlePeripheral::isStarted() const
{
  return started;
}

bool BlePeripheral::isConnected() const
{
  return connectedCount() > 0;
}

uint8_t BlePeripheral::connectedCount() const
{
  return connHandle_ != BLE_HS_CONN_HANDLE_NONE ? 1 : 0;
}

void BlePeripheral::configureSecurity()
{
  // No pairing is required to use the services, but accept it if the
  // central asks for it. Without a display or keyboard that is Just Works.
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 0;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
}

// Must be called with advMutex held
void BlePeripheral::advertise()
{
  if (!started) return;

  struct ble_hs_adv_fields fields;
  memset(&fields, 0, sizeof(fields));
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  configureAdvertising(fields);

  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0)
  {
    ESP_LOGE(TAG, "Setting advertising data failed, rc=%d", rc);
    return;
  }

  // The name does not fit next to a 128-bit service UUID, so it goes to the
  // scan response
  struct ble_hs_adv_fields scanRsp;
  memset(&scanRsp, 0, sizeof(scanRsp));
  const char* name = ble_svc_gap_device_name();
  size_t nameLen = strlen(name);
  scanRsp.name = (const uint8_t*)name;
  scanRsp.name_is_complete = nameLen <= BLE_SCAN_RSP_NAME_MAX;
  scanRsp.name_len = scanRsp.name_is_complete ? nameLen : BLE_SCAN_RSP_NAME_MAX;

  rc = ble_gap_adv_rsp_set_fields(&scanRsp);
  if (rc != 0)
  {
    ESP_LOGE(TAG, "Setting scan response failed, rc=%d", rc);
    return;
  }

  struct ble_gap_adv_params params;
  memset(&params, 0, sizeof(params));
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  rc = ble_gap_adv_start(bleStackOwnAddrType(), nullptr, BLE_HS_FOREVER, &params, gapEventCallback, this);
  if (rc != 0 && rc != BLE_HS_EALREADY)
    ESP_LOGE(TAG, "Starting advertising failed, rc=%d", rc);
}

int BlePeripheral::gapEventCallback(struct ble_gap_event* event, void* arg)
{
  return static_cast<BlePeripheral*>(arg)->handleGapEvent(event);
}

int BlePeripheral::handleGapEvent(struct ble_gap_event* event)
{
  switch (event->type)
  {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0)
      {
        connHandle_ = event->connect.conn_handle;
        if (!started)
        {
          // Connected while we were shutting down
          ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
          return 0;
        }
        onConnect(event->connect.conn_handle);
      }
      else
      {
        // Connection attempt failed, keep advertising
        std::lock_guard<std::mutex> lock(advMutex);
        advertise();
      }
      return 0;

    case BLE_GAP_EVENT_DISCONNECT:
    {
      uint16_t handle = event->disconnect.conn.conn_handle;
      if (handle == connHandle_)
      {
        onDisconnect(handle);
        connHandle_ = BLE_HS_CONN_HANDLE_NONE;
      }
      std::lock_guard<std::mutex> lock(advMutex);
      advertise();
      return 0;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
      if (connHandle_ == BLE_HS_CONN_HANDLE_NONE)
      {
        std::lock_guard<std::mutex> lock(advMutex);
        advertise();
      }
      return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
    {
      // The central lost its keys and pairs again, drop the old bond
      struct ble_gap_conn_desc desc;
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
        ble_store_util_delete_peer(&desc.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
      ESP_LOGI(TAG, "Encryption change, status=%d", event->enc_change.status);
      return 0;

    default:
      return onGapEvent(event);
  }
}
