#include "driver/ledc.h"
#include "Common.h"
#include "Platform.h"
#include "Display.h"

// Pin assignments of the display, other pins are in Common.h
#if defined(CONFIG_ATS_BOARD_LILYGO_T_EMBED)

// LilyGo T-Embed S3 ST7789 using SPI
#define TFT_MISO  -1   // Not connected
#define TFT_MOSI  11
#define TFT_SCLK  12
#define TFT_CS    10
#define TFT_DC    13
#define TFT_RST    9   // Connect reset to ensure display initialises

#else

// ST7789 using 8-bit parallel
#define TFT_CS   6
#define TFT_DC   7
#define TFT_RST  5

#define TFT_WR   8
#define TFT_RD   9

#define TFT_D0  39
#define TFT_D1  40
#define TFT_D2  41
#define TFT_D3  42
#define TFT_D4  45
#define TFT_D5  46
#define TFT_D6  47
#define TFT_D7  48

#endif

// The panel is 170 x 320 pixels, the ST7789 memory is 240 x 320
#define TFT_WIDTH  170
#define TFT_HEIGHT 320

// Rotation of the display, landscape and upside down
#define TFT_ROTATION 3

//
// ST7789 panel with the initialization sequence and the scan order that
// the firmware has always used
//
class Display::Panel : public lgfx::Panel_ST7789
{
public:
  // Some panels are mirrored and need a different scan order
  void setMirrored(bool mirrored) { mirrored_ = mirrored; }

protected:
  const uint8_t *getInitCommands(uint8_t listno) const override
  {
    // Command, number of data bytes (plus CMD_INIT_DELAY if a delay follows the data), data...
    // The color format and scan order are set by Panel_LCD after the sequence
#if defined(CONFIG_ATS_BOARD_LILYGO_T_EMBED)
    static constexpr uint8_t list0[] =
    {
      CMD_SLPOUT,   0 + CMD_INIT_DELAY, 120,
      CMD_NORON,    0,
      0xB6,         2, 0x0A, 0x82,
      CMD_RAMCTRL,  2, 0x00, 0xE0,  // 5 to 6-bit conversion: r0 = r5, b0 = b5
      CMD_PORCTRL,  5, 0x0C, 0x0C, 0x00, 0x33, 0x33,
      CMD_GCTRL,    1, 0x35,
      CMD_VCOMS,    1, 0x28,
      CMD_LCMCTRL,  1, 0x0C,
      CMD_VDVVRHEN, 2, 0x01, 0xFF,
      CMD_VRHS,     1, 0x10,
      CMD_VDVSET,   1, 0x20,
      CMD_FRCTR2,   1, 0x0F,
      CMD_PWCTRL1,  2, 0xA4, 0xA1,
      CMD_PVGAMCTRL, 14, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32,
                         0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17,
      CMD_NVGAMCTRL, 14, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31,
                         0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E,
      CMD_CASET,    4, 0x00, 0x00, 0x00, 0xEF,
      CMD_RASET,    4, 0x00, 0x00, 0x01, 0x3F,
      CMD_NOP,      0 + CMD_INIT_DELAY, 120,
      CMD_DISPON,   0 + CMD_INIT_DELAY, 120,
      0xFF, 0xFF, // end
    };
#else
    static constexpr uint8_t list0[] =
    {
      CMD_SLPOUT,   0 + CMD_INIT_DELAY, 120,
      CMD_NORON,    0,
      CMD_PORCTRL,  5, 0x0B, 0x0B, 0x00, 0x33, 0x33,
      CMD_GCTRL,    1, 0x75,
      CMD_VCOMS,    1, 0x28,
      CMD_LCMCTRL,  1, 0x2C,
      CMD_VDVVRHEN, 1, 0x01,
      CMD_VRHS,     1, 0x1F,
      CMD_FRCTR2,   1, 0x13,
      CMD_PWCTRL1,  1, 0xA7,
      CMD_PWCTRL1,  2, 0xA4, 0xA1,
      0xD6,         1, 0xA1,
      CMD_PVGAMCTRL, 14, 0xF0, 0x05, 0x0A, 0x06, 0x06, 0x03, 0x2B,
                         0x32, 0x43, 0x36, 0x11, 0x10, 0x2B, 0x32,
      CMD_NVGAMCTRL, 14, 0xF0, 0x08, 0x0C, 0x0B, 0x09, 0x24, 0x2B,
                         0x22, 0x43, 0x38, 0x15, 0x16, 0x2F, 0x37,
      CMD_NOP,      0 + CMD_INIT_DELAY, 120,
      CMD_DISPON,   0 + CMD_INIT_DELAY, 120,
      0xFF, 0xFF, // end
    };
#endif
    switch(listno)
    {
      case 0: return list0;
      default: return nullptr;
    }
  }

