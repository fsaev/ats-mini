#ifndef BLE_CENTRAL_H
#define BLE_CENTRAL_H

#include <atomic>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include "host/ble_hs.h"

#define BLE_SCAN_DURATION 10
#define MAX_SCAN_ATTEMPTS 3

// What the scanner knows about an advertiser: the advertising data and, if
// it was received, the scan response data
struct BleAdvInfo {
  const uint8_t* data = nullptr;
  size_t length = 0;
  const uint8_t* scanRsp = nullptr;
  size_t scanRspLength = 0;

  // True if a 16-bit service UUID is listed in the advertised data
  bool hasService16(uint16_t uuid) const;

  // Copy the device name (complete or shortened) as a C string.
  // Returns the length, 0 if there is no name.
  size_t name(char* out, size_t outSize) const;
};

//
// Central role: scan for a peer which the subclass accepts, connect to it,
// let the subclass set it up over GATT and report when it is usable again
// after a disconnect.
//
// The application task calls begin(), end() and loop(). Everything else is
// driven by the NimBLE host task, so the state is atomic and the callbacks
// below run in the host task.
//
class BleCentral {
public:
  BleCentral() = default;
  virtual ~BleCentral() = default;

  void begin(const char* deviceName);
  void end();
  void loop();

  bool isStarted() const;
  bool isConnected() const;
  bool isConnectPending() const;
  const char* peerName() const;

protected:
  virtual void configureSecurity() {}
  virtual void configureScan(struct ble_gap_disc_params& params) { (void)params; }
  // ATT MTU to ask for, 0 for the stack default
  virtual uint16_t preferredMtu() const { return 0; }
  virtual bool acceptsAdvertisement(const BleAdvInfo& adv) = 0;
  // Start the asynchronous GATT setup of the connected peer. Has to end with
  // setupFinished(). May run again after a pairing, see retryAfterSecurity().
  virtual void startPeerSetup(uint16_t connHandle) = 0;
  // Redo the GATT step that failed for lack of security
  virtual void resumeSetup() = 0;
  virtual void resetConnectedPeerState() = 0;
  // A notification or indication from the peer
  virtual void onNotify(uint16_t attrHandle, const uint8_t* data, size_t length)
  { (void)attrHandle; (void)data; (void)length; }

  virtual void onConnectFailed() {}
  virtual void onScanStart() {}

  uint16_t connHandle() const { return connHandle_.load(); }

  // Report the result of the peer setup, from any task
  void setupFinished(bool ok);
  // To be called when a GATT request failed with the given status. If it was
  // refused for lack of security, pairs (or encrypts with the stored keys)
  // and returns true, then resumeSetup() is called once the link is secure.
  bool retryAfterSecurity(int gattStatus);

  std::atomic<uint8_t> scanAttempts{0};
  uint32_t scanDuration = BLE_SCAN_DURATION;

private:
  enum class State : uint8_t {
    Idle,
    Started,
    PendingScan,
    Scanning,
    PendingConnect,
    Connecting,
    SettingUp,
    Connected,
    Disconnecting,
    Stopping,
  };

  static int discEventCallback(struct ble_gap_event* event, void* arg);
  static int connEventCallback(struct ble_gap_event* event, void* arg);
  static int mtuCallback(uint16_t connHandle, const struct ble_gatt_error* error, uint16_t mtu, void* arg);

  int handleDiscEvent(struct ble_gap_event* event);
  int handleConnEvent(struct ble_gap_event* event);
  void handleEncChange(uint16_t connHandle, int status);

  bool transition(State from, State to);
  void enterScanning(uint32_t seconds = BLE_SCAN_DURATION);
  bool beginScan(uint32_t seconds);
  void stopScan();
  void acceptPeer(const ble_addr_t& addr, const char* name, size_t nameLength);
  void checkCandidateTimeout();
  void connectToPeer();
  void beginSetup();
  void recoverFromDisconnect();
  void clearPeer();
  void disconnectPeer();
  void disconnectAndWait();

  std::atomic<State> state_{State::Idle};
  std::atomic<uint16_t> connHandle_{BLE_HS_CONN_HANDLE_NONE};
  std::atomic<uint32_t> stateSinceMs_{0};

  // Peer chosen by the scan, valid from PendingConnect on. Written in the
  // host task while scanning, read in the application task afterwards.
  ble_addr_t peerAddr_{};
  bool havePeer_ = false;
  char peerName_[32] = {};

  // Scan bookkeeping, guarded by scanMutex as it is used by the host task and
  // by loop(). The candidate matched but its name is still to come in the scan
  // response. The last advertisement did not match but the scan response might
  // add the missing service.
  std::mutex scanMutex;
  bool candidatePending_ = false;
  uint32_t candidateSinceMs_ = 0;
  ble_addr_t candidateAddr_{};
  bool lastAdvValid_ = false;
  ble_addr_t lastAdvAddr_{};
  uint8_t lastAdv_[31] = {};
  size_t lastAdvLength_ = 0;

  // Security handling of the current link, host task only
  bool securityTried_ = false;
  bool bondRetried_ = false;
  bool awaitingSecurity_ = false;
};

#endif
