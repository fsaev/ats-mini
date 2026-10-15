#include <string.h>
#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Menu.h"
#include "nvs_flash.h"
#include "esp_littlefs.h"

// Time of inactivity to start writing preferences
#define STORE_TIME    10000

// Preferences saved here
Prefs prefs;

bool Prefs::begin(const char *name, bool readOnly_)
{
  end();
  readOnly = readOnly_;
  if(nvs_open_from_partition(STORAGE_PARTITION, name, readOnly? NVS_READONLY : NVS_READWRITE, &handle) != ESP_OK)
    handle = 0;
  return(handle != 0);
}

void Prefs::end()
{
  if(handle) nvs_close(handle);
  handle = 0;
}

bool Prefs::clear()
{
  if(!handle || readOnly) return(false);
  return(nvs_erase_all(handle) == ESP_OK && nvs_commit(handle) == ESP_OK);
}

uint8_t Prefs::getU8(const char *key, uint8_t def)
{
  uint8_t value;
  return(handle && nvs_get_u8(handle, key, &value) == ESP_OK? value : def);
}

uint16_t Prefs::getU16(const char *key, uint16_t def)
{
  uint16_t value;
  return(handle && nvs_get_u16(handle, key, &value) == ESP_OK? value : def);
}

uint32_t Prefs::getU32(const char *key, uint32_t def)
{
  uint32_t value;
  return(handle && nvs_get_u32(handle, key, &value) == ESP_OK? value : def);
}

bool Prefs::getBool(const char *key, bool def)
{
  return(getU8(key, def? 1 : 0) != 0);
}

std::string Prefs::getString(const char *key, const char *def)
{
  size_t length = 0;
  if(!handle || nvs_get_str(handle, key, NULL, &length) != ESP_OK || !length)
    return(def);

  std::string value(length, '\0');
  if(nvs_get_str(handle, key, &value[0], &length) != ESP_OK)
    return(def);

  // Drop the terminating zero
  value.resize(length? length - 1 : 0);
  return(value);
}

size_t Prefs::getBytes(const char *key, void *buf, size_t len)
{
  size_t size = len;
  return(handle && nvs_get_blob(handle, key, buf, &size) == ESP_OK? size : 0);
}

size_t Prefs::putU8(const char *key, uint8_t value)
{
  if(!handle || readOnly) return(0);
  return(nvs_set_u8(handle, key, value) == ESP_OK && nvs_commit(handle) == ESP_OK? sizeof(value) : 0);
}

size_t Prefs::putU16(const char *key, uint16_t value)
{
  if(!handle || readOnly) return(0);
  return(nvs_set_u16(handle, key, value) == ESP_OK && nvs_commit(handle) == ESP_OK? sizeof(value) : 0);
}

size_t Prefs::putU32(const char *key, uint32_t value)
{
  if(!handle || readOnly) return(0);
  return(nvs_set_u32(handle, key, value) == ESP_OK && nvs_commit(handle) == ESP_OK? sizeof(value) : 0);
}

size_t Prefs::putBool(const char *key, bool value)
{
  return(putU8(key, value? 1 : 0));
}

size_t Prefs::putString(const char *key, const char *value)
{
  if(!handle || readOnly) return(0);
  return(nvs_set_str(handle, key, value) == ESP_OK && nvs_commit(handle) == ESP_OK? strlen(value) : 0);
}

size_t Prefs::putBytes(const char *key, const void *buf, size_t len)
{
  if(!handle || readOnly) return(0);
  return(nvs_set_blob(handle, key, buf, len) == ESP_OK && nvs_commit(handle) == ESP_OK? len : 0);
}

bool nvsInit()
{
  // Default partition, used by the Wi-Fi and Bluetooth stacks
  esp_err_t err = nvs_flash_init();
  if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if(err != ESP_OK) return(false);

  // Settings partition
  err = nvs_flash_init_partition(STORAGE_PARTITION);
  if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    nvs_flash_erase_partition(STORAGE_PARTITION);
    err = nvs_flash_init_partition(STORAGE_PARTITION);
  }
  return(err == ESP_OK);
}

static uint32_t itIsTimeToSave = 0;       // Preferences to save, or 0 for none
static bool savingPrefsFlag    = false;   // TRUE: Saving preferences
static uint32_t storeTime      = nowMs();

