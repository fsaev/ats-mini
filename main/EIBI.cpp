#include "Common.h"
#include "Platform.h"
#include "Draw.h"
#include "EIBI.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>

#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

// LittleFS is mounted here by diskInit()
#define EIBI_PATH "/littlefs/schedules.bin"
#define TEMP_PATH "/littlefs/schedules.tmp"
#ifndef EIBI_URL
#define EIBI_URL  "http://eibispace.de/dx/eibi.txt"
#endif

#define EIBI_TIMEOUT_MS  5000   // Connection timeout
#define EIBI_STALL_MS   15000   // Give up if no data arrives for this long

static const char *TAG = "eibi";

// Minimal self-closing wrapper around a stdio file
class EibiFile
{
public:
  EibiFile(const char *path, const char *mode) : fp(fopen(path, mode)) {}
  ~EibiFile() { close(); }
  EibiFile(const EibiFile &) = delete;
  EibiFile &operator=(const EibiFile &) = delete;

  explicit operator bool() const { return(fp!=NULL); }

  bool seek(size_t pos) { return(fp && fseek(fp, (long)pos, SEEK_SET)==0); }
  size_t read(void *buf, size_t len) { return(fp? fread(buf, 1, len, fp) : 0); }
  size_t write(const void *buf, size_t len) { return(fp? fwrite(buf, 1, len, fp) : 0); }

  size_t position() const
  {
    long pos = fp? ftell(fp) : -1;
    return(pos<0? 0 : (size_t)pos);
  }

  size_t size()
  {
    if(!fp) return(0);
    long cur = ftell(fp);
    if(cur<0 || fseek(fp, 0, SEEK_END)!=0) return(0);
    long end = ftell(fp);
    fseek(fp, cur, SEEK_SET);
    return(end<0? 0 : (size_t)end);
  }

  // Returns FALSE if flushing the written data to the file failed
  bool close()
  {
    bool ok = true;
    if(fp) ok = fclose(fp)==0;
    fp = NULL;
    return(ok);
  }

private:
  FILE *fp;
};

const BandLabel bandLabels[] =
{
  {  472,   479,  "630m (CW)"     },
  {  500,   518,  "NAVTEX"        },
  {  525,  1705,  "MW Broadcast"  },
  { 1600,  1800,  "Top Band"      },
  { 2300,  2495,  "120m BC"       },
  { 3200,  3400,  "90m BC"        },
  { 3500,  3800,  "80m (Amateur)" },
  { 3700,  3800,  "80m (SSB)"     },
  { 3900,  4000,  "75m BC"        },
  { 4720,  4750,  "60m (Amateur)" },
  { 4750,  4995,  "60m BC"        },
  { 5900,  6200,  "49m BC"        },
  { 7000,  7040,  "40m (CW)"      },
  { 7040,  7100,  "40m (DIGI)"    },
  { 7100,  7200,  "40m (SSB)"     },
  { 7200,  7600,  "41m BC"        },
  { 9400,  9900,  "31m BC"        },
  {10100, 10140,  "30m (CW)"      },
  {10136, 10140,  "30m (FT8)"     },
  {10140, 10150,  "30m (DIGI)"    },
  {11600, 12100,  "25m BC"        },
  {14000, 14070,  "20m (CW)"      },
  {14070, 14100,  "20m (DIGI/FT8)"},
  {14100, 14350,  "20m (SSB)"     },
  {15100, 15800,  "19m BC"        },
  {18068, 18110,  "17m (CW)"      },
  {18100, 18110,  "17m (FT8)"     },
  {18110, 18168,  "17m (SSB)"     },
  {17480, 17900,  "16m BC"        },
  {21000, 21070,  "15m (CW)"      },
  {21070, 21100,  "15m (DIGI/FT8)"},
  {21100, 21450,  "15m (SSB)"     },
  {21450, 21850,  "13m BC"        },
  {24890, 24915,  "12m (CW)"      },
  {24915, 24925,  "12m (FT8)"     },
  {24925, 24990,  "12m (SSB)"     },
  {26960, 27410,  "11m (CB)"      },
  {25670, 26100,  "11m BC"        },
  {28000, 28120,  "10m (CW)"      },
  {28120, 28190,  "10m (DIGI/FT8)"},
  {28200, 29700,  "10m (SSB/FM)"  },
  {26100, 26500,  "11m BC"        },
  {29600, 30000,  "9m BC"         }
};

// FIXME: this might be slow
bool eibiAvailable()
{
  struct stat st;
  return(stat(EIBI_PATH, &st)==0);
}

