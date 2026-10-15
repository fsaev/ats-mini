#include "BleCentral.h"
#include "BleStack.h"
#include "Platform.h"

#include <string.h>
#include "esp_log.h"
#include "services/gap/ble_svc_gap.h"

static const char* TAG = "BleCentral";

static constexpr uint32_t BLE_DISCONNECT_WAIT_MS = 500;
static constexpr int32_t BLE_CONNECT_TIMEOUT_MS = 10000;
// Pairing and the GATT discovery have to finish within this time
static constexpr uint32_t BLE_SETUP_TIMEOUT_MS = 20000;
static constexpr uint32_t BLE_DISCONNECT_TIMEOUT_MS = 5000;
// How long to wait for the scan response of a matching advertiser which sent
// no name, before taking it as it is
static constexpr uint32_t BLE_SCAN_RSP_WAIT_MS = 400;

// Advertising data types
static constexpr uint8_t AD_UUIDS16_INCOMPLETE = 0x02;
static constexpr uint8_t AD_UUIDS16_COMPLETE = 0x03;
static constexpr uint8_t AD_NAME_SHORT = 0x08;
static constexpr uint8_t AD_NAME_COMPLETE = 0x09;

static bool addrEqual(const ble_addr_t& a, const ble_addr_t& b)
{
  return a.type == b.type && memcmp(a.val, b.val, sizeof(a.val)) == 0;
}

// Walk the length/type/value structures of advertising data. The callback
// returns true to stop.
template <typename F>
static void forEachAdStructure(const uint8_t* data, size_t length, F callback)
{
  size_t pos = 0;
  while (data && pos + 1 < length)
  {
    size_t structLength = data[pos];
    if (structLength == 0 || pos + 1 + structLength > length) return;
    if (callback(data[pos + 1], data + pos + 2, structLength - 1)) return;
    pos += 1 + structLength;
  }
}

static bool segmentHasService16(const uint8_t* data, size_t length, uint16_t uuid)
{
  bool found = false;
  forEachAdStructure(data, length, [&](uint8_t type, const uint8_t* value, size_t valueLength) {
    if (type != AD_UUIDS16_INCOMPLETE && type != AD_UUIDS16_COMPLETE) return false;
    for (size_t i = 0; i + 1 < valueLength; i += 2)
      if ((uint16_t)(value[i] | (value[i + 1] << 8)) == uuid) found = true;
    return found;
  });
  return found;
}

bool BleAdvInfo::hasService16(uint16_t uuid) const
{
  return segmentHasService16(data, length, uuid) || segmentHasService16(scanRsp, scanRspLength, uuid);
}

size_t BleAdvInfo::name(char* out, size_t outSize) const
{
  if (outSize == 0) return 0;
  out[0] = 0;

  // Prefer the complete name over the shortened one
  const uint8_t* best = nullptr;
  size_t bestLength = 0;
  bool bestComplete = false;
  auto scan = [&](const uint8_t* segment, size_t segmentLength) {
    forEachAdStructure(segment, segmentLength, [&](uint8_t type, const uint8_t* value, size_t valueLength) {
      if (type != AD_NAME_SHORT && type != AD_NAME_COMPLETE) return false;
      if (best && bestComplete) return false;
      best = value;
      bestLength = valueLength;
      bestComplete = type == AD_NAME_COMPLETE;
      return false;
    });
  };
  scan(data, length);
  scan(scanRsp, scanRspLength);

  if (!best) return 0;
  size_t n = bestLength < outSize - 1 ? bestLength : outSize - 1;
  memcpy(out, best, n);
  out[n] = 0;
  return n;
}

//
// State handling
//

bool BleCentral::transition(State from, State to)
{
  if (!state_.compare_exchange_strong(from, to)) return false;
  stateSinceMs_ = nowMs();
  return true;
}

void BleCentral::begin(const char* deviceName)
{
  if (state_ != State::Idle) return;
  if (!bleStackIsReady())
  {
    ESP_LOGE(TAG, "BLE stack is not ready");
    return;
  }

  if (deviceName && *deviceName)
    ble_svc_gap_device_name_set(deviceName);

  uint16_t mtu = preferredMtu();
  if (mtu) ble_att_set_preferred_mtu(mtu);
  configureSecurity();

  clearPeer();
  connHandle_ = BLE_HS_CONN_HANDLE_NONE;
  scanAttempts = 0;
  transition(State::Idle, State::Started);
  enterScanning();
}