  // Scan order of the landscape rotation, as it was used with TFT_eSPI. The
  // color order bit is added by Panel_LCD according to the panel configuration.
  uint8_t getMadCtl(uint8_t r) const override
  {
    if(r == TFT_ROTATION)
      return mirrored_ ? (MAD_MV | MAD_MX | MAD_MY | MAD_BGR) : (MAD_MV | MAD_MY);
    return Panel_ST7789::getMadCtl(r);
  }

private:
  bool mirrored_ = false;
};

#if defined(CONFIG_ATS_BOARD_LILYGO_T_EMBED)
typedef lgfx::Bus_SPI DisplayBus;
#else
typedef lgfx::Bus_Parallel8 DisplayBus;
#endif

Display::Display()
{
  // These live as long as the display does
  panel_ = new Panel();
  DisplayBus *bus = new DisplayBus();

  {
    auto cfg = bus->config();
#if defined(CONFIG_ATS_BOARD_LILYGO_T_EMBED)
    cfg.spi_host    = SPI2_HOST;
    cfg.spi_mode    = 0;
    cfg.freq_write  = 40000000;
    cfg.freq_read   = 20000000;
    cfg.spi_3wire   = false;
    cfg.use_lock    = true;
    cfg.dma_channel = SPI_DMA_CH_AUTO;
    cfg.pin_sclk    = TFT_SCLK;
    cfg.pin_mosi    = TFT_MOSI;
    cfg.pin_miso    = TFT_MISO;
    cfg.pin_dc      = TFT_DC;
#else
    cfg.freq_write  = 20000000;
    cfg.freq_read   = 8000000;
    cfg.pin_rd      = TFT_RD;
    cfg.pin_wr      = TFT_WR;
    cfg.pin_rs      = TFT_DC;
    cfg.pin_d0      = TFT_D0;
    cfg.pin_d1      = TFT_D1;
    cfg.pin_d2      = TFT_D2;
    cfg.pin_d3      = TFT_D3;
    cfg.pin_d4      = TFT_D4;
    cfg.pin_d5      = TFT_D5;
    cfg.pin_d6      = TFT_D6;
    cfg.pin_d7      = TFT_D7;
#endif
    bus->config(cfg);
  }
  panel_->setBus(bus);

  {
    auto cfg = panel_->config();
    cfg.pin_cs           = TFT_CS;
    cfg.pin_rst          = TFT_RST;
    cfg.pin_busy         = -1;
    cfg.panel_width      = TFT_WIDTH;
    cfg.panel_height     = TFT_HEIGHT;
    cfg.memory_width     = 240;
    cfg.memory_height    = 320;
#if defined(CONFIG_ATS_BOARD_LILYGO_T_EMBED)
    cfg.offset_x         = 0;
    cfg.offset_y         = 0;
    cfg.rgb_order        = false;  // Colour order Blue-Green-Red
    cfg.readable         = false;
#else
    cfg.offset_x         = 35;     // The panel is in the middle of the memory
    cfg.offset_y         = 0;
    cfg.rgb_order        = true;   // Colour order Red-Green-Blue
    cfg.readable         = true;
#endif
    cfg.offset_rotation  = 0;
    // The first byte read after a command is a dummy, see readRegister8()
    cfg.dummy_read_bits  = 0;
    cfg.invert           = true;   // The ST7789 inversion is on, see begin()
    cfg.dlen_16bit       = false;
    cfg.bus_shared       = false;
    panel_->config(cfg);
  }
  setPanel(panel_);
}

