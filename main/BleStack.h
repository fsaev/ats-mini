#ifndef BLE_STACK_H
#define BLE_STACK_H

#include <stdint.h>

//
// Life cycle of the NimBLE host and the BLE controller.
//
// The host and the controller are initialised once on the first
// bleStackEnable() and then kept initialised for the lifetime of the firmware
// (a full deinit leads to crashes, see bleStop() in BleMode.cpp). Between
// BLE sessions only the controller is switched off to save power.
//

// Bring the stack up (first call) or the controller back up (later calls) and
// wait until the host is synchronised with the controller.
// Returns false if that did not succeed, the stack is then left powered down.
bool bleStackEnable(const char* deviceName);

// Power the controller down. The caller must have stopped advertising,
// scanning and disconnected all peers before.
void bleStackDisable();

// True if the host is synchronised with the controller and can be used
bool bleStackIsReady();

// Own address type to use for advertising, scanning and connecting
uint8_t bleStackOwnAddrType();

#endif