static bool entryIsNow(const StationSchedule *entry, int now)
{
  // Check if entry applies to all hours
  if(entry->start_h < 0 || entry->end_h < 0) return(true);

  // These are starting/ending times in minutes
  int start = entry->start_h * 60 + entry->start_m;
  int end   = entry->end_h * 60 + entry->end_m;

  // Check for inclusive schedule
  if(start <= end && now >= start && now <= end) return(true);

  // Check for exclusive schedule
  if(start > end && (now >= start || now <= end)) return(true);

  // Nope
  return(false);
}

const StationSchedule *eibiNext(uint16_t freq, uint8_t hour, uint8_t minute, size_t *offset)
{
  // Will return this static entry
  static StationSchedule entry;

  // Must have valid offset
  if(!offset) return(NULL);

  // If no valid offset yet, find some
  if(*offset==(size_t)-1) eibiLookup(freq, hour, minute, offset);

  // Open file with EIBI data
  EibiFile file(EIBI_PATH, "rb");
  if(!file) return(NULL);

  StationSchedule *result = NULL;
  int now = hour * 60 + minute;

  // Go to the starting offset
  if(!file.seek(*offset))
  {
    file.close();
    return(NULL);
  }

  while(file.read((uint8_t*)&entry, sizeof(entry)) == sizeof(entry))
  {
    if((entry.freq>freq) && entryIsNow(&entry, now))
    {
      *offset = file.position() - sizeof(entry);
      result = &entry;
      break;
    }
  }

  file.close();
  return(result);
}

const StationSchedule *eibiPrev(uint16_t freq, uint8_t hour, uint8_t minute, size_t *offset)
{
  // Will return this static entry
  static StationSchedule entry;

  // Must have valid offset
  if(!offset) return(NULL);

  // If no valid offset yet, find some
  if(*offset==(size_t)-1) eibiLookup(freq, hour, minute, offset);

  // Open file with EIBI data
  EibiFile file(EIBI_PATH, "rb");
  if(!file) return(NULL);

  StationSchedule *result = NULL;
  int now = hour * 60 + minute;

  for(size_t pos = *offset; file.seek(pos) ; pos -= sizeof(entry))
  {
    if(file.read((uint8_t*)&entry, sizeof(entry)) != sizeof(entry)) break;

    if((entry.freq<freq) && entryIsNow(&entry, now))
    {
      *offset = pos;
      result = &entry;
      break;
    }

    // Reached the beginning of the file
    if(pos < sizeof(entry)) break;
  }

  file.close();
  return(result);
}

const StationSchedule *eibiAtSameFreq(uint8_t hour, uint8_t minute, size_t *offset, bool same)
{
  // Will return this static entry
  static StationSchedule entry;

  // Must have valid offset
  if(!offset) return(NULL);

  // Open file with EIBI data
  EibiFile file(EIBI_PATH, "rb");
  if(!file) return(NULL);

  // Read current entry to get frequency
  StationSchedule e0;
  if(!file.seek(*offset) || file.read((uint8_t*)&e0, sizeof(e0)) != sizeof(e0))
  {
    file.close();
    return(NULL);
  }

  StationSchedule *result = NULL;
  int now = hour * 60 + minute;

  if(same && entryIsNow(&e0, now))
  {
    entry = e0;
    result = &entry;
    file.close();
    return(result);
  }

  while(file.read((uint8_t*)&entry, sizeof(entry)) == sizeof(entry))
  {
    if(entry.freq != e0.freq)
      break;
    else if(entryIsNow(&entry, now))
    {
      *offset = file.position() - sizeof(entry);
      result = &entry;
      break;
    }
  }

  file.close();
  return(result);
}

const StationSchedule *eibiLookup(uint16_t freq, uint8_t hour, uint8_t minute, size_t *offset)
{
  // Will return this static entry
  static StationSchedule entry;

  // Open file with EIBI data
  EibiFile file(EIBI_PATH, "rb");
  if(!file) return(NULL);

  // Set up binary search
  ssize_t total = file.size() / sizeof(entry);
  ssize_t left  = 0;
  ssize_t right = total;
  ssize_t match = -1;
  ssize_t mid;

  // Search for the frequency
  while(left <= right)
  {
    // Go to the middle entry
    mid = (left + right) / 2;

    // Save current offset, correcting for file size
    if(offset) *offset = (mid<0? 0 : mid>=total? total-1 : mid) * sizeof(entry);

    // Go to the middle entry
    if(!file.seek(mid * sizeof(entry)))
    {
      file.close();
      return(NULL);
    }

    // Read middle entry
    if(file.read((uint8_t*)&entry, sizeof(entry)) != sizeof(entry))
    {
      file.close();
      return(NULL);
    }

    // Compare frequency
    if(entry.freq < freq)
      left = mid + 1;
    else if(entry.freq > freq)
      right = mid - 1;
    else
    {
      match = mid;
      right = mid - 1;
    }
  }

  // Drop out if not found
  if(match < 0)
  {
    file.close();
    return(NULL);
  }

  // If found entry is not the same as the last read entry...
  if(match != mid)
  {
    // Read the right entry
    if(file.seek(match * sizeof(entry)))
      if(file.read((uint8_t*)&entry, sizeof(entry)) == sizeof(entry))
        mid = match;

    // Drop out if failed to read
    if(match != mid)
    {
      file.close();
      return(NULL);
    }
  }

  // This is our current time in minutes
  int now = hour * 60 + minute;

  // Keep reading entries from file
  do
  {
    // Report offset within the file
    if(offset) *offset = file.position() - sizeof(entry);

    // Match frequency
    if(entry.freq != freq) break;

    // Match time
    if(entryIsNow(&entry, now))
    {
      file.close();
      return(&entry);
    }
  }
  while(file.read((uint8_t*)&entry, sizeof(entry)) == sizeof(entry));

  // Not found
  file.close();
  return(NULL);
}