bool Display::begin()
{
  if(!init()) return false;

  // Inversion is on, the argument is relative to the panel configuration
  invertDisplay(false);
  setRotation(TFT_ROTATION);

#if !defined(CONFIG_ATS_BOARD_LILYGO_T_EMBED)
  // Detect and fix the mirrored & inverted display
  // https://github.com/esp32-si4732/ats-mini/issues/41
  uint8_t did3 = readRegister8(ST7789_RDDID, 3);
  // 0x048181B3 - the original display
  // 0x04858552 - high gamma display
  // 0x00009307 - inverted & mirrored display
  if(did3 == 0x93)
  {
    // Turn the inversion off and change the scan order
    invertDisplay(true);
    panel_->setMirrored(true);
    setRotation(TFT_ROTATION);
  }
  else if(did3 == 0x85)
  {
    startWrite();
    writeCommand(0x26); // GAMSET
    writeData(8);       // Gamma Curve 3

    writeCommand(0x55); // WRCACE (content adaptive brightness and color)
    writeData(0xB1);    // High enhancement, UI mode
    endWrite();
  }
#endif

  return true;
}

void Display::powerDown()
{
  startWrite();
  writeCommand(ST7789_DISPOFF);
  writeCommand(ST7789_SLPIN);
  endWrite();
}

void Display::powerUp()
{
  startWrite();
  writeCommand(ST7789_SLPOUT);
  endWrite();
  delayMs(120);
  startWrite();
  writeCommand(ST7789_DISPON);
  endWrite();
}

uint8_t Display::readRegister8(uint8_t command, uint8_t index)
{
  // TFT_eSPI returned the index-th byte read after the command, starting
  // with 1, while LovyanGFX counts the bytes to skip
  return (uint8_t)panel_->readCommand(command, index - 1, 1);
}

uint32_t Display::readRegister32(uint8_t command, uint8_t index)
{
  uint32_t value = 0;
  for(uint8_t i = 0 ; i < 4 ; i++)
    value = (value << 8) | readRegister8(command, index + i);
  return value;
}

Display tft;
lgfx::LGFX_Sprite spr(&tft);

//
// Backlight
//
#define BACKLIGHT_TIMER   LEDC_TIMER_0
#define BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_MODE    LEDC_LOW_SPEED_MODE

void backlightInit()
{
  // Note: At brightness levels below 100%, switching from the PWM may cause power spikes and/or RFI
  ledc_timer_config_t timer = {};
  timer.speed_mode      = BACKLIGHT_MODE;
  timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.timer_num       = BACKLIGHT_TIMER;
  timer.freq_hz         = 16000;
  timer.clk_cfg         = LEDC_AUTO_CLK;
  ledc_timer_config(&timer);

  ledc_channel_config_t channel = {};
  channel.gpio_num   = PIN_LCD_BL;
  channel.speed_mode = BACKLIGHT_MODE;
  channel.channel    = BACKLIGHT_CHANNEL;
  channel.timer_sel  = BACKLIGHT_TIMER;
  channel.duty       = 0;
  channel.hpoint     = 0;
  ledc_channel_config(&channel);
}

void backlightSet(uint8_t level)
{
  ledc_set_duty(BACKLIGHT_MODE, BACKLIGHT_CHANNEL, level);
  ledc_update_duty(BACKLIGHT_MODE, BACKLIGHT_CHANNEL);
}