void BleCentral::end()
{
  if (state_ == State::Idle) return;

  state_ = State::Stopping;
  stopScan();

  // Abort a connection attempt which is under way
  if (ble_gap_conn_active())
  {
    ble_gap_conn_cancel();
    for (uint32_t waited = 0; ble_gap_conn_active() && waited < BLE_DISCONNECT_WAIT_MS; waited += 10)
      delayMs(10);
  }

  disconnectAndWait();
  resetConnectedPeerState();
  clearPeer();
  scanAttempts = 0;
  state_ = State::Idle;
}

void BleCentral::loop()
{
  switch (state_.load())
  {
    case State::PendingScan:
      enterScanning(scanDuration);
      return;

    case State::Scanning:
      checkCandidateTimeout();
      return;

    case State::PendingConnect:
      connectToPeer();
      return;

    case State::SettingUp:
      if ((uint32_t)(nowMs() - stateSinceMs_) > BLE_SETUP_TIMEOUT_MS)
      {
        ESP_LOGW(TAG, "Peer setup timed out");
        setupFinished(false);
      }
      return;

    case State::Disconnecting:
      if ((uint32_t)(nowMs() - stateSinceMs_) > BLE_DISCONNECT_TIMEOUT_MS)
      {
        ESP_LOGW(TAG, "Disconnect timed out");
        connHandle_ = BLE_HS_CONN_HANDLE_NONE;
        recoverFromDisconnect();
      }
      return;

    case State::Idle:
    case State::Started:
    case State::Connecting:
    case State::Connected:
    case State::Stopping:
      return;
  }
}

bool BleCentral::isStarted() const
{
  return state_ != State::Idle;
}

bool BleCentral::isConnected() const
{
  return state_ == State::Connected && connHandle_ != BLE_HS_CONN_HANDLE_NONE;
}

bool BleCentral::isConnectPending() const
{
  return state_ == State::PendingConnect;
}

const char* BleCentral::peerName() const
{
  return peerName_[0] ? peerName_ : nullptr;
}

//
// Scanning
//

void BleCentral::enterScanning(uint32_t seconds)
{
  State current = state_;
  if (current != State::Started && current != State::PendingScan) return;
  if (!transition(current, State::Scanning)) return;

  if (!beginScan(seconds))
    transition(State::Scanning, State::Started);
}

bool BleCentral::beginScan(uint32_t seconds)
{
  State current = state_;
  if (current == State::Idle || current == State::Stopping) return false;
  if (isConnected()) return false;
  if (MAX_SCAN_ATTEMPTS && scanAttempts >= MAX_SCAN_ATTEMPTS) return false;
  if (!bleStackIsReady()) return false;

  struct ble_gap_disc_params params;
  memset(&params, 0, sizeof(params));
  params.filter_duplicates = 1;
  configureScan(params);
  scanDuration = seconds;
  ++scanAttempts;
  onScanStart();

  {
    std::lock_guard<std::mutex> lock(scanMutex);
    candidatePending_ = false;
    lastAdvValid_ = false;
  }

  int rc = ble_gap_disc(bleStackOwnAddrType(), (int32_t)(seconds * 1000), &params, discEventCallback, this);
  if (rc != 0)
  {
    ESP_LOGW(TAG, "Starting scan failed, rc=%d", rc);
    return false;
  }
  return true;
}

void BleCentral::stopScan()
{
  if (!bleStackIsReady()) return;
  // Reports no completion event, the caller moves on to the next state
  ble_gap_disc_cancel();
}

int BleCentral::discEventCallback(struct ble_gap_event* event, void* arg)
{
  return static_cast<BleCentral*>(arg)->handleDiscEvent(event);
}

