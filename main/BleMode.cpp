#include "Common.h"
#include "Themes.h"
#include "Remote.h"
#include "Draw.h"
#include "BleHidCentral.h"
#include "BleStack.h"
#include "BleMode.h"
#include "Platform.h"

static BleUartPeripheral BLESerial;
static BleHidCentral BLEHid;
static RemoteState remoteBLEState;

//
// Get current connection status
// (-1 - not connected, 0 - disabled, 1 - connected)
//
int8_t getBleStatus()
{
  if (BLESerial.isStarted())
    return BLESerial.isConnected() ? 1 : -1;

  if (BLEHid.isStarted())
    return BLEHid.isConnected() ? 1 : -1;

  return 0;
}

//
// Stop BLE hardware
//
// We do not deinit the NimBLE host and the controller fully because it leads
// to crashes. Only the controller is powered down, see BleStack.cpp.
// Also see: https://github.com/afpineda/NuS-NimBLE-Serial/issues/12#issuecomment-2912732754
//
void bleStop()
{
  if (BLESerial.isStarted())
    BLESerial.end();

  if (BLEHid.isStarted())
    BLEHid.end();

  // Experimental attempt to stop consuming power
  // https://github.com/esp32-si4732/ats-mini/discussions/324#discussioncomment-17123023
  bleStackDisable();
}

void bleInit(uint8_t bleMode)
{
  bleStop();
  if (bleMode != BLE_OFF && !bleStackEnable(RECEIVER_NAME)) return;

  switch(bleMode)
  {
    case BLE_ADHOC:
      BLESerial.begin(RECEIVER_NAME);
      break;
    case BLE_HID:
      BLEHid.begin(RECEIVER_NAME);
      break;
  }
}

int bleLoop(uint8_t bleMode)
{
  if (bleMode == BLE_ADHOC)
  {
    if (BLESerial.isConnected())
      remoteTickTime(&BLESerial, &remoteBLEState);
    if (!BLESerial.isConnected()) return 0;
    if (BLESerial.available())
      return remoteDoCommand(&BLESerial, &remoteBLEState, BLESerial.read());
    return 0;
  }

  if (bleMode != BLE_HID)
    return 0;

  if (BLEHid.isStarted() && !BLEHid.isConnected() && BLEHid.isConnectPending() && BLEHid.peerName())
  {
    drawScreen();
    drawScreen("Connecting BLE HID", BLEHid.peerName());
    delayMs(500);
  }

  BLEHid.loop();
  if (!BLEHid.isConnected()) return 0;

  BleHidState input = BLEHid.update();
  int event = input.isPressed ? REMOTE_PRESSED : 0;
  if (!input.rotation && !input.wasClicked && !input.wasShortPressed) return event;

  event |= REMOTE_CHANGED;
  if (input.rotation)
  {
    event |= input.rotation << REMOTE_DIRECTION;
    event |= REMOTE_PREFS;
  }
  if (input.wasClicked)
    event |= REMOTE_CLICK;
  if (input.wasShortPressed)
    event |= REMOTE_SHORT_PRESS;
  return event;
}

bool bleConsumeAbortPending(uint8_t bleMode)
{
  if (bleMode == BLE_ADHOC)
    return BLESerial.consumeAbortPending();

  if (bleMode == BLE_HID)
    return BLEHid.consumeAbortPending();

  return false;
}
