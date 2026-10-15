#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include "nvs.h"

#define STORAGE_PARTITION "settings"

// Flash file system, mounted at DISK_MOUNT_POINT by diskInit()
#define DISK_PARTITION   "littlefs"
#define DISK_MOUNT_POINT "/littlefs"

#define SAVE_SETTINGS 0x01
#define SAVE_BANDS    0x02
#define SAVE_MEMORIES 0x04
#define SAVE_CUR_BAND 0x08
#define SAVE_VERIFY   0x80
#define SAVE_ALL      (SAVE_SETTINGS|SAVE_BANDS|SAVE_MEMORIES|SAVE_VERIFY)

//
// Typed access to one namespace of the "settings" NVS partition.
// The on-flash key types match what the Arduino Preferences class used
// (u8, u16, u32, str and blob), so existing user settings stay readable.
//
class Prefs
{
public:
  // Open a namespace, returns false on failure (all accessors then
  // return their default values and writes are ignored)
  bool begin(const char *name, bool readOnly = false);
  void end();
  bool clear();

  uint8_t getU8(const char *key, uint8_t def = 0);
  uint16_t getU16(const char *key, uint16_t def = 0);
  uint32_t getU32(const char *key, uint32_t def = 0);
  bool getBool(const char *key, bool def = false);
  std::string getString(const char *key, const char *def = "");
  // Returns the number of bytes read, or 0 if the key does not exist or does not fit
  size_t getBytes(const char *key, void *buf, size_t len);

  size_t putU8(const char *key, uint8_t value);
  size_t putU16(const char *key, uint16_t value);
  size_t putU32(const char *key, uint32_t value);
  size_t putBool(const char *key, bool value);
  size_t putString(const char *key, const char *value);
  // Returns the number of bytes written, or 0 on failure
  size_t putBytes(const char *key, const void *buf, size_t len);

private:
  nvs_handle_t handle = 0;
  bool readOnly = true;
};

extern Prefs prefs;

// Initialize the NVS partitions (default and STORAGE_PARTITION)
bool nvsInit();

void prefsTickTime();
void prefsInvalidate();
bool prefsAreWritten();
bool nvsErase();

bool diskInit(bool force = false);

void prefsRequestSave(uint32_t what, bool now = false);
void prefsSave(uint32_t items = SAVE_ALL);
bool prefsLoad(uint32_t items = SAVE_ALL);
void prefsSaveBand(uint8_t idx, bool openPrefs = true);
bool prefsLoadBand(uint8_t idx, bool openPrefs = true);
void prefsSaveMemory(uint8_t idx, bool openPrefs = true);
bool prefsLoadMemory(uint8_t idx, bool openPrefs = true);

#endif // STORAGE_H