// To store any change to preferences, we need at least STORE_TIME
// milliseconds of inactivity.
void prefsRequestSave(uint32_t what, bool now)
{
  // Underflow is ok here, see prefsTickTime()
  storeTime = nowMs() - (now? STORE_TIME : 0);
  itIsTimeToSave |= what;
}

void prefsTickTime()
{
  // Save configuration if requested
  if(itIsTimeToSave && ((nowMs() - storeTime) >= STORE_TIME))
  {
    prefsSave(itIsTimeToSave);
    storeTime = nowMs();
    itIsTimeToSave = 0;
  }
}

// Return true if preferences have been written
bool prefsAreWritten()
{
  bool result = savingPrefsFlag;
  savingPrefsFlag = false;
  return(result);
}

// Invlaidate all currently saved preferences
void prefsInvalidate()
{
  static const char *sections[] =
  { "settings", "memories", "bands", "network", 0 };

  // Clear all applicable sections
  for(int j = 0 ; sections[j] ; ++j)
  {
    prefs.begin(sections[j], false);
    prefs.clear();
    prefs.end();
  }
}

struct SavedBand
{
  uint8_t bandMode;       // Band mode (FM, AM, LSB, or USB)
  uint16_t currentFreq;   // Current frequency
  int8_t currentStepIdx;  // Current frequency step
  int8_t bandwidthIdx;    // Index of the table bandwidthFM, bandwidthAM or bandwidthSSB;
  int16_t usbCal;         // USB calibration value
  int16_t lsbCal;         // LSB calibration value
};

void prefsSaveBand(uint8_t idx, bool openPrefs)
{
  SavedBand value;
  char name[32];

  // Will be saving to bands
  if(openPrefs) prefs.begin("bands", false);

  // Compose preference name and value
  sprintf(name, "Band-%d", idx);
  value.currentFreq    = bands[idx].currentFreq;     // Frequency
  value.bandMode       = bands[idx].bandMode;        // Modulation
  value.currentStepIdx = bands[idx].currentStepIdx;  // Step
  value.bandwidthIdx   = bands[idx].bandwidthIdx;    // Bandwidth
  value.usbCal         = bands[idx].usbCal;          // USB Calibration
  value.lsbCal         = bands[idx].lsbCal;          // LSB Calibration

  // Write a preference
  prefs.putBytes(name, &value, sizeof(value));

  // Done with band preferences
  if(openPrefs) prefs.end();
}

bool prefsLoadBand(uint8_t idx, bool openPrefs)
{
  SavedBand value;
  char name[32];

  // Will be loading from bands
  if(openPrefs) prefs.begin("bands", true);

  // Compose preference name
  sprintf(name, "Band-%d", idx);

  // Read preference
  bool result = !!prefs.getBytes(name, &value, sizeof(value));
  if(result)
  {
    bands[idx].currentFreq    = value.currentFreq;    // Frequency
    bands[idx].bandMode       = value.bandMode;       // Modulation
    bands[idx].currentStepIdx = value.currentStepIdx; // Step
    bands[idx].bandwidthIdx   = value.bandwidthIdx;   // Bandwidth
    bands[idx].usbCal         = value.usbCal;         // USB Calibration
    bands[idx].lsbCal         = value.lsbCal;         // LSB Calibration
  }

  // Done with band preferences
  if(openPrefs) prefs.end();

  // Done
  return(result);
}

void prefsSaveMemory(uint8_t idx, bool openPrefs)
{
  char name[32];

  // Will be saving to memories
  if(openPrefs) prefs.begin("memories", false);

  // Compose preference name
  sprintf(name, "Memory-%d", idx);

  // Write a preference
  prefs.putBytes(name, &memories[idx], sizeof(memories[idx]));

  // Done with memory preferences
  if(openPrefs) prefs.end();
}

bool prefsLoadMemory(uint8_t idx, bool openPrefs)
{
  char name[32];

  // Will be loading from memories
  if(openPrefs) prefs.begin("memories", true);

  // Compose preference name
  sprintf(name, "Memory-%d", idx);

  // Write a preference
  bool result = !!prefs.getBytes(name, &memories[idx], sizeof(memories[idx]));

  // Done with memory preferences
  if(openPrefs) prefs.end();

  // Done
  return(result);
}