char replace_accented_char(char c)
{
  switch((unsigned char)c)
  {
    // Lowercase vowels with accents
    case 0xE1: case 0xE0: case 0xE2: case 0xE3: case 0xE4: return 'a'; // á, à, â, ã, ä
    case 0xE9: case 0xE8: case 0xEA: case 0xEB: return 'e';             // é, è, ê, ë
    case 0xED: case 0xEC: case 0xEE: case 0xEF: return 'i';            // í, ì, î, ï
    case 0xF3: case 0xF2: case 0xF4: case 0xF5: case 0xF6: return 'o';  // ó, ò, ô, õ, ö
    case 0xFA: case 0xF9: case 0xFB: case 0xFC: return 'u';             // ú, ù, û, ü
    // Uppercase vowels with accents
    case 0xC1: case 0xC0: case 0xC2: case 0xC3: case 0xC4: return 'A';  // Á, À, Â, Ã, Ä
    case 0xC9: case 0xC8: case 0xCA: case 0xCB: return 'E';             // É, È, Ê, Ë
    case 0xCD: case 0xCC: case 0xCE: case 0xCF: return 'I';             // Í, Ì, Î, Ï
    case 0xD3: case 0xD2: case 0xD4: case 0xD5: case 0xD6: return 'O';  // Ó, Ò, Ô, Õ, Ö
    case 0xDA: case 0xD9: case 0xDB: case 0xDC: return 'U';             // Ú, Ù, Û, Ü
    // Other special chars
    case 0xF1: return 'n';  // ñ
    case 0xD1: return 'N';  // Ñ
    case 0xE7: return 'c';  // ç
    case 0xC7: return 'C';  // Ç
    default: return c;      // No change
  }
}

static bool eibiParseLine(const char *line, StationSchedule &entry)
{
  char nameStr[sizeof(entry.name) + 1] = {0};
  char freqStr[15] = {0};
  char timeStr[10] = {0};
  char tmpCol[12]  = {0};
  char *p, *t;

  // Scan line for data
  if(sscanf(line, "%14c%9c%11c%24c", freqStr, timeStr, tmpCol, nameStr)<3)
    return(false);

  // Terminate found data
  freqStr[14] = '\0';
  timeStr[9] = '\0';
  nameStr[24] = '\0';

  // Parse frequency
  entry.freq = (uint16_t)atof(freqStr);
  if(!entry.freq) return(false);

  // Parse time
  int sh, sm, eh, em;
  if(sscanf(timeStr, "%2d%2d-%2d%2d", &sh, &sm, &eh, &em) != 4) return(false);
  entry.start_h = sh;
  entry.start_m = sm;
  entry.end_h   = eh;
  entry.end_m   = em;

  // Remove jammers
  if(strstr(nameStr, "Jammer")) return(false);

  // Remove leading and trailing white space from name
  for(p = nameStr ; *p==' ' || *p=='\t' ; ++p);
  for(t = p + strlen(p) - 1 ; t>=p && (*t==' ' || *t=='\t') ; *t--='\0');

  // Replace accented characters
  for (t = p; *t != '\0'; t++) {
    *t = replace_accented_char(*t);
  }

  // Copy name
  strncpy(entry.name, p, sizeof(entry.name) - 1);
  entry.name[sizeof(entry.name)-1] = '\0';

  // Done
  return(true);
}

