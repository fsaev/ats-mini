#ifndef REMOTE_H
#define REMOTE_H

#include <stdint.h>
#include "ByteStream.h"
#include "Platform.h"

typedef struct {
  uint32_t remoteTimer = nowMs();
  uint8_t remoteSeqnum = 0;
  bool remoteLogOn = false;
} RemoteState;

void remoteTickTime(ByteStream* stream, RemoteState* state);
int remoteDoCommand(ByteStream* stream, RemoteState* state, char key);
int serialLoop(uint8_t usbMode);
bool serialConsumeAbortPending(uint8_t usbMode);

#endif