void prefsSave(uint32_t items)
{
  if(items & SAVE_SETTINGS)
  {
    // Will be saving to settings
    prefs.begin("settings", false);

    // Save main global settings
    prefs.putU8("Version",  VER_SETTINGS);      // Settings version
    prefs.putU16("App",     VER_APP);           // Application version
    prefs.putU8("Volume",   volume);            // Current volume
    prefs.putU8("Band",     bandIdx);           // Current band
    prefs.putU8("WiFiMode", wifiModeIdx);       // WiFi connection mode

    // Save additional global settings
    prefs.putU16("Brightness", currentBrt);     // Brightness
    prefs.putU8("FmAGC",       FmAgcIdx);       // FM AGC/ATTN
    prefs.putU8("AmAGC",       AmAgcIdx);       // AM AGC/ATTN
    prefs.putU8("SsbAGC",      SsbAgcIdx);      // SSB AGC/ATTN
    prefs.putU8("AmAVC",       AmAvcIdx);       // AM AVC
    prefs.putU8("SsbAVC",      SsbAvcIdx);      // SSB AVC
    prefs.putU8("AmSoftMute",  AmSoftMuteIdx);  // AM soft mute
    prefs.putU8("SsbSoftMute", SsbSoftMuteIdx); // SSB soft mute
    prefs.putU16("Sleep",      currentSleep);   // Sleep delay
    prefs.putU8("Theme",       themeIdx);       // Color theme
    prefs.putU8("RDSMode",     rdsModeIdx);     // RDS mode
    prefs.putU8("SleepMode",   sleepModeIdx);   // Sleep mode
    prefs.putU8("ZoomMenu",    zoomMenu);       // TRUE: Zoom menu
    prefs.putBool("ScrollDir", scrollDirection<0); // TRUE: Reverse scroll
    prefs.putU8("UTCOffset",   utcOffsetIdx);   // UTC Offset
    prefs.putU32("Squelch",      ((uint32_t)currentSquelch[FM]) |
                                  ((uint32_t)currentSquelch[LSB] << 8) |
                                  ((uint32_t)currentSquelch[USB] << 16) |
                                  ((uint32_t)currentSquelch[AM] << 24)); // Squelch
    prefs.putU8("FmRegion",    FmRegionIdx);    // FM region
    prefs.putU8("UILayout",    uiLayoutIdx);    // UI Layout
    prefs.putU8("BLEMode",     bleModeIdx);     // Bluetooth mode
    prefs.putU8("USBMode",     usbModeIdx);     // USB mode

    // Done with global settings
    prefs.end();
  }

  if(items & SAVE_BANDS)
  {
    // Will be saving to bands
    prefs.begin("bands", false);
    prefs.putU8("Version", VER_BANDS);
    // Save band settings
    for(int i=0 ; i<getTotalBands() ; i++) prefsSaveBand(i, false);
    // Done with bands
    prefs.end();
  }
  else if(items & SAVE_CUR_BAND)
  {
    // Save current band only
    prefsSaveBand(bandIdx);
  }

  if(items & SAVE_MEMORIES)
  {
    // Will be saving to memories
    prefs.begin("memories", false);
    prefs.putU8("Version", VER_MEMORIES);
    // Save current memories
    for(int i=0 ; i<getTotalMemories() ; i++) prefsSaveMemory(i, false);
    // Done with memories
    prefs.end();
  }

  // Preferences have been saved
  savingPrefsFlag = true;
}

