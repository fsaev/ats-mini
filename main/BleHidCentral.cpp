#include "BleHidCentral.h"
#include "Draw.h"
#include "Platform.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char* TAG = "BleHid";

static constexpr uint16_t hidServiceUuid16 = 0x1812;
static constexpr uint16_t deviceInfoServiceUuid16 = 0x180A;
static constexpr uint16_t reportCharUuid16 = 0x2A4D;
static constexpr uint16_t pnpIdUuid16 = 0x2A50;
static constexpr uint16_t cccdUuid16 = 0x2902;

static const ble_uuid16_t pnpIdUuid = BLE_UUID16_INIT(pnpIdUuid16);

static constexpr uint16_t consumerUsagePlayPause = 0x00CD;
static constexpr uint16_t consumerUsageScanNextTrack = 0x00B5;
static constexpr uint16_t consumerUsageScanPreviousTrack = 0x00B6;
static constexpr uint16_t consumerUsageVolumeIncrement = 0x00E9;
static constexpr uint16_t consumerUsageVolumeDecrement = 0x00EA;

static constexpr uint16_t consumerBitfield16VolumeDecrement = 1u << 0;
static constexpr uint16_t consumerBitfield16VolumeIncrement = 1u << 1;
static constexpr uint16_t consumerBitfield16ScanPreviousTrack = 1u << 3;
static constexpr uint16_t consumerBitfield16ScanNextTrack = 1u << 4;
static constexpr uint16_t consumerBitfield16PlayPause = 1u << 5;

// The remotes we know how to talk to. They are recognised by the PnP ID in
// their Device Information service. The report handle is the value handle of
// the consumer control input report characteristic in the HID service.
//
// Add a new supported device by adding a line here.
struct BleHidCentral::KnownDevice {
  const char* name;
  uint8_t pnpId[7];
  DecoderKind decoder;
  uint16_t reportHandle;
  bool supportsDoubleClick;
};

static constexpr size_t knownDeviceCount = 2;

namespace {

static bool decodeConsumerBitfield16(
  const uint8_t* data,
  size_t length,
  bool& hasScanNext,
  bool& hasScanPrevious,
  bool& hasVolumeIncrement,
  bool& hasVolumeDecrement,
  bool& hasPlayPause)
{
  if (data == nullptr || length < 2) return false;

  uint16_t bits = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
  hasScanNext = (bits & consumerBitfield16ScanNextTrack) != 0;
  hasScanPrevious = (bits & consumerBitfield16ScanPreviousTrack) != 0;
  hasVolumeIncrement = (bits & consumerBitfield16VolumeIncrement) != 0;
  hasVolumeDecrement = (bits & consumerBitfield16VolumeDecrement) != 0;
  hasPlayPause = (bits & consumerBitfield16PlayPause) != 0;
  return true;
}

static bool decodeConsumerUsage16(
  const uint8_t* data,
  size_t length,
  bool& hasScanNext,
  bool& hasScanPrevious,
  bool& hasVolumeIncrement,
  bool& hasVolumeDecrement,
  bool& hasPlayPause)
{
  if (data == nullptr || length != 2) return false;

  uint16_t usage = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
  hasScanNext = usage == consumerUsageScanNextTrack;
  hasScanPrevious = usage == consumerUsageScanPreviousTrack;
  hasVolumeIncrement = usage == consumerUsageVolumeIncrement;
  hasVolumeDecrement = usage == consumerUsageVolumeDecrement;
  hasPlayPause = usage == consumerUsagePlayPause;
  return true;
}

}  // namespace

const BleHidCentral::KnownDevice* BleHidCentral::knownDevices()
{
  static const KnownDevice devices[knownDeviceCount] = {
    // MINI_KEYBOARD
    { "MINI_KEYBOARD", {0x02, 0xAC, 0x05, 0x2C, 0x02, 0x1B, 0x01},
      DecoderKind::ConsumerUsage16, 50, true },
    // VOL20
    { "VOL20", {0x01, 0xD7, 0x07, 0x00, 0x00, 0x10, 0x01},
      DecoderKind::ConsumerBitfield16, 56, false },
  };
  return devices;
}

//
// Input handling
//

