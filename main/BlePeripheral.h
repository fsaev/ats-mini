#ifndef BLE_PERIPHERAL_H
#define BLE_PERIPHERAL_H

#include <atomic>
#include <mutex>
#include <stdint.h>
#include "host/ble_hs.h"

//
// Connectable advertiser accepting a single central. Advertising resumes
// after a disconnect for as long as the peripheral is started.
//
// The GAP events arrive in the NimBLE host task, begin() and end() are meant
// to be called from the application task.
//
class BlePeripheral {
public:
  BlePeripheral() = default;
  virtual ~BlePeripheral() = default;

  void begin(const char* deviceName);
  void end();

  bool isStarted() const;
  bool isConnected() const;
  uint8_t connectedCount() const;

protected:
  virtual void configureDefaults() {}
  virtual void configureSecurity();
  virtual void configureAdvertising(struct ble_hs_adv_fields& fields) { (void)fields; }
  virtual void destroyServices() {}

  // Called in the host task
  virtual void onConnect(uint16_t connHandle) { (void)connHandle; }
  virtual void onDisconnect(uint16_t connHandle) { (void)connHandle; }
  // Receives the events not handled here (subscribe, MTU change...)
  virtual int onGapEvent(struct ble_gap_event* event) { (void)event; return 0; }

  uint16_t connHandle() const { return connHandle_.load(); }

private:
  static int gapEventCallback(struct ble_gap_event* event, void* arg);
  int handleGapEvent(struct ble_gap_event* event);
  void advertise();

  std::atomic<bool> started{false};
  std::atomic<uint16_t> connHandle_{BLE_HS_CONN_HANDLE_NONE};
  // Serialises advertising start (host task) against end() (application task)
  std::mutex advMutex;
};

#endif
