#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include "nvs.h"
#include "qrcode.h"

static void displayQRCode(esp_qrcode_handle_t qrcode)
{
  int size = esp_qrcode_get_size(qrcode);

  for(int y = 0 ; y < size ; y++)
    for(int x = 0 ; x < size ; x++)
      if(esp_qrcode_get_module(qrcode, x, y))
        spr.fillRect(2 + x * 4, 170 - 2 - size * 4 + y * 4, 4, 4, TH.text);
}

static void drawAboutCommon(uint8_t arrow)
{
  if(arrow & 3) spr.fillRect(282, 11, 22, 3, TH.text_muted);
  if(arrow & 2) spr.fillTriangle(279, 12, 285, 8, 285, 16, TH.text_muted);
  if(arrow & 1) spr.fillTriangle(307, 12, 301, 8, 301, 16, TH.text_muted);

  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TH.text_muted);
  spr.drawString(RECEIVER_DESC, 0, 0, FONT4);
  spr.setTextColor(TH.text);
  spr.drawString(getVersion(), 2, 25, FONT2);
}

//
// Show HELP screen
//
void drawAboutHelp(uint8_t arrow)
{
  drawAboutCommon(arrow);
  esp_qrcode_config_t qrcode_config = ESP_QRCODE_CONFIG_DEFAULT();
  qrcode_config.display_func = displayQRCode;
  esp_qrcode_generate(&qrcode_config, MANUAL_URL);
  spr.drawString("Scan the QR code to read", 130, 70 + 16 * -1, FONT2);
  spr.drawString("the User Manual.", 130, 70 + 16 * 0, FONT2);
  spr.drawString("Click the encoder button", 130, 70 + 16 * 1, FONT2);
  spr.drawString("to continue.", 130, 70 + 16 * 2, FONT2);
  if(arrow)
  {
    spr.drawString("Rotate the encoder to see", 130, 70 + 16 * 3, FONT2);
    spr.drawString("the next page.", 130, 70 + 16 * 4, FONT2);
  }
  else
  {
    spr.drawString("To see this screen again,", 130, 70 + 16 * 4, FONT2);
    spr.drawString("go to Menu->Settings->About.", 130, 70 + 16 * 5, FONT2);
  }
  spr.pushSprite(0, 0);
}

// Size of the running firmware image, in bytes
static uint32_t getSketchSize()
{
  static uint32_t size = 0;

  if(!size)
  {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_partition_pos_t pos = { running->address, running->size };
    esp_image_metadata_t data;
    if(esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &data) == ESP_OK)
      size = data.image_len;
  }

  return(size);
}

// Size of the partition the next firmware update goes to, in bytes
static uint32_t getFreeSketchSpace()
{
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  return(next? next->size : 0);
}

static const char *getChipModel()
{
  esp_chip_info_t info;
  esp_chip_info(&info);
  switch(info.model)
  {
    case CHIP_ESP32S3: return("ESP32-S3");
    default:           return("ESP32");
  }
}

static int getChipRevision()
{
  esp_chip_info_t info;
  esp_chip_info(&info);
  return(info.revision);
}

//
// Show SYSTEM screen
//
static void drawAboutSystem(uint8_t arrow)
{
  drawAboutCommon(arrow);

  char text[100];
  sprintf(
    text,
    "CPU: %s r%i, %lu MHz",
    getChipModel(),
    getChipRevision(),
    (unsigned long)esp_rom_get_cpu_ticks_per_us()
  );
  spr.drawString(text, 2, 70 + 16 * -1, FONT2);

  uint32_t flashSize = 0;
  esp_flash_get_physical_size(NULL, &flashSize);

  size_t fsTotal = 0, fsUsed = 0;
  esp_littlefs_info(DISK_PARTITION, &fsTotal, &fsUsed);

  sprintf(
    text,
    "FLASH: %luM, %luk (%luk), FS %luk (%luk)",
    (unsigned long)(flashSize / (1024U * 1024U)),
    (unsigned long)(getFreeSketchSpace() / 1024U),
    (unsigned long)((getFreeSketchSpace() - getSketchSize()) / 1024U),
    (unsigned long)(fsTotal / 1024U),
    (unsigned long)((fsTotal - fsUsed) / 1024U)
  );
  spr.drawString(text, 2, 70 + 16 * 0, FONT2);

  nvs_stats_t nvs_stats;
  nvs_get_stats(STORAGE_PARTITION, &nvs_stats);
  sprintf(
    text,
    "NVS: TOTAL %u, USED %u, FREE %u",
    nvs_stats.total_entries,
    nvs_stats.used_entries,
    nvs_stats.free_entries
  );
  spr.drawString(text, 2, 70 + 16 * 1, FONT2);

  sprintf(
    text,
    "MEM: HEAP %luk (%luk), PSRAM %luk (%luk)",
    (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL)/1024U),
    (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)/1024U),
    (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)/1024U),
    (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024U)
  );
  spr.drawString(text, 2, 70 + 16 * 2, FONT2);

  sprintf(
    text,
    "Display ID: %08lX, STAT: %02X%08lX",
#if !defined(CONFIG_ATS_BOARD_LILYGO_T_EMBED)
    (unsigned long)tft.readRegister32(ST7789_RDDID, 1),
    (unsigned int)tft.readRegister8(ST7789_RDDST, 1),
    (unsigned long)tft.readRegister32(ST7789_RDDST, 2)
#else
    0UL, 0U, 0UL
#endif
  );
  spr.drawString(text, 2, 70 + 16 * 3, FONT2);

  char *ip = getWiFiIPAddress();
  sprintf(text, "WiFi MAC: %s%s%s", getMACAddress(), *ip ? ", IP: " : "", *ip ? ip : "");
  spr.drawString(text, 2, 70 + 16 * 4, FONT2);

  for(int i=0 ; i<8 ; i++)
  {
    uint16_t rgb = (i&1? 0x001F:0) | (i&2? 0x07E0:0) | (i&4? 0xF800:0);
    spr.fillRect(i*40, 160, 40, 20, rgb);
  }
  spr.pushSprite(0, 0);
}

//
// Show AUTHORS screen
//
static void drawAboutAuthors(uint8_t arrow)
{
  drawAboutCommon(arrow);
  spr.drawString(FIRMWARE_URL, 2, 25 + 16, FONT2);
  spr.drawString(AUTHORS_LINE1, 2, 70, FONT2);
  spr.drawString(AUTHORS_LINE2, 2, 70 + 16, FONT2);
  spr.drawString(AUTHORS_LINE3, 2, 70 + 16 * 2, FONT2);
  spr.drawString(AUTHORS_LINE4, 2, 70 + 16 * 3, FONT2);
  spr.pushSprite(0, 0);
}

//
// Draw ABOUT screens
//
void drawAbout()
{
  switch(doAbout(0))
  {
    case 0: drawAboutHelp(1); break;
    case 1: drawAboutAuthors(3); break;
    case 2: drawAboutSystem(2); break;
    default: break;
  }
}