BleHidState BleHidCentral::update()
{
  std::lock_guard<std::mutex> lock(inputMutex);

  if (!pendingState.rotation && !pendingState.wasClicked && !pendingState.wasShortPressed &&
      playPauseClickDeadline && (int32_t)(nowMs() - playPauseClickDeadline) >= 0)
  {
    pendingState.wasClicked = true;
    playPauseClickDeadline = 0;
  }

  pendingState.isPressed =
    (pressedMask_ & (ScanNextPressed | ScanPreviousPressed)) ||
    (virtualPushUntil && (int32_t)(virtualPushUntil - nowMs()) > 0);
  BleHidState result = pendingState;
  pendingState = {};
  abortPending = false;
  return result;
}

bool BleHidCentral::consumeAbortPending()
{
  std::lock_guard<std::mutex> lock(inputMutex);

  bool pending = abortPending;
  abortPending = false;
  if (pending)
  {
    pendingState = {};
    virtualPushUntil = 0;
    playPauseClickDeadline = 0;
    playPausePressedAt = 0;
    ignoreNextPlayPauseRelease = false;
    pressedMask_ = 0;
  }
  return pending;
}

void BleHidCentral::onNotify(uint16_t attrHandle, const uint8_t* data, size_t length)
{
  handleInputReport(attrHandle, data, length);
}

void BleHidCentral::handleInputReport(uint16_t attrHandle, const uint8_t* data, size_t length)
{
  bool hasScanNext = false;
  bool hasScanPrevious = false;
  bool hasVolumeIncrement = false;
  bool hasVolumeDecrement = false;
  bool hasPlayPause = false;

  std::lock_guard<std::mutex> lock(inputMutex);

  if (attrHandle != reportHandle_) return;

  bool decoded = false;
  switch (decoder_)
  {
    case DecoderKind::ConsumerBitfield16:
      decoded = decodeConsumerBitfield16(
        data,
        length,
        hasScanNext,
        hasScanPrevious,
        hasVolumeIncrement,
        hasVolumeDecrement,
        hasPlayPause);
      break;

    case DecoderKind::ConsumerUsage16:
      decoded = decodeConsumerUsage16(
        data,
        length,
        hasScanNext,
        hasScanPrevious,
        hasVolumeIncrement,
        hasVolumeDecrement,
        hasPlayPause);
      break;

    case DecoderKind::None:
      return;
  }

  if (!decoded) return;
  abortPending = true;

  bool volumeIncrementPressed = !!(pressedMask_ & VolumeIncrementPressed);
  bool volumeDecrementPressed = !!(pressedMask_ & VolumeDecrementPressed);
  bool scanNextPressed = !!(pressedMask_ & ScanNextPressed);
  bool scanPreviousPressed = !!(pressedMask_ & ScanPreviousPressed);
  bool playPausePressed = !!(pressedMask_ & PlayPausePressed);
  uint32_t now = nowMs();
  bool isReleaseReport =
    !hasScanNext &&
    !hasScanPrevious &&
    !hasVolumeIncrement &&
    !hasVolumeDecrement &&
    !hasPlayPause;

  if (hasPlayPause && !playPausePressed)
  {
    playPausePressedAt = now;
    ignoreNextPlayPauseRelease = false;
  }

  if (playPausePressed && !isReleaseReport)
    hasPlayPause = true;

  if (hasVolumeIncrement && !volumeIncrementPressed && pendingState.rotation < 32767)
  {
    pendingState.rotation++;
    if (playPausePressed)
    {
      holdVirtualPush();
      ignoreNextPlayPauseRelease = true;
    }
  }

  if (hasVolumeDecrement && !volumeDecrementPressed && pendingState.rotation > -32768)
  {
    pendingState.rotation--;
    if (playPausePressed)
    {
      holdVirtualPush();
      ignoreNextPlayPauseRelease = true;
    }
  }

  if (hasScanNext && !scanNextPressed && pendingState.rotation < 32767)
  {
    pendingState.rotation++;
    holdVirtualPush();
  }

  if (hasScanPrevious && !scanPreviousPressed && pendingState.rotation > -32768)
  {
    pendingState.rotation--;
    holdVirtualPush();
  }

  if (isReleaseReport && playPausePressed)
  {
    uint32_t heldMs = playPausePressedAt ? (uint32_t)(now - playPausePressedAt) : 0;

    if (ignoreNextPlayPauseRelease)
    {
      ignoreNextPlayPauseRelease = false;
      hasPlayPause = true;
    }
    else if (heldMs >= keyboardLongPressMs)
    {
      playPausePressedAt = 0;
      pendingState.wasShortPressed = true;
      playPauseClickDeadline = 0;
    }
    else if (heldMs >= keyboardPressMinMs)
    {
      playPausePressedAt = 0;
      pendingState.wasClicked = true;
      playPauseClickDeadline = 0;
    }
    else if (supportsDoubleClick_ && playPauseClickDeadline && (int32_t)(now - playPauseClickDeadline) < 0)
    {
      playPausePressedAt = 0;
      pendingState.wasShortPressed = true;
      playPauseClickDeadline = 0;
    }
    else if (supportsDoubleClick_)
    {
      playPausePressedAt = 0;
      playPauseClickDeadline = now + playPauseDoubleClickMs;
    }
    else
    {
      playPausePressedAt = 0;
      pendingState.wasClicked = true;
      playPauseClickDeadline = 0;
    }
  }

  auto setPressed = [this](uint8_t bit, bool isPressed) {
    if (isPressed)
      pressedMask_ |= bit;
    else
      pressedMask_ &= ~bit;
  };

  setPressed(VolumeIncrementPressed, hasVolumeIncrement);
  setPressed(VolumeDecrementPressed, hasVolumeDecrement);
  setPressed(ScanNextPressed, hasScanNext);
  setPressed(ScanPreviousPressed, hasScanPrevious);
  setPressed(PlayPausePressed, hasPlayPause);
}

