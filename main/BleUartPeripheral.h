#ifndef BLE_UART_PERIPHERAL_H
#define BLE_UART_PERIPHERAL_H

#include <atomic>
#include "BleByteRing.h"
#include "BlePeripheral.h"
#include "ByteStream.h"

// Nordic UART service, the UUIDs are defined in BleUartPeripheral.cpp:
//   service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX      6E400002-... (written by the central)
//   TX      6E400003-... (notified to the central)
#define BLE_BUFFER_SIZE             1024
#define BLE_MIN_MTU                 23
#define BLE_MAX_MTU                 517
#define BLE_TX_PAYLOAD_CAP          40
#define BLE_TX_DEFER_MS             5
#define BLE_NOTIFY_RETRY_DELAY_MS   50

// Register the GATT service. Has to be called once, after the NimBLE port
// is initialised and before the host is started (see BleStack.cpp).
int bleUartGattInit();

class BleUartPeripheral : public BlePeripheral, public ByteStream {
public:
  BleUartPeripheral() = default;

  int available() override;
  int peek() override;
  int read() override;
  void flush() override;
  bool consumeAbortPending();

  size_t write(const uint8_t* data, size_t size) override;
  using ByteStream::write;

  // Called by the GATT access callback when the central writes to RX
  static void rxReceived(const uint8_t* data, size_t length);

protected:
  void configureDefaults() override;
  void configureAdvertising(struct ble_hs_adv_fields& fields) override;
  void destroyServices() override;

  void onConnect(uint16_t connHandle) override;
  void onDisconnect(uint16_t connHandle) override;
  int onGapEvent(struct ble_gap_event* event) override;

private:
  bool canSend() const;
  size_t txPayloadSize() const;
  size_t pumpTx();
  void resetSession();

  // The RX ring is filled by the host task and read by the application task.
  // The TX ring, the defer and the retry timers belong to the application
  // task alone, the host task only requests a reset through txResetPending.
  BleByteRing<BLE_BUFFER_SIZE> rxBuf;
  BleByteRing<BLE_BUFFER_SIZE> txBuf;
  std::atomic<bool> abortPending{false};
  std::atomic<bool> txResetPending{false};
  std::atomic<bool> txSubscribed{false};
  // Negotiated ATT MTU for the active peer; actual TX uses min(mtu - 3, cap).
  std::atomic<uint16_t> txPeerMtu{BLE_MIN_MTU};
  // Flat notify payload assembled from txBuf.
  uint8_t txChunk[BLE_MAX_MTU - 3];
  // Delay before starting a fresh burst so small writes can batch together.
  uint32_t txDeferUntilMs = 0;
  // Backoff deadline for retrying after the stack ran out of buffers.
  uint32_t txRetryAfterMs = 0;

  static std::atomic<BleUartPeripheral*> activeInstance;
};

#endif