bool prefsLoad(uint32_t items)
{
  if(items & SAVE_SETTINGS)
  {
    // Will be loading from settings
    prefs.begin("settings", true);

    // Check currently saved version
    if((items & SAVE_VERIFY) && (prefs.getU8("Version", 0) != VER_SETTINGS))
    {
      prefs.end();
      return(false);
    }

    // Load main global settings
    volume         = prefs.getU8("Volume", volume);          // Current volume
    bandIdx        = prefs.getU8("Band", bandIdx);           // Current band
    wifiModeIdx    = prefs.getU8("WiFiMode", wifiModeIdx);   // WiFi connection mode
    currentBrt     = prefs.getU16("Brightness", currentBrt); // Brightness
    FmAgcIdx       = prefs.getU8("FmAGC", FmAgcIdx);         // FM AGC/ATTN
    AmAgcIdx       = prefs.getU8("AmAGC", AmAgcIdx);         // AM AGC/ATTN
    SsbAgcIdx      = prefs.getU8("SsbAGC", SsbAgcIdx);       // SSB AGC/ATTN
    AmAvcIdx       = prefs.getU8("AmAVC", AmAvcIdx);         // AM AVC
    SsbAvcIdx      = prefs.getU8("SsbAVC", SsbAvcIdx);       // SSB AVC
    AmSoftMuteIdx  = prefs.getU8("AmSoftMute", AmSoftMuteIdx);   // AM soft mute
    SsbSoftMuteIdx = prefs.getU8("SsbSoftMute", SsbSoftMuteIdx); // SSB soft mute
    currentSleep   = prefs.getU16("Sleep", currentSleep);    // Sleep delay
    themeIdx       = prefs.getU8("Theme", themeIdx);         // Color theme
    rdsModeIdx     = prefs.getU8("RDSMode", rdsModeIdx);     // RDS mode
    sleepModeIdx   = prefs.getU8("SleepMode", sleepModeIdx); // Sleep mode
    zoomMenu       = prefs.getU8("ZoomMenu", zoomMenu);      // TRUE: Zoom menu
    scrollDirection = prefs.getBool("ScrollDir", scrollDirection<0)? -1:1; // TRUE: Reverse scroll
    utcOffsetIdx   = prefs.getU8("UTCOffset", utcOffsetIdx); // UTC Offset
    uint32_t squelch = prefs.getU32("Squelch", ((uint32_t)currentSquelch[FM]) |
                                                ((uint32_t)currentSquelch[LSB] << 8) |
                                                ((uint32_t)currentSquelch[USB] << 16) |
                                                ((uint32_t)currentSquelch[AM] << 24)); // Squelch
    currentSquelch[FM]  = squelch & 0xff;
    currentSquelch[LSB] = (squelch >> 8) & 0xff;
    currentSquelch[USB] = (squelch >> 16) & 0xff;
    currentSquelch[AM]  = (squelch >> 24) & 0xff;
    FmRegionIdx    = prefs.getU8("FmRegion", FmRegionIdx);   // FM region
    uiLayoutIdx    = prefs.getU8("UILayout", uiLayoutIdx);   // UI Layout
    bleModeIdx     = prefs.getU8("BLEMode", bleModeIdx);     // Bluetooth mode
    usbModeIdx     = prefs.getU8("USBMode", usbModeIdx);     // USB mode

    // Done with global settings
    prefs.end();
  }

  if(items & SAVE_BANDS)
  {
    // Will be loading from bands
    prefs.begin("bands", true);

    // Check currently saved version
    if((items & SAVE_VERIFY) && (prefs.getU8("Version", 0) != VER_BANDS))
    {
      prefs.end();
      return(false);
    }

    // Read band settings
    for(int i=0 ; i<getTotalBands() ; i++) prefsLoadBand(i, false);

    // Done with bands
    prefs.end();
  }
  else if(items & SAVE_CUR_BAND)
  {
    // Load current band only
    prefsLoadBand(bandIdx);
  }

  if(items & SAVE_MEMORIES)
  {
    // Will be loading from memories
    prefs.begin("memories", true);

    // Check currently saved version
    if((items & SAVE_VERIFY) && (prefs.getU8("Version", 0) != VER_MEMORIES))
    {
      prefs.end();
      return(false);
    }

    // Read all memories
    for(int i=0 ; i<getTotalMemories() ; i++) prefsLoadMemory(i, false);

    // Done with memories
    prefs.end();
  }

  return(true);
}

static bool diskMount()
{
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = DISK_MOUNT_POINT;
  conf.partition_label = DISK_PARTITION;
  return(esp_vfs_littlefs_register(&conf) == ESP_OK);
}

bool diskInit(bool force)
{
  if(!force)
  {
    prefs.begin("storage", true);
    force = prefs.getU8("Version", 0) != VER_STORAGE;
    prefs.end();
  }

  if(force)
  {
    esp_vfs_littlefs_unregister(DISK_PARTITION);
    esp_littlefs_format(DISK_PARTITION);
  }

  bool mounted = diskMount();

  if(!mounted)
  {
    if(esp_littlefs_format(DISK_PARTITION) != ESP_OK)
    {
      return(false);
    }

    mounted = diskMount();
    if(!mounted)
    {
      return(false);
    }
  }

  prefs.begin("storage", false);
  prefs.putU8("Version", VER_STORAGE);
  prefs.end();

  return(true);
}

bool nvsErase()
{
  return(nvs_flash_erase() == ESP_OK &&
         nvs_flash_init() == ESP_OK &&
         nvs_flash_erase_partition(STORAGE_PARTITION) == ESP_OK &&
         nvs_flash_init_partition(STORAGE_PARTITION) == ESP_OK);
}