int BleCentral::handleDiscEvent(struct ble_gap_event* event)
{
  std::lock_guard<std::mutex> lock(scanMutex);
  if (state_ != State::Scanning) return 0;

  switch (event->type)
  {
    case BLE_GAP_EVENT_DISC:
    {
      const struct ble_gap_disc_desc& report = event->disc;
      char name[sizeof(peerName_)];

      if (report.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP)
      {
        // The advertiser which matched is still missing its name
        if (candidatePending_ && addrEqual(report.addr, candidateAddr_))
        {
          BleAdvInfo rsp;
          rsp.data = report.data;
          rsp.length = report.length_data;
          size_t nameLength = rsp.name(name, sizeof(name));
          acceptPeer(candidateAddr_, name, nameLength);
        }
        // The advertiser did not match on its own, the scan response may
        // list the missing service
        else if (lastAdvValid_ && addrEqual(report.addr, lastAdvAddr_))
        {
          lastAdvValid_ = false;
          BleAdvInfo info;
          info.data = lastAdv_;
          info.length = lastAdvLength_;
          info.scanRsp = report.data;
          info.scanRspLength = report.length_data;
          if (acceptsAdvertisement(info))
            acceptPeer(lastAdvAddr_, name, info.name(name, sizeof(name)));
        }
        return 0;
      }

      bool connectable = report.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                         report.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
      if (candidatePending_)
      {
        // Do not wait forever for a scan response which may never come
        if ((uint32_t)(nowMs() - candidateSinceMs_) > BLE_SCAN_RSP_WAIT_MS)
          acceptPeer(candidateAddr_, "", 0);
        return 0;
      }
      if (!connectable) return 0;

      BleAdvInfo info;
      info.data = report.data;
      info.length = report.length_data;
      if (acceptsAdvertisement(info))
      {
        size_t nameLength = info.name(name, sizeof(name));
        if (nameLength || report.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND)
          acceptPeer(report.addr, name, nameLength);
        else
        {
          // Active scanning: the name is probably in the scan response
          candidatePending_ = true;
          candidateSinceMs_ = nowMs();
          candidateAddr_ = report.addr;
        }
      }
      else
      {
        lastAdvLength_ = report.length_data < sizeof(lastAdv_) ? report.length_data : sizeof(lastAdv_);
        memcpy(lastAdv_, report.data, lastAdvLength_);
        lastAdvAddr_ = report.addr;
        lastAdvValid_ = true;
      }
      return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
      if (candidatePending_)
        acceptPeer(candidateAddr_, "", 0);
      else
        transition(State::Scanning, State::PendingScan);
      return 0;

    default:
      return 0;
  }
}

// Called with scanMutex held
void BleCentral::acceptPeer(const ble_addr_t& addr, const char* name, size_t nameLength)
{
  peerAddr_ = addr;
  size_t n = nameLength < sizeof(peerName_) - 1 ? nameLength : sizeof(peerName_) - 1;
  memcpy(peerName_, name, n);
  peerName_[n] = 0;
  havePeer_ = true;
  candidatePending_ = false;
  lastAdvValid_ = false;

  stopScan();
  transition(State::Scanning, State::PendingConnect);
}

void BleCentral::checkCandidateTimeout()
{
  std::lock_guard<std::mutex> lock(scanMutex);
  if (candidatePending_ && state_ == State::Scanning &&
      (uint32_t)(nowMs() - candidateSinceMs_) > BLE_SCAN_RSP_WAIT_MS)
    acceptPeer(candidateAddr_, "", 0);
}

void BleCentral::clearPeer()
{
  std::lock_guard<std::mutex> lock(scanMutex);
  havePeer_ = false;
  peerName_[0] = 0;
  candidatePending_ = false;
  lastAdvValid_ = false;
}

//
// Connecting
//

void BleCentral::connectToPeer()
{
  if (!transition(State::PendingConnect, State::Connecting)) return;

  int rc = BLE_HS_ENOENT;
  if (havePeer_)
  {
    rc = ble_gap_connect(bleStackOwnAddrType(), &peerAddr_, BLE_CONNECT_TIMEOUT_MS, nullptr, connEventCallback, this);
    if (rc != 0)
      ESP_LOGW(TAG, "Connect failed, rc=%d", rc);
  }

  if (rc != 0)
  {
    clearPeer();
    onConnectFailed();
    transition(State::Connecting, State::PendingScan);
  }
}

int BleCentral::connEventCallback(struct ble_gap_event* event, void* arg)
{
  return static_cast<BleCentral*>(arg)->handleConnEvent(event);
}

int BleCentral::handleConnEvent(struct ble_gap_event* event)
{
  switch (event->type)
  {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0)
      {
        uint16_t handle = event->connect.conn_handle;
        connHandle_ = handle;
        if (!transition(State::Connecting, State::SettingUp))
        {
          // Stopping, nobody is interested anymore
          ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
          return 0;
        }
        scanAttempts = 0;
        beginSetup();
      }
      else if (state_ == State::Connecting)
      {
        ESP_LOGW(TAG, "Connect failed, status=%d", event->connect.status);
        clearPeer();
        onConnectFailed();
        transition(State::Connecting, State::PendingScan);
      }
      return 0;

    case BLE_GAP_EVENT_DISCONNECT:
    {
      if (event->disconnect.conn.conn_handle != connHandle_) return 0;
      connHandle_ = BLE_HS_CONN_HANDLE_NONE;
      State current = state_;
      if (current == State::SettingUp || current == State::Connected || current == State::Disconnecting)
        recoverFromDisconnect();
      return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
      handleEncChange(event->enc_change.conn_handle, event->enc_change.status);
      return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
    {
      State current = state_;
      if (event->notify_rx.conn_handle != connHandle_ ||
          (current != State::SettingUp && current != State::Connected))
        return 0;

      uint8_t buffer[64];
      uint16_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
      if (length > sizeof(buffer)) length = sizeof(buffer);
      if (os_mbuf_copydata(event->notify_rx.om, 0, length, buffer) == 0)
        onNotify(event->notify_rx.attr_handle, buffer, length);
      return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
    {
      // The peer lost its keys, drop the old bond and pair again
      struct ble_gap_conn_desc desc;
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
        ble_store_util_delete_peer(&desc.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
      // We have no display or keyboard so this is not expected, pairing
      // fails by timeout
      ESP_LOGW(TAG, "Passkey action %d not supported", event->passkey.params.action);
      return 0;

    default:
      return 0;
  }
}

//
// Setup of the connected peer
//

void BleCentral::beginSetup()
{
  securityTried_ = false;
  bondRetried_ = false;
  awaitingSecurity_ = false;

  uint16_t handle = connHandle_;
  // Not fatal if it fails, e.g. when the peer started the exchange already
  if (ble_gattc_exchange_mtu(handle, mtuCallback, this) != 0)
    startPeerSetup(handle);
}

int BleCentral::mtuCallback(uint16_t connHandle, const struct ble_gatt_error* error, uint16_t mtu, void* arg)
{
  (void)error;
  (void)mtu;
  BleCentral* self = static_cast<BleCentral*>(arg);
  if (self->state_ == State::SettingUp && connHandle == self->connHandle_)
    self->startPeerSetup(connHandle);
  return 0;
}

void BleCentral::setupFinished(bool ok)
{
  if (ok)
  {
    transition(State::SettingUp, State::Connected);
    return;
  }

  if (!transition(State::SettingUp, State::Disconnecting)) return;
  disconnectPeer();
}

bool BleCentral::retryAfterSecurity(int gattStatus)
{
  switch (gattStatus)
  {
    case BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN):
    case BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC):
    case BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_KEY_SZ):
    case BLE_HS_EAUTHEN:
    case BLE_HS_EENCRYPT:
    case BLE_HS_EENCRYPT_KEY_SZ:
      break;
    default:
      return false;
  }

  if (securityTried_ || state_ != State::SettingUp) return false;
  securityTried_ = true;

  uint16_t handle = connHandle_;
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(handle, &desc) == 0 && desc.sec_state.encrypted)
  {
    // The link got encrypted in the meantime, e.g. at the request of the peer
    resumeSetup();
    return true;
  }

  awaitingSecurity_ = true;
  int rc = ble_gap_security_initiate(handle);
  // Already in progress, e.g. started by the peer, is fine
  if (rc != 0 && rc != BLE_HS_EALREADY)
  {
    ESP_LOGW(TAG, "Security initiate failed, rc=%d", rc);
    awaitingSecurity_ = false;
    return false;
  }
  return true;
}

