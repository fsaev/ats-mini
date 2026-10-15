#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <LovyanGFX.hpp>

// ST7789 commands used by the firmware
#define ST7789_RDDID   0x04
#define ST7789_RDDST   0x09
#define ST7789_SLPIN   0x10
#define ST7789_SLPOUT  0x11
#define ST7789_DISPOFF 0x28
#define ST7789_DISPON  0x29

// Fonts. The numbers are those of the built-in TFT_eSPI fonts that the layout was designed for.
#define FONT0         (&lgfx::fonts::Font0)
#define FONT2         (&lgfx::fonts::Font2)
#define FONT4         (&lgfx::fonts::Font4)
#define FONT6         (&lgfx::fonts::Font6)
#define FONT7         (&lgfx::fonts::Font7)
#define FONT8         (&lgfx::fonts::Font8)
#define FONT_ORBITRON (&lgfx::fonts::Orbitron_Light_24)

// Display panel, the pins and the bus depend on the hardware variant
class Display : public lgfx::LGFX_Device
{
public:
  Display();

  // Initialize the panel and fix up known display variants
  bool begin();

  // Put the panel to sleep and wake it up again
  void powerDown();
  void powerUp();

  // Read a register of the ST7789 the way TFT_eSPI did: 'index' is the 1-based
  // number of the byte to return, counting the first dummy byte
  uint8_t readRegister8(uint8_t command, uint8_t index);
  uint32_t readRegister32(uint8_t command, uint8_t index);

private:
  class Panel;
  Panel *panel_;
};

extern Display tft;
extern lgfx::LGFX_Sprite spr;

// Backlight PWM, level is 0 (off) to 255 (full brightness)
void backlightInit();
void backlightSet(uint8_t level);

#endif // DISPLAY_H