// Process one line of the EiBi schedule and write the parsed entry, if
// any, to the file. Returns FALSE on write errors.
static bool eibiProcessLine(char *charBuf, int charCnt, EibiFile &file, int &lineCnt, int byteCnt)
{
  static const char *eibiMessage = "Loading EiBi Schedule";
  char *p, *t;

  // Remove whitespace
  charBuf[charCnt] = '\0';
  for(p = charBuf ; *p && (unsigned char)*p<=' ' ; ++p);
  t = charBuf + charCnt;
  while(t>p && (unsigned char)t[-1]<=' ') *--t = '\0';

  // If valid non-empty schedule line...
  if(t>p && isdigit((unsigned char)*p))
  {
    // Remove LFs
    for(t = p ; *t ; ++t)
      if(*t=='\r') *t = ' ';

    // If parsed a new entry...
    StationSchedule entry = {};
    if(eibiParseLine(p, entry))
    {
      // Write it to the output file
      if(file.write(&entry, sizeof(entry)) != sizeof(entry)) return(false);
      lineCnt++;

      if(!(lineCnt & 31))
      {
        char statusMessage[64];
        snprintf(statusMessage, sizeof(statusMessage), "... %d bytes, %d entries ...", byteCnt, lineCnt);
        drawScreen(eibiMessage, statusMessage);
      }
    }
  }

  return(true);
}

bool eibiLoadSchedule()
{
  static const char *eibiMessage = "Loading EiBi Schedule";

  // Need to be connected to the network
  consumeAbortPending();
  if(getWiFiStatus() < 2)
    return(false);

  drawScreen(eibiMessage, "Connecting...");

  // Open HTTP connection to EiBi site
  esp_http_client_config_t config = {};
  config.url = EIBI_URL;
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = EIBI_TIMEOUT_MS;
  config.disable_auto_redirect = true;  // Redirects are errors, as before
  config.buffer_size = 1024;
  config.buffer_size_tx = 512;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  config.crt_bundle_attach = esp_crt_bundle_attach; // Only used for HTTPS URLs
#endif

  esp_http_client_handle_t http = esp_http_client_init(&config);
  if(!http)
  {
    drawScreen(eibiMessage, "Failed connecting to EiBi!");
    return(false);
  }

  if(esp_http_client_open(http, 0)!=ESP_OK
  || esp_http_client_fetch_headers(http)<0
  || esp_http_client_get_status_code(http)!=200)
  {
    drawScreen(eibiMessage, "Failed connecting to EiBi!");
    esp_http_client_cleanup(http);
    return(false);
  }

  // Open file in the local flash file system
  EibiFile file(TEMP_PATH, "wb");
  if(!file)
  {
    drawScreen(eibiMessage, "Failed opening local storage!");
    esp_http_client_cleanup(http);
    return(false);
  }

  // Start loading data. Use a short read timeout, so that we can
  // see the abort request quickly.
  esp_http_client_set_timeout_ms(http, 250);

  int64_t totalLen = esp_http_client_get_content_length(http);
  int byteCnt = 0, lineCnt = 0, charCnt = 0;
  char charBuf[200];
  char rxBuf[512];
  bool failed = false;
  uint32_t lastData = nowMs();

  while(!failed && (totalLen<=0 || byteCnt<totalLen))
  {
    if(consumeAbortPending())
    {
      file.close();
      esp_http_client_cleanup(http);
      unlink(TEMP_PATH);
      drawScreen(eibiMessage, "CANCELED!");
      return(false);
    }

    int len = esp_http_client_read(http, rxBuf, sizeof(rxBuf));

    if(len==0)
    {
      // Connection closed by the server (or all data received)
      break;
    }
    else if(len<0)
    {
      // Nothing received within the read timeout is not an error by itself
      if(len==-ESP_ERR_HTTP_EAGAIN && (nowMs() - lastData)<EIBI_STALL_MS)
        continue;

      ESP_LOGE(TAG, "Download error %d", len);
      failed = true;
      break;
    }

    lastData = nowMs();

    for(int i=0 ; i<len ; i++)
    {
      char c = rxBuf[i];
      byteCnt++;

      if(c!='\n' && charCnt<(int)sizeof(charBuf)-1) charBuf[charCnt++] = c;
      else
      {
        if(!eibiProcessLine(charBuf, charCnt, file, lineCnt, byteCnt))
        {
          failed = true;
          break;
        }

        // Done with the current buffer, start a new one
        charCnt = 0;
        if(c!='\n') charBuf[charCnt++] = c;
      }
    }
  }

  // Done with HTTP connection
  esp_http_client_cleanup(http);

  // Connection dropped before all data arrived, or nothing useful received
  if(!failed && ((totalLen>0 && byteCnt<totalLen) || !lineCnt)) failed = true;

  // Done with the file, make sure all data has been written
  if(!file.close()) failed = true;

  if(failed)
  {
    unlink(TEMP_PATH);
    drawScreen(eibiMessage, "Failed loading EiBi data!");
    return(false);
  }

  // Move new schedule to its permanent place
  if(rename(TEMP_PATH, EIBI_PATH)!=0)
  {
    unlink(EIBI_PATH);
    rename(TEMP_PATH, EIBI_PATH);
  }

  // Success
  identifyFrequency(currentFrequency + currentBFO / 1000);
  drawScreen(eibiMessage, "DONE!");
  return(true);
}
