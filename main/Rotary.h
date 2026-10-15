// Rotary encoder handler, see Rotary.cpp
#ifndef rotary_h
#define rotary_h

#include <stdint.h>
#include "sdkconfig.h"

// Enable CONFIG_ATS_HALF_STEP to emit codes twice per step
// (the LILYGO T-Embed encoder always needs it)
#if defined(CONFIG_ATS_BOARD_LILYGO_T_EMBED) && !defined(CONFIG_ATS_HALF_STEP)
#define CONFIG_ATS_HALF_STEP 1
#endif

// Values returned by 'process'
#define DIR_NONE 0x0    // No complete step yet
#define DIR_CW   0x10   // Clockwise step
#define DIR_CCW  0x20   // Anti-clockwise step

class Rotary
{
  public:
    // The pins must be configured as inputs with pull-ups by the caller
    Rotary(uint8_t pin1, uint8_t pin2);
    // Process pin(s), called from the GPIO interrupt handler
    unsigned char process();
  private:
    unsigned char state;
    unsigned char pin1;
    unsigned char pin2;
};
#endif