void BleCentral::handleEncChange(uint16_t handle, int status)
{
  ESP_LOGI(TAG, "Encryption change, status=%d", status);
  if (state_ != State::SettingUp || handle != connHandle_ || !awaitingSecurity_) return;

  if (status == 0)
  {
    awaitingSecurity_ = false;
    resumeSetup();
    return;
  }

  // The stored keys may be stale, e.g. the peer was reset. Forget the bond
  // and pair again, once.
  if (!bondRetried_)
  {
    bondRetried_ = true;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(handle, &desc) == 0)
    {
      ble_store_util_delete_peer(&desc.peer_id_addr);
      int rc = ble_gap_security_initiate(handle);
      if (rc == 0 || rc == BLE_HS_EALREADY) return;
    }
  }

  awaitingSecurity_ = false;
  setupFinished(false);
}

//
// Disconnecting
//

void BleCentral::disconnectPeer()
{
  uint16_t handle = connHandle_;
  if (handle == BLE_HS_CONN_HANDLE_NONE) return;

  int rc = ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
  if (rc != 0 && rc != BLE_HS_EALREADY)
    ESP_LOGW(TAG, "Terminate failed, rc=%d", rc);
}

void BleCentral::disconnectAndWait()
{
  if (connHandle_ == BLE_HS_CONN_HANDLE_NONE) return;

  disconnectPeer();
  for (uint32_t waited = 0; connHandle_ != BLE_HS_CONN_HANDLE_NONE && waited < BLE_DISCONNECT_WAIT_MS; waited += 10)
    delayMs(10);
  if (connHandle_ != BLE_HS_CONN_HANDLE_NONE)
  {
    ESP_LOGW(TAG, "Peer did not disconnect in time");
    connHandle_ = BLE_HS_CONN_HANDLE_NONE;
  }
}

void BleCentral::recoverFromDisconnect()
{
  resetConnectedPeerState();
  clearPeer();
  scanAttempts = 0;

  State current = state_;
  if (current == State::Stopping || current == State::Idle) return;

  transition(current, State::PendingScan);
}