// Called with inputMutex held
void BleHidCentral::holdVirtualPush()
{
  virtualPushUntil = nowMs() + virtualPushHoldMs;
}

//
// Central configuration
//

void BleHidCentral::configureSecurity()
{
  // Bonding with Secure Connections, Just Works as we have no input or output
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  // Pairing is started only when the peer refuses a request for lack of security
}

void BleHidCentral::configureScan(struct ble_gap_disc_params& params)
{
  // The parameters are in units of 0.625 ms
  params.itvl = BLE_SCAN_INTERVAL * 1000 / 625;
  params.window = BLE_SCAN_WINDOW * 1000 / 625;
  params.passive = 0;
}

uint16_t BleHidCentral::preferredMtu() const
{
  return 185;
}

void BleHidCentral::onScanStart()
{
  char statusLine[40];
  uint8_t maxAttempts = MAX_SCAN_ATTEMPTS;

  drawScreen();
  if (maxAttempts)
  {
    snprintf(statusLine, sizeof(statusLine), "Scanning for BLE HID %u/%u...", (unsigned)scanAttempts.load(), (unsigned)maxAttempts);
    drawScreen(statusLine);
  }
  else
    drawScreen("Scanning for BLE HID...");

  delayMs(500);
}

bool BleHidCentral::acceptsAdvertisement(const BleAdvInfo& adv)
{
  return adv.hasService16(hidServiceUuid16);
}

//
// Setup of the connected remote, one request after the other:
//   1. discover the services, locate Device Information and HID
//   2. read the PnP ID and recognise the remote
//   3. discover the characteristics of the HID service, locate the report
//   4. discover the descriptors of the report, locate the CCCD
//   5. write the CCCD to enable the notifications
// A request refused for lack of security makes the central pair, then the
// request is repeated, see BleCentral::retryAfterSecurity().
//

void BleHidCentral::startPeerSetup(uint16_t connHandle)
{
  (void)connHandle;
  clearReportBinding();
  step_ = SetupStep::DiscoverServices;
  runSetupStep();
}

void BleHidCentral::resumeSetup()
{
  runSetupStep();
}

