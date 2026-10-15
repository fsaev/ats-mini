#ifndef BLE_HID_CENTRAL_H
#define BLE_HID_CENTRAL_H

#include <mutex>
#include "BleCentral.h"

// Scan interval and window in milliseconds
#define BLE_SCAN_INTERVAL 100
#define BLE_SCAN_WINDOW 100

struct BleHidState {
  bool isPressed = false;
  int16_t rotation = 0;
  bool wasClicked = false;
  bool wasShortPressed = false;
};

//
// Central which connects to a known BLE HID remote (a keyboard or a knob),
// subscribes to its consumer control input report and turns the reports into
// rotation and clicks.
//
// The reports arrive in the NimBLE host task, update() and
// consumeAbortPending() are called from the application task.
//
class BleHidCentral : public BleCentral {
public:
  BleHidCentral() = default;

  BleHidState update();
  bool consumeAbortPending();

protected:
  void configureSecurity() override;
  void configureScan(struct ble_gap_disc_params& params) override;
  uint16_t preferredMtu() const override;
  bool acceptsAdvertisement(const BleAdvInfo& adv) override;
  void startPeerSetup(uint16_t connHandle) override;
  void resumeSetup() override;
  void resetConnectedPeerState() override;
  void onNotify(uint16_t attrHandle, const uint8_t* data, size_t length) override;
  void onScanStart() override;

private:
  enum class DecoderKind : uint8_t {
    None,
    ConsumerBitfield16,
    ConsumerUsage16,
  };

  enum PressBit : uint8_t {
    ScanNextPressed = 1 << 0,
    ScanPreviousPressed = 1 << 1,
    VolumeIncrementPressed = 1 << 2,
    VolumeDecrementPressed = 1 << 3,
    PlayPausePressed = 1 << 4,
  };

  // Steps of the GATT setup, each one is a request to the peer
  enum class SetupStep : uint8_t {
    DiscoverServices,
    ReadPnpId,
    DiscoverCharacteristics,
    DiscoverDescriptors,
    WriteCccd,
  };

  struct KnownDevice;
  static const KnownDevice* knownDevices();

  static constexpr uint32_t virtualPushHoldMs = 150;
  static constexpr uint32_t playPauseDoubleClickMs = 400;
  static constexpr uint32_t keyboardPressMinMs = 50;
  static constexpr uint32_t keyboardLongPressMs = 500;

  static int serviceCallback(uint16_t connHandle, const struct ble_gatt_error* error,
                             const struct ble_gatt_svc* service, void* arg);
  static int pnpIdCallback(uint16_t connHandle, const struct ble_gatt_error* error,
                           struct ble_gatt_attr* attr, void* arg);
  static int characteristicCallback(uint16_t connHandle, const struct ble_gatt_error* error,
                                    const struct ble_gatt_chr* chr, void* arg);
  static int descriptorCallback(uint16_t connHandle, const struct ble_gatt_error* error,
                                uint16_t chrValHandle, const struct ble_gatt_dsc* dsc, void* arg);
  static int cccdWriteCallback(uint16_t connHandle, const struct ble_gatt_error* error,
                               struct ble_gatt_attr* attr, void* arg);

  void runSetupStep();
  void failSetup(const char* reason);
  // Common handling of a failed request: pair and retry if the peer asks
  // for security, give up otherwise. Returns false if the setup was aborted.
  bool handleRequestError(const struct ble_gatt_error* error, const char* what);
  bool matchPnpId(const uint8_t* value, size_t length);
  void clearReportBinding();
  void handleInputReport(uint16_t attrHandle, const uint8_t* data, size_t length);
  void holdVirtualPush();

  // Setup progress, host task only
  SetupStep step_ = SetupStep::DiscoverServices;
  uint16_t disStart_ = 0;
  uint16_t disEnd_ = 0;
  uint16_t hidStart_ = 0;
  uint16_t hidEnd_ = 0;
  const KnownDevice* matched_ = nullptr;
  bool reportFound_ = false;
  uint16_t reportDescEnd_ = 0;
  uint16_t cccdHandle_ = 0;

  // Input handling. Written by the host task when a report arrives and by
  // the application task in update(), so everything below needs the mutex.
  std::mutex inputMutex;
  BleHidState pendingState{};
  bool abortPending = false;
  DecoderKind decoder_ = DecoderKind::None;
  uint16_t reportHandle_ = 0;
  uint32_t virtualPushUntil = 0;
  uint32_t playPauseClickDeadline = 0;
  uint32_t playPausePressedAt = 0;
  bool ignoreNextPlayPauseRelease = false;
  bool supportsDoubleClick_ = true;
  uint8_t pressedMask_ = 0;
};

#endif