void BleHidCentral::runSetupStep()
{
  uint16_t handle = connHandle();
  int rc = BLE_HS_EUNKNOWN;

  switch (step_)
  {
    case SetupStep::DiscoverServices:
      disStart_ = disEnd_ = hidStart_ = hidEnd_ = 0;
      rc = ble_gattc_disc_all_svcs(handle, serviceCallback, this);
      break;

    case SetupStep::ReadPnpId:
      matched_ = nullptr;
      rc = ble_gattc_read_by_uuid(handle, disStart_, disEnd_, &pnpIdUuid.u, pnpIdCallback, this);
      break;

    case SetupStep::DiscoverCharacteristics:
      reportFound_ = false;
      reportDescEnd_ = 0;
      rc = ble_gattc_disc_all_chrs(handle, hidStart_, hidEnd_, characteristicCallback, this);
      break;

    case SetupStep::DiscoverDescriptors:
      cccdHandle_ = 0;
      rc = ble_gattc_disc_all_dscs(handle, matched_->reportHandle, reportDescEnd_, descriptorCallback, this);
      break;

    case SetupStep::WriteCccd:
    {
      static const uint8_t enableNotifications[2] = {0x01, 0x00};
      rc = ble_gattc_write_flat(handle, cccdHandle_, enableNotifications, sizeof(enableNotifications), cccdWriteCallback, this);
      break;
    }
  }

  if (rc != 0)
  {
    ESP_LOGW(TAG, "GATT request for step %d failed, rc=%d", (int)step_, rc);
    failSetup("request failed");
  }
}

void BleHidCentral::failSetup(const char* reason)
{
  ESP_LOGW(TAG, "Peer setup failed: %s", reason);
  clearReportBinding();
  setupFinished(false);
}

bool BleHidCentral::handleRequestError(const struct ble_gatt_error* error, const char* what)
{
  if (retryAfterSecurity(error->status))
    return true;

  ESP_LOGW(TAG, "%s failed, status=%d", what, error->status);
  failSetup(what);
  return false;
}

int BleHidCentral::serviceCallback(uint16_t connHandle, const struct ble_gatt_error* error,
                                   const struct ble_gatt_svc* service, void* arg)
{
  BleHidCentral* self = static_cast<BleHidCentral*>(arg);
  if (connHandle != self->connHandle() || self->step_ != SetupStep::DiscoverServices) return 0;

  if (error->status == 0)
  {
    uint16_t uuid = service ? ble_uuid_u16(&service->uuid.u) : 0;
    if (uuid == deviceInfoServiceUuid16)
    {
      self->disStart_ = service->start_handle;
      self->disEnd_ = service->end_handle;
    }
    else if (uuid == hidServiceUuid16)
    {
      self->hidStart_ = service->start_handle;
      self->hidEnd_ = service->end_handle;
    }
  }
  else if (error->status == BLE_HS_EDONE)
  {
    if (self->disStart_ && self->hidStart_)
    {
      self->step_ = SetupStep::ReadPnpId;
      self->runSetupStep();
    }
    else
      self->failSetup("Device Information or HID service missing");
  }
  else
    self->handleRequestError(error, "Service discovery");

  return 0;
}

bool BleHidCentral::matchPnpId(const uint8_t* value, size_t length)
{
  const KnownDevice* devices = knownDevices();
  for (size_t i = 0; i < knownDeviceCount; i++)
  {
    if (length == sizeof(devices[i].pnpId) && memcmp(value, devices[i].pnpId, length) == 0)
    {
      matched_ = &devices[i];
      return true;
    }
  }
  return false;
}

int BleHidCentral::pnpIdCallback(uint16_t connHandle, const struct ble_gatt_error* error,
                                 struct ble_gatt_attr* attr, void* arg)
{
  BleHidCentral* self = static_cast<BleHidCentral*>(arg);
  if (connHandle != self->connHandle() || self->step_ != SetupStep::ReadPnpId) return 0;

  if (error->status == 0)
  {
    uint8_t value[16];
    if (attr && attr->om)
    {
      uint16_t length = OS_MBUF_PKTLEN(attr->om);
      if (length <= sizeof(value) && os_mbuf_copydata(attr->om, 0, length, value) == 0)
        self->matchPnpId(value, length);
    }
  }
  else if (error->status == BLE_HS_EDONE)
  {
    if (self->matched_)
    {
      ESP_LOGI(TAG, "Recognised %s", self->matched_->name);
      self->step_ = SetupStep::DiscoverCharacteristics;
      self->runSetupStep();
    }
    else
      self->failSetup("unsupported device");
  }
  else
    self->handleRequestError(error, "PnP ID read");

  return 0;
}

int BleHidCentral::characteristicCallback(uint16_t connHandle, const struct ble_gatt_error* error,
                                          const struct ble_gatt_chr* chr, void* arg)
{
  BleHidCentral* self = static_cast<BleHidCentral*>(arg);
  if (connHandle != self->connHandle() || self->step_ != SetupStep::DiscoverCharacteristics) return 0;

  if (error->status == 0)
  {
    if (self->reportFound_ && self->reportDescEnd_ == 0)
    {
      // The descriptors of the report end where the next characteristic starts
      self->reportDescEnd_ = chr->def_handle - 1;
    }
    else if (!self->reportFound_ &&
             ble_uuid_u16(&chr->uuid.u) == reportCharUuid16 &&
             chr->val_handle == self->matched_->reportHandle &&
             (chr->properties & BLE_GATT_CHR_PROP_NOTIFY))
    {
      self->reportFound_ = true;
    }
  }
  else if (error->status == BLE_HS_EDONE)
  {
    if (!self->reportFound_)
      self->failSetup("input report not found");
    else
    {
      if (self->reportDescEnd_ == 0)
        self->reportDescEnd_ = self->hidEnd_;
      if (self->reportDescEnd_ <= self->matched_->reportHandle)
        self->failSetup("input report has no descriptors");
      else
      {
        self->step_ = SetupStep::DiscoverDescriptors;
        self->runSetupStep();
      }
    }
  }
  else
    self->handleRequestError(error, "Characteristic discovery");

  return 0;
}

int BleHidCentral::descriptorCallback(uint16_t connHandle, const struct ble_gatt_error* error,
                                      uint16_t chrValHandle, const struct ble_gatt_dsc* dsc, void* arg)
{
  (void)chrValHandle;
  BleHidCentral* self = static_cast<BleHidCentral*>(arg);
  if (connHandle != self->connHandle() || self->step_ != SetupStep::DiscoverDescriptors) return 0;

  if (error->status == 0)
  {
    if (dsc && ble_uuid_u16(&dsc->uuid.u) == cccdUuid16)
      self->cccdHandle_ = dsc->handle;
  }
  else if (error->status == BLE_HS_EDONE)
  {
    if (!self->cccdHandle_)
      self->failSetup("CCCD not found");
    else
    {
      // From here on reports may arrive
      {
        std::lock_guard<std::mutex> lock(self->inputMutex);
        self->decoder_ = self->matched_->decoder;
        self->reportHandle_ = self->matched_->reportHandle;
        self->supportsDoubleClick_ = self->matched_->supportsDoubleClick;
      }
      self->step_ = SetupStep::WriteCccd;
      self->runSetupStep();
    }
  }
  else
    self->handleRequestError(error, "Descriptor discovery");

  return 0;
}

int BleHidCentral::cccdWriteCallback(uint16_t connHandle, const struct ble_gatt_error* error,
                                     struct ble_gatt_attr* attr, void* arg)
{
  (void)attr;
  BleHidCentral* self = static_cast<BleHidCentral*>(arg);
  if (connHandle != self->connHandle() || self->step_ != SetupStep::WriteCccd) return 0;

  if (error->status == 0)
    self->setupFinished(true);
  else
    self->handleRequestError(error, "CCCD write");

  return 0;
}

void BleHidCentral::resetConnectedPeerState()
{
  std::lock_guard<std::mutex> lock(inputMutex);

  pendingState = {};
  abortPending = false;
  decoder_ = DecoderKind::None;
  reportHandle_ = 0;
  virtualPushUntil = 0;
  playPauseClickDeadline = 0;
  playPausePressedAt = 0;
  ignoreNextPlayPauseRelease = false;
  supportsDoubleClick_ = true;
  pressedMask_ = 0;
}

void BleHidCentral::clearReportBinding()
{
  std::lock_guard<std::mutex> lock(inputMutex);

  decoder_ = DecoderKind::None;
  reportHandle_ = 0;
}
