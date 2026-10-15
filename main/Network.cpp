#include "Common.h"
#include "Platform.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "mdns.h"

#define CONNECT_TIME  3000  // Time of inactivity to start connecting WiFi
#define WIFI_MULTI_TOTAL_TIMEOUT  30000
#define WIFI_MULTI_CONNECT_TIMEOUT 5000 // Timeout of one connection attempt
#define WIFI_MULTI_SCAN_TIMEOUT   10000 // Timeout of one network scan
#define WIFI_MAX_KNOWN_APS 3
#define WIFI_SCAN_MAX_RECORDS 40

#define NTP_SERVER        "pool.ntp.org"
#define NTP_INTERVAL_MS   (5 * 60 * 1000)
#define NTP_WAIT_TIME     15000  // How long to wait for the first SNTP reply

#define WEB_MAX_BODY      4096  // Maximum size of a POST body we accept

// TX power in units of 0.25 dBm, see Kconfig.projbuild
#ifndef CONFIG_ATS_WIFI_TX_POWER_QDBM
#define CONFIG_ATS_WIFI_TX_POWER_QDBM 68
#endif

static const char *TAG = "net";

// Bits of the WiFi event group
#define EV_GOT_IP    BIT0   // Station obtained an IP address
#define EV_STA_FAIL  BIT1   // Station connection attempt failed
#define EV_SCAN_DONE BIT2   // Scan completed

//
// Access Point (AP) mode settings
//
static const char *apSSID    = RECEIVER_NAME;
static const int   apChannel = 10;      // WiFi channel number (1..13)
static const bool  apHideMe  = false;   // TRUE: disable SSID broadcast
static const int   apClients = 3;       // Maximum simultaneous connected clients
// The AP has no password, which means open authentication

// These are our own access point (AP) addresses
#define AP_IP_A 10
#define AP_IP_B 1
#define AP_IP_C 1
#define AP_IP_D 1

// Requests may come from the web server task, ticks come from the main task
static volatile bool itIsTimeToWiFi = false; // TRUE: Need to connect to WiFi
static volatile uint32_t connectTime = 0;

// Settings. These are only accessed from the web server task once
// the server is running, and from the main task while it is not.
static std::string loginUsername;
static std::string loginPassword;
static bool wifiScanHidden = false;

// WiFi driver state
static bool wifiReady = false;                  // WiFi driver initialized
static esp_netif_t *staNetif = NULL;
static esp_netif_t *apNetif = NULL;
static EventGroupHandle_t wifiEvents = NULL;
static volatile bool staGotIp = false;          // Station has an IP address
static volatile bool staAutoReconnect = false;  // Reconnect when connection is lost

// Services state
static httpd_handle_t httpServer = NULL;
static bool mdnsUp = false;
static volatile bool ntpTimeSet = false;        // System time has been set by SNTP
static volatile bool ntpFresh = false;          // SNTP has replied since the network came up

// Known WiFi network
struct KnownAP
{
  std::string ssid;
  std::string pass;
};

static bool wifiEnsureInit();
static bool wifiInitAP();
static bool wifiConfigAP();
static bool wifiConnect();
static void webInit();
static void webStop();

static bool webSetConfig(const std::string &body);

static std::string webInputField(const char *name, const std::string &value, bool pass = false);
static const char *webStyleSheet();
static std::string webPage(const std::string &body);
static std::string webUtcOffsetSelector();
static std::string webThemeSelector();
static std::string webRadioPage();
static std::string webMemoryPage();
static std::string webConfigPage();

//
// printf() into a std::string
//
static std::string strFormat(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static std::string strFormat(const char *fmt, ...)
{
  char buf[96];
  va_list args;

  va_start(args, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if(len < 0) return(std::string());
  if((size_t)len < sizeof(buf)) return(std::string(buf, len));

  // Did not fit, retry with a big enough buffer
  std::string result(len + 1, '\0');
  va_start(args, fmt);
  vsnprintf(&result[0], result.size(), fmt, args);
  va_end(args);
  result.resize(len);
  return(result);
}

//
// Convert IPv4 address to text
//
static std::string ipToString(const esp_ip4_addr_t &ip)
{
  return(strFormat(IPSTR, IP2STR(&ip)));
}

//
// Delayed WiFi connection
//
void netRequestConnect()
{
  connectTime = nowMs();
  itIsTimeToWiFi = true;
}

void netTickTime()
{
  // Connect to WiFi if requested
  if(itIsTimeToWiFi && ((nowMs() - connectTime) > CONNECT_TIME))
  {
    itIsTimeToWiFi = false;
    netInit(wifiModeIdx);
    connectTime = nowMs();
  }
}

//
// Get number of stations connected to our access point
//
static int apStationCount()
{
  wifi_sta_list_t list;
  return(esp_wifi_ap_get_sta_list(&list)==ESP_OK? list.num : 0);
}

//
// Get current connection status
// (-1 - not connected, 0 - disabled, 1 - connected, 2 - connected to network)
//
int8_t getWiFiStatus()
{
  wifi_mode_t mode = WIFI_MODE_NULL;

  if(!wifiReady || esp_wifi_get_mode(&mode)!=ESP_OK)
    return(0);

  switch(mode)
  {
    case WIFI_MODE_NULL:
      return(0);
    case WIFI_MODE_AP:
      return(apStationCount()? 1 : -1);
    case WIFI_MODE_STA:
      return(staGotIp? 2 : -1);
    case WIFI_MODE_APSTA:
      return(staGotIp? 2 : apStationCount()? 1 : -1);
    default:
      return(-1);
  }
}

char *getWiFiIPAddress()
{
  static char ip[16];
  esp_netif_ip_info_t info;

  ip[0] = '\0';
  if(staGotIp && staNetif && esp_netif_get_ip_info(staNetif, &info)==ESP_OK)
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));

  return(ip);
}

//
// Stop SNTP
//
static void ntpStop()
{
  // This is safe to call when SNTP is not running
  esp_netif_sntp_deinit();
}

//
// Stop WiFi hardware.
//
// We keep the WiFi driver initialized (and its network interfaces created)
// and only stop it and set the mode to NULL, which switches the radio off.
// De-initializing the driver on every call would free about 50kB of RAM, but
// netInit()/netStop() are called often (mode changes, light sleep), and
// repeated esp_wifi_init()/deinit() cycles are not worth the fragmentation
// and the risk of a failed re-initialization.
//
void netStop()
{
  // Stop services using the network
  if(mdnsUp)
  {
    mdns_free();
    mdnsUp = false;
  }

  webStop();
  ntpStop();

  // Do not try to reconnect from now on
  staAutoReconnect = false;

  if(wifiReady)
  {
    wifi_mode_t mode = WIFI_MODE_NULL;

    // If WiFi hardware is running, shut it down
    if(esp_wifi_get_mode(&mode)==ESP_OK && mode!=WIFI_MODE_NULL)
    {
      // If network connection up, shut it down
      if(mode==WIFI_MODE_STA || mode==WIFI_MODE_APSTA)
        esp_wifi_disconnect();

      esp_wifi_stop();
      esp_wifi_set_mode(WIFI_MODE_NULL);
    }
  }

  staGotIp = false;
  if(wifiEvents) xEventGroupClearBits(wifiEvents, EV_GOT_IP | EV_STA_FAIL | EV_SCAN_DONE);
}

//
// Initialize WiFi network and services
//
void netInit(uint8_t netMode, bool showStatus)
{
  // Always disable WiFi first
  netStop();

  // Do not initialize WiFi if disabled
  if(netMode==NET_OFF) return;

  // Initialize the WiFi driver if not done yet
  if(!wifiEnsureInit()) return;

  wifi_mode_t mode;
  switch(netMode)
  {
    case NET_AP_ONLY:    mode = WIFI_MODE_AP;    break;
    case NET_AP_CONNECT: mode = WIFI_MODE_APSTA; break;
    default:             mode = WIFI_MODE_STA;   break; // No access point
  }

  bool wantAP = mode!=WIFI_MODE_STA;

  // The IP address of the access point has to be configured while it is down
  if(wantAP) wifiInitAP();

  if(esp_wifi_set_mode(mode)!=ESP_OK || (wantAP && !wifiConfigAP()) || esp_wifi_start()!=ESP_OK)
  {
    ESP_LOGE(TAG, "Failed starting WiFi");
    esp_wifi_set_mode(WIFI_MODE_NULL);
    return;
  }

  if(wantAP)
  {
    // Let user see connection status if successful
    esp_netif_ip_info_t info;
    std::string ip = "0.0.0.0";
    if(apNetif && esp_netif_get_ip_info(apNetif, &info)==ESP_OK) ip = ipToString(info.ip);

    drawScreen(
      (std::string("Use Access Point ") + apSSID).c_str(),
      ("IP : " + ip + " or atsmini.local").c_str()
    );

    if(showStatus) delayMs(2000);
  }

  // Initialize WiFi and try connecting to a network
  if(netMode>NET_AP_ONLY && wifiConnect())
  {
    // Let user see connection status if successful
    if(netMode!=NET_SYNC && showStatus) delayMs(2000);

    // NTP time updates will happen every 5 minutes
    sntp_set_sync_interval(NTP_INTERVAL_MS);
    ntpFresh = false;

    esp_sntp_config_t sntpConfig = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    sntpConfig.wait_for_sync = false;
    sntpConfig.sync_cb = [] (struct timeval *) { ntpTimeSet = true; ntpFresh = true; };
    if(esp_netif_sntp_init(&sntpConfig)!=ESP_OK)
      ESP_LOGE(TAG, "Failed starting SNTP");

    // Get NTP time from the network. SNTP works in the background, so wait
    // for the first reply (when time was never set, or when this is the only
    // reason for being connected) but not forever.
    clockReset();
    if(netMode==NET_SYNC || !ntpTimeSet)
    {
      uint32_t start = nowMs();
      while(!ntpFresh && (nowMs() - start)<NTP_WAIT_TIME && !consumeAbortPending())
        delayMs(100);
    }
    ntpSyncTime();
  }

  // If only connected to sync...
  if(netMode==NET_SYNC)
  {
    // Drop network connection
    netStop();
  }
  else
  {
    // Initialize web server for remote configuration
    webInit();

    // Initialize mDNS
    if(mdns_init()==ESP_OK)
    {
      mdnsUp = true;
      mdns_hostname_set("atsmini"); // Set the hostname to "atsmini.local"
      mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    else
      ESP_LOGE(TAG, "Failed starting mDNS");
  }
}

//
// Returns TRUE if NTP time is available
//
bool ntpIsAvailable()
{
  return(ntpTimeSet);
}

//
// Update NTP time and synchronize clock with NTP time
//
// The SNTP service updates the system time on its own every 5 minutes
// while the network is up, here we just pick it up.
//
bool ntpSyncTime()
{
  if(staGotIp && ntpTimeSet)
  {
    struct tm utc;
    time_t now = time(NULL);

    gmtime_r(&now, &utc);
    return(clockSet(utc.tm_hour, utc.tm_min, utc.tm_sec));
  }

  return(false);
}

//
// WiFi and IP events. These run in the system event loop task.
//
static void wifiEventHandler(void *, esp_event_base_t base, int32_t id, void *data)
{
  if(base==WIFI_EVENT)
  {
    switch(id)
    {
      case WIFI_EVENT_AP_START:
      case WIFI_EVENT_STA_START:
        esp_wifi_set_max_tx_power(CONFIG_ATS_WIFI_TX_POWER_QDBM);
        break;

      case WIFI_EVENT_SCAN_DONE:
        xEventGroupSetBits(wifiEvents, EV_SCAN_DONE);
        break;

      case WIFI_EVENT_STA_DISCONNECTED:
      {
        const wifi_event_sta_disconnected_t *evt = (const wifi_event_sta_disconnected_t *)data;

        staGotIp = false;
        xEventGroupClearBits(wifiEvents, EV_GOT_IP);

        // This is the result of our own esp_wifi_disconnect(), nothing to do
        if(evt && evt->reason==WIFI_REASON_ASSOC_LEAVE) break;

        // Either keep the connection alive or report a failed attempt
        if(staAutoReconnect) esp_wifi_connect();
        else xEventGroupSetBits(wifiEvents, EV_STA_FAIL);
        break;
      }

      default:
        break;
    }
  }
  else if(base==IP_EVENT)
  {
    switch(id)
    {
      case IP_EVENT_STA_GOT_IP:
        staGotIp = true;
        xEventGroupSetBits(wifiEvents, EV_GOT_IP);
        break;

      case IP_EVENT_STA_LOST_IP:
        staGotIp = false;
        xEventGroupClearBits(wifiEvents, EV_GOT_IP);
        break;

      default:
        break;
    }
  }
}

//
// Initialize the WiFi driver, network stack, and event loop. This is
// done once, the first time the network is needed.
//
static bool wifiEnsureInit()
{
  if(wifiReady) return(true);

  // These are fine to be already initialized by someone else
  esp_err_t err = esp_netif_init();
  if(err!=ESP_OK)
  {
    ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
    return(false);
  }

  err = esp_event_loop_create_default();
  if(err!=ESP_OK && err!=ESP_ERR_INVALID_STATE)
  {
    ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
    return(false);
  }

  if(!wifiEvents) wifiEvents = xEventGroupCreate();
  if(!wifiEvents) return(false);

  if(!staNetif) staNetif = esp_netif_create_default_wifi_sta();
  if(!apNetif)  apNetif  = esp_netif_create_default_wifi_ap();
  if(!staNetif || !apNetif)
  {
    ESP_LOGE(TAG, "Failed creating network interfaces");
    return(false);
  }

  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&config);
  if(err!=ESP_OK)
  {
    ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
    return(false);
  }

  // Do not store WiFi settings in the NVS
  esp_wifi_set_storage(WIFI_STORAGE_RAM);

  err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, NULL);
  if(err==ESP_OK)
    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, NULL);
  if(err!=ESP_OK)
  {
    ESP_LOGE(TAG, "Failed registering event handlers: %s", esp_err_to_name(err));
    esp_wifi_deinit();
    return(false);
  }

  wifiReady = true;
  return(true);
}

//
// Configure the IP address and DHCP server of the access point (AP).
// This has to be done before the WiFi is started.
//
static bool wifiInitAP()
{
  esp_netif_ip_info_t info;
  memset(&info, 0, sizeof(info));
  IP4_ADDR(&info.ip, AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D);
  IP4_ADDR(&info.gw, AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D);
  IP4_ADDR(&info.netmask, 255, 255, 255, 0);

  // DHCP server has to be stopped while the address is changed
  // (it is OK if it is already stopped)
  esp_netif_dhcps_stop(apNetif);

  esp_err_t err = esp_netif_set_ip_info(apNetif, &info);
  if(err!=ESP_OK) ESP_LOGE(TAG, "Failed setting AP address: %s", esp_err_to_name(err));

  // The DHCP server hands out addresses from the same subnet
  err = esp_netif_dhcps_start(apNetif);
  if(err!=ESP_OK && err!=ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
    ESP_LOGE(TAG, "Failed starting DHCP server: %s", esp_err_to_name(err));

  return(true);
}

//
// Set the access point (AP) parameters. The WiFi mode must include
// the AP when this is called.
//
static bool wifiConfigAP()
{
  wifi_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));

  cfg.ap.ssid_len        = strlen(apSSID);
  memcpy(cfg.ap.ssid, apSSID, cfg.ap.ssid_len);
  cfg.ap.channel         = apChannel;
  cfg.ap.authmode        = WIFI_AUTH_OPEN; // No password
  cfg.ap.ssid_hidden     = apHideMe? 1 : 0;
  cfg.ap.max_connection  = apClients;
  cfg.ap.beacon_interval = 100;

  esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
  if(err!=ESP_OK) ESP_LOGE(TAG, "Failed configuring AP: %s", esp_err_to_name(err));
  return(err==ESP_OK);
}

//
// Check for the user pressing the abort button, remember it in *aborted
//
static bool abortRequested(bool *aborted)
{
  if(!*aborted && consumeAbortPending()) *aborted = true;
  return(*aborted);
}

//
// Try connecting to the given network, returns TRUE on success
//
static bool wifiTryConnect(const KnownAP &ap, uint32_t timeout, bool *aborted)
{
  wifi_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));

  snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ap.ssid.c_str());
  snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", ap.pass.c_str());
  cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  cfg.sta.threshold.authmode = ap.pass.empty()? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
  cfg.sta.pmf_cfg.capable = true;
  cfg.sta.pmf_cfg.required = false;
  cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

  // Make sure there is no connection or connection attempt in progress.
  // The event for this has reason WIFI_REASON_ASSOC_LEAVE and is ignored.
  esp_wifi_disconnect();

  if(esp_wifi_set_config(WIFI_IF_STA, &cfg)!=ESP_OK) return(false);

  xEventGroupClearBits(wifiEvents, EV_GOT_IP | EV_STA_FAIL);
  if(esp_wifi_connect()!=ESP_OK) return(false);

  uint32_t start = nowMs();
  while((nowMs() - start) < timeout)
  {
    EventBits_t bits = xEventGroupWaitBits(
      wifiEvents, EV_GOT_IP | EV_STA_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(50)
    );

    if(bits & EV_GOT_IP) return(true);
    if(bits & EV_STA_FAIL) return(false);
    if(abortRequested(aborted)) break;
  }

  // Timed out or aborted, drop the connection attempt
  esp_wifi_disconnect();
  delayMs(100);
  return(false);
}

//
// Scan WiFi networks and connect to the strongest known one. If hidden
// networks are enabled, also try known networks which did not show up in
// the scan. Each connection attempt is limited by the given timeout.
// Returns TRUE when connected.
//
static bool wifiMultiRun(const std::vector<KnownAP> &aps, bool scanHidden, uint32_t timeout, bool *aborted)
{
  if(staGotIp) return(true);
  if(aps.empty()) return(false);

  std::vector<int> apRssi(aps.size(), -1000);
  std::vector<bool> seen(aps.size(), false);

  // Cannot scan while connecting
  esp_wifi_disconnect();

  // Scan the networks
  wifi_scan_config_t scan;
  memset(&scan, 0, sizeof(scan));
  scan.show_hidden = false;

  xEventGroupClearBits(wifiEvents, EV_SCAN_DONE);
  esp_err_t err = esp_wifi_scan_start(&scan, false);

  if(err!=ESP_OK)
    ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
  else
  {
    bool done = false;
    uint32_t start = nowMs();

    while(!done && (nowMs() - start) < WIFI_MULTI_SCAN_TIMEOUT)
    {
      done = !!(xEventGroupWaitBits(wifiEvents, EV_SCAN_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(50)) & EV_SCAN_DONE);
      if(!done && abortRequested(aborted)) break;
    }

    if(!done)
      esp_wifi_scan_stop();
    else
    {
      uint16_t count = 0;
      esp_wifi_scan_get_ap_num(&count);
      if(count > WIFI_SCAN_MAX_RECORDS) count = WIFI_SCAN_MAX_RECORDS;

      std::vector<wifi_ap_record_t> records(count? count : 1);
      // This also releases the memory held by the driver for the scan result
      if(esp_wifi_scan_get_ap_records(&count, records.data())==ESP_OK)
      {
        for(uint16_t i=0 ; i<count ; i++)
          for(size_t j=0 ; j<aps.size() ; j++)
            if(!strcmp((const char *)records[i].ssid, aps[j].ssid.c_str()))
            {
              seen[j] = true;
              if(records[i].rssi > apRssi[j]) apRssi[j] = records[i].rssi;
            }
      }
    }
  }

  if(*aborted) return(false);

  // Order candidates: visible networks by signal strength
  std::vector<size_t> order;
  for(size_t j=0 ; j<aps.size() ; j++)
    if(seen[j]) order.push_back(j);
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return(apRssi[a] > apRssi[b]); });

  // ...then hidden networks, if allowed
  if(scanHidden)
    for(size_t j=0 ; j<aps.size() ; j++)
      if(!seen[j]) order.push_back(j);

  for(size_t j : order)
  {
    ESP_LOGI(TAG, "Connecting to %s%s", aps[j].ssid.c_str(), seen[j]? "" : " (hidden)");
    if(wifiTryConnect(aps[j], timeout, aborted)) return(true);
    if(*aborted) break;
  }

  return(false);
}

//
// Connect to a WiFi network
//
static bool wifiConnect()
{
  static const char *status = "Connecting to WiFi network...";
  std::vector<KnownAP> aps;
  Prefs np;

  // Get the preferences
  np.begin("network", true);
  wifiScanHidden = np.getBool("wifiscanhidden", false);

  // Try connecting to known WiFi networks
  for(int j=0 ; j<WIFI_MAX_KNOWN_APS ; j++)
  {
    char nameSSID[16], namePASS[16];
    snprintf(nameSSID, sizeof(nameSSID), "wifissid%d", j+1);
    snprintf(namePASS, sizeof(namePASS), "wifipass%d", j+1);

    KnownAP ap;
    ap.ssid = np.getString(nameSSID, "");
    ap.pass = np.getString(namePASS, "");

    if(!ap.ssid.empty()) aps.push_back(ap);
  }

  // Done with preferences
  np.end();

  drawScreen(status);

  consumeAbortPending();
  bool connected = false;
  bool aborted = false;
  uint32_t start = nowMs();
  while(((nowMs() - start)<WIFI_MULTI_TOTAL_TIMEOUT) && !connected)
  {
    connected = wifiMultiRun(aps, wifiScanHidden, WIFI_MULTI_CONNECT_TIMEOUT, &aborted);

    if(abortRequested(&aborted))
    {
      esp_wifi_disconnect();
      connected = false;
      break;
    }

    if(!connected && ((nowMs() - start)<WIFI_MULTI_TOTAL_TIMEOUT))
      delayMs(1000);
  }

  // If failed connecting to WiFi network...
  if(!connected)
  {
    // Make sure the connection attempts are over
    esp_wifi_disconnect();
    // WiFi connection failed
    drawScreen(status, "No WiFi connection");
    // Done
    return(false);
  }
  else
  {
    // Keep the connection alive from now on
    staAutoReconnect = true;

    // Find out which network we are connected to
    wifi_ap_record_t info;
    std::string ssid;
    if(esp_wifi_sta_get_ap_info(&info)==ESP_OK) ssid = (const char *)info.ssid;

    esp_netif_ip_info_t ipInfo;
    std::string ip = "0.0.0.0";
    if(esp_netif_get_ip_info(staNetif, &ipInfo)==ESP_OK) ip = ipToString(ipInfo.ip);

    // WiFi connection succeeded
    drawScreen(
      ("Connected to WiFi network (" + ssid + ")").c_str(),
      ("IP : " + ip + " or atsmini.local").c_str()
    );
    // Done
    return(true);
  }
}

//
// HTTP helpers
//

// Decode %XX and + in a form value
static std::string webUrlDecode(const char *s, size_t len)
{
  std::string out;
  out.reserve(len);

  for(size_t i=0 ; i<len ; i++)
  {
    if(s[i]=='+')
      out += ' ';
    else if(s[i]=='%' && i+2<len && isxdigit((unsigned char)s[i+1]) && isxdigit((unsigned char)s[i+2]))
    {
      char hex[3] = { s[i+1], s[i+2], '\0' };
      out += (char)strtol(hex, NULL, 16);
      i += 2;
    }
    else
      out += s[i];
  }

  return(out);
}

// Parsed application/x-www-form-urlencoded body
class WebForm
{
public:
  WebForm(const std::string &body)
  {
    size_t pos = 0;

    while(pos <= body.size())
    {
      size_t end = body.find('&', pos);
      if(end==std::string::npos) end = body.size();

      if(end > pos)
      {
        size_t eq = body.find('=', pos);
        if(eq==std::string::npos || eq>end) eq = end;

        Field f;
        f.name  = webUrlDecode(body.c_str() + pos, eq - pos);
        f.value = eq<end? webUrlDecode(body.c_str() + eq + 1, end - eq - 1) : std::string();
        fields.push_back(f);
      }

      pos = end + 1;
    }
  }

  const std::string *get(const char *name) const
  {
    for(const Field &f : fields)
      if(f.name==name) return(&f.value);
    return(NULL);
  }

  bool has(const char *name) const { return(get(name)!=NULL); }

private:
  struct Field { std::string name, value; };
  std::vector<Field> fields;
};

// Read the whole body of a request
static esp_err_t webReadBody(httpd_req_t *req, std::string *body)
{
  size_t total = req->content_len;

  if(total > WEB_MAX_BODY)
  {
    httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, NULL);
    return(ESP_FAIL);
  }

  body->assign(total, '\0');

  size_t got = 0;
  int timeouts = 0;
  while(got < total)
  {
    int len = httpd_req_recv(req, &(*body)[got], total - got);

    if(len==HTTPD_SOCK_ERR_TIMEOUT)
    {
      // Retry a few times
      if(++timeouts < 3) continue;
      httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, NULL);
      return(ESP_FAIL);
    }

    // Connection closed or failed
    if(len <= 0) return(ESP_FAIL);

    got += len;
  }

  return(ESP_OK);
}

static esp_err_t webSendPage(httpd_req_t *req, const std::string &page)
{
  httpd_resp_set_type(req, "text/html");
  return(httpd_resp_send(req, page.c_str(), page.size()));
}

static esp_err_t webRedirect(httpd_req_t *req, const char *location)
{
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location);
  return(httpd_resp_send(req, NULL, 0));
}

static esp_err_t webRequestAuthentication(httpd_req_t *req)
{
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Login Required\"");
  return(httpd_resp_send(req, NULL, 0));
}

// Decode Base64, returns FALSE on invalid input
static bool webBase64Decode(const std::string &in, std::string *out)
{
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  uint32_t acc = 0;
  int bits = 0;

  out->clear();
  for(char c : in)
  {
    if(c=='=') break;
    const char *p = c? strchr(alphabet, c) : NULL;
    if(!p) return(false);

    acc = (acc << 6) | (uint32_t)(p - alphabet);
    bits += 6;
    if(bits >= 8)
    {
      bits -= 8;
      *out += (char)((acc >> bits) & 0xFF);
    }
  }

  return(true);
}

// Compare strings without leaking the position of the first difference
static bool webSameString(const std::string &a, const std::string &b)
{
  unsigned char diff = a.size()!=b.size();
  for(size_t i=0 ; i<a.size() && i<b.size() ; i++) diff |= a[i] ^ b[i];
  return(!diff);
}

// Returns TRUE if request is allowed to proceed, i.e. if login is not
// required or if the request has correct HTTP basic authentication
static bool webAuthorized(httpd_req_t *req)
{
  if(loginUsername.empty() || loginPassword.empty()) return(true);

  size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
  if(!len || len>512) return(false);

  std::string header(len + 1, '\0');
  if(httpd_req_get_hdr_value_str(req, "Authorization", &header[0], header.size())!=ESP_OK)
    return(false);
  header.resize(len);

  static const char prefix[] = "Basic ";
  if(header.compare(0, sizeof(prefix)-1, prefix)!=0) return(false);

  std::string credentials;
  if(!webBase64Decode(header.substr(sizeof(prefix)-1), &credentials)) return(false);

  size_t colon = credentials.find(':');
  if(colon==std::string::npos) return(false);

  bool userOK = webSameString(credentials.substr(0, colon), loginUsername);
  bool passOK = webSameString(credentials.substr(colon + 1), loginPassword);
  return(userOK && passOK);
}

//
// Request handlers. These run in the HTTP server task.
//
static esp_err_t webHandleRadio(httpd_req_t *req)
{
  return(webSendPage(req, webRadioPage()));
}

static esp_err_t webHandleMemory(httpd_req_t *req)
{
  return(webSendPage(req, webMemoryPage()));
}

static esp_err_t webHandleConfig(httpd_req_t *req)
{
  if(!webAuthorized(req)) return(webRequestAuthentication(req));
  return(webSendPage(req, webConfigPage()));
}

static esp_err_t webHandleSetConfig(httpd_req_t *req)
{
  if(!webAuthorized(req)) return(webRequestAuthentication(req));

  // Only POST carries the configuration
  if(req->method==HTTP_POST)
  {
    std::string body;
    esp_err_t err = webReadBody(req, &body);
    if(err!=ESP_OK) return(err);

    webSetConfig(body);
  }

  // Show config page again
  return(webRedirect(req, "/config"));
}

static esp_err_t webHandleNotFound(httpd_req_t *req, httpd_err_code_t)
{
  httpd_resp_set_status(req, "404 Not Found");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "Not found", HTTPD_RESP_USE_STRLEN);
  return(ESP_OK);
}

//
// Initialize internal web server
//
static void webInit()
{
  if(httpServer) return;

  // Get the login credentials
  Prefs np;
  np.begin("network", true);
  loginUsername = np.getString("loginusername", "");
  loginPassword = np.getString("loginpassword", "");
  wifiScanHidden = np.getBool("wifiscanhidden", false);
  np.end();

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port       = 80;
  config.stack_size        = 6144;
  config.task_priority     = 3;
  config.max_uri_handlers  = 8;
  config.max_open_sockets  = 4;
  config.max_req_hdr_len   = 2048;  // Browsers send long headers (cookies, etc)
  config.lru_purge_enable  = true;  // Drop the oldest connection when out of sockets
  config.recv_wait_timeout = 5;
  config.send_wait_timeout = 5;

  esp_err_t err = httpd_start(&httpServer, &config);
  if(err!=ESP_OK)
  {
    ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
    httpServer = NULL;
    return;
  }

  struct Route { const char *uri; esp_err_t (*handler)(httpd_req_t *); };
  static const Route routes[] =
  {
    { "/",          webHandleRadio     },
    { "/memory",    webHandleMemory    },
    { "/config",    webHandleConfig    },
    // This method saves configuration form contents
    { "/setconfig", webHandleSetConfig },
  };

  for(const Route &r : routes)
  {
    httpd_uri_t uri;
    memset(&uri, 0, sizeof(uri));
    uri.uri     = r.uri;
    uri.method  = (httpd_method_t)HTTP_ANY;
    uri.handler = r.handler;
    httpd_register_uri_handler(httpServer, &uri);
  }

  httpd_register_err_handler(httpServer, HTTPD_404_NOT_FOUND, webHandleNotFound);
}

static void webStop()
{
  if(httpServer)
  {
    httpd_stop(httpServer);
    httpServer = NULL;
  }
}

// Parse non-negative decimal value, returns -1 on failure
static int webParseIndex(const std::string &s)
{
  char *end = NULL;
  long value = strtol(s.c_str(), &end, 10);
  return((end==s.c_str() || value<0 || value>1000)? -1 : (int)value);
}

static bool webSetConfig(const std::string &body)
{
  uint32_t prefsSave = 0;
  WebForm form(body);
  Prefs np;

  // Start modifying preferences
  np.begin("network", false);

  // Save user name and password
  if(form.has("username") && form.has("password"))
  {
    loginUsername = *form.get("username");
    loginPassword = *form.get("password");

    np.putString("loginusername", loginUsername.c_str());
    np.putString("loginpassword", loginPassword.c_str());
  }

  // Save SSIDs and their passwords
  bool haveSSID = false;
  for(int j=0 ; j<WIFI_MAX_KNOWN_APS ; j++)
  {
    char nameSSID[16], namePASS[16];

    snprintf(nameSSID, sizeof(nameSSID), "wifissid%d", j+1);
    snprintf(namePASS, sizeof(namePASS), "wifipass%d", j+1);

    if(form.has(nameSSID) && form.has(namePASS))
    {
      const std::string &ssid = *form.get(nameSSID);
      const std::string &pass = *form.get(namePASS);
      np.putString(nameSSID, ssid.c_str());
      np.putString(namePASS, pass.c_str());
      haveSSID |= !ssid.empty() && !pass.empty();
    }
  }

  // Save hidden SSID scanning preference
  wifiScanHidden = form.has("wifiscanhidden");
  np.putBool("wifiscanhidden", wifiScanHidden);

  // Save time zone
  if(form.has("utcoffset"))
  {
    int idx = webParseIndex(*form.get("utcoffset"));
    if(idx>=0 && idx<getTotalUTCOffsets())
    {
      utcOffsetIdx = idx;
      clockRefreshTime();
      prefsSave |= SAVE_SETTINGS;
    }
  }

  // Save theme
  if(form.has("theme"))
  {
    int idx = webParseIndex(*form.get("theme"));
    if(idx>=0 && idx<getTotalThemes())
    {
      themeIdx = idx;
      prefsSave |= SAVE_SETTINGS;
    }
  }

  // Save scroll direction and menu zoom
  scrollDirection = form.has("scroll")? -1 : 1;
  zoomMenu        = form.has("zoom");
  prefsSave |= SAVE_SETTINGS;

  // Done with the preferences
  np.end();

  // Save preferences immediately
  prefsRequestSave(prefsSave, true);

  // If we are currently in AP mode, and infrastructure mode requested,
  // and there is at least one SSID / PASS pair, request network connection
  if(haveSSID && (wifiModeIdx>NET_AP_ONLY) && !staGotIp)
    netRequestConnect();

  return(true);
}

//
// Web page generation
//
static std::string htmlEscape(const std::string &value)
{
  std::string out;
  out.reserve(value.size());

  for(char c : value)
    switch(c)
    {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default:   out += c;        break;
    }

  return(out);
}

static std::string webInputField(const char *name, const std::string &value, bool pass)
{
  return(
    std::string("<INPUT TYPE='") + (pass? "PASSWORD":"TEXT") + "' NAME='" +
    name + "' VALUE='" + htmlEscape(value) + "'>"
  );
}

static const char *webStyleSheet()
{
  return
"BODY"
"{"
  "margin: 0;"
  "padding: 0;"
"}"
"H1"
"{"
  "text-align: center;"
"}"
"TABLE"
"{"
  "width: 100%;"
  "max-width: 768px;"
  "border: 0px;"
  "margin-left: auto;"
  "margin-right: auto;"
"}"
"TH, TD"
"{"
  "padding: 0.5em;"
"}"
"TH.HEADING"
"{"
  "background-color: #80A0FF;"
  "column-span: all;"
  "text-align: center;"
"}"
"TD.LABEL"
"{"
  "text-align: right;"
"}"
"INPUT[type=text], INPUT[type=password], SELECT"
"{"
  "width: 95%;"
  "padding: 0.5em;"
"}"
"INPUT[type=submit]"
"{"
  "width: 50%;"
  "padding: 0.5em 0;"
"}"
".CENTER"
"{"
  "text-align: center;"
"}"
;
}

static std::string webPage(const std::string &body)
{
  std::string page;
  page.reserve(body.size() + 1024);

  page +=
"<!DOCTYPE HTML>"
"<HTML>"
"<HEAD>"
  "<META CHARSET='UTF-8'>"
  "<META NAME='viewport' CONTENT='width=device-width, initial-scale=1.0'>"
  "<TITLE>ATS-Mini Config</TITLE>"
  "<STYLE>";
  page += webStyleSheet();
  page +=
  "</STYLE>"
"</HEAD>"
"<BODY STYLE='font-family: sans-serif;'>";
  page += body;
  page +=
"</BODY>"
"</HTML>"
;

  return(page);
}

static std::string webUtcOffsetSelector()
{
  std::string result;

  for(int i=0 ; i<getTotalUTCOffsets(); i++)
    result += strFormat(
      "<OPTION VALUE='%d'%s>%s</OPTION>",
      i, utcOffsetIdx==i? " SELECTED":"",
      utcOffsets[i].desc
    );

  return(result);
}

static std::string webThemeSelector()
{
  std::string result;

  for(int i=0 ; i<getTotalThemes(); i++)
    result += strFormat(
      "<OPTION VALUE='%d'%s>%s</OPTION>",
       i, themeIdx==i? " SELECTED":"", theme[i].name
    );

  return(result);
}

static std::string webRadioPage()
{
  std::string ip;
  std::string ssid;
  std::string freq = currentMode == FM?
    strFormat("%.2fMHz ", currentFrequency / 100.0)
  : strFormat("%.2fkHz ", currentFrequency + currentBFO / 1000.0);

  esp_netif_ip_info_t info;
  wifi_mode_t mode = WIFI_MODE_NULL;
  if(wifiReady) esp_wifi_get_mode(&mode);

  if(staGotIp && staNetif && esp_netif_get_ip_info(staNetif, &info)==ESP_OK)
  {
    wifi_ap_record_t ap;
    ip = ipToString(info.ip);
    if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK) ssid = (const char *)ap.ssid;
  }
  else
  {
    bool apUp = (mode==WIFI_MODE_AP) || (mode==WIFI_MODE_APSTA);
    ip = apUp && apNetif && esp_netif_get_ip_info(apNetif, &info)==ESP_OK? ipToString(info.ip) : "0.0.0.0";
    ssid = apSSID;
  }

  ssid = htmlEscape(ssid);

  return webPage(
std::string("<H1>ATS-Mini Pocket Receiver</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/memory'>Memory</A>&nbsp;|&nbsp;<A HREF='/config'>Config</A>"
"</P>"
"<TABLE COLUMNS=2>"
"<TR>"
  "<TD CLASS='LABEL'>IP Address</TD>"
  "<TD><A HREF='http://") + ip + "'>" + ip + "</A> (" + ssid + ")</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>MAC Address</TD>"
  "<TD>" + htmlEscape(getMACAddress()) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Firmware</TD>"
  "<TD>" + htmlEscape(getVersion(true)) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Band</TD>"
  "<TD>" + htmlEscape(getCurrentBand()->bandName) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Frequency</TD>"
  "<TD>" + freq + bandModeDesc[currentMode] + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Signal Strength</TD>"
  "<TD>" + strFormat("%u", (unsigned)rssi) + "dBuV</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Signal to Noise</TD>"
  "<TD>" + strFormat("%u", (unsigned)snr) + "dB</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Battery Voltage</TD>"
  "<TD>" + strFormat("%.2f", batteryMonitor()) + "V</TD>"
"</TR>"
"</TABLE>"
);
}

static std::string webMemoryPage()
{
  std::string items;

  for(int j=0 ; j<MEMORY_COUNT ; j++)
  {
    items += strFormat("<TR><TD CLASS='LABEL' WIDTH='10%%'>%02d</TD><TD>", j+1);

    if(!memories[j].freq)
      items += "&nbsp;---&nbsp;</TD></TR>";
    else
    {
      items += memories[j].mode == FM?
        strFormat("%.2fMHz ", memories[j].freq / 1000000.0)
      : strFormat("%.2fkHz ", memories[j].freq / 1000.0);
      items += bandModeDesc[memories[j].mode];
      items += "</TD></TR>";
    }
  }

  return webPage(
std::string("<H1>ATS-Mini Pocket Receiver Memory</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/'>Status</A>&nbsp;|&nbsp;<A HREF='/config'>Config</A>"
"</P>"
"<TABLE COLUMNS=2>") + items + "</TABLE>"
);
}

static std::string webConfigPage()
{
  Prefs np;

  np.begin("network", true);
  std::string ssid1 = np.getString("wifissid1", "");
  std::string pass1 = np.getString("wifipass1", "");
  std::string ssid2 = np.getString("wifissid2", "");
  std::string pass2 = np.getString("wifipass2", "");
  std::string ssid3 = np.getString("wifissid3", "");
  std::string pass3 = np.getString("wifipass3", "");
  bool scanHidden = np.getBool("wifiscanhidden", false);
  np.end();

  return webPage(
std::string("<H1>ATS-Mini Config</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/'>Status</A>"
  "&nbsp;|&nbsp;<A HREF='/memory'>Memory</A>"
"</P>"
"<FORM ACTION='/setconfig' METHOD='POST'>"
  "<TABLE COLUMNS=2>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 1</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>") + webInputField("wifissid1", ssid1) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass1", pass1, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 2</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid2", ssid2) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass2", pass2, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 3</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid3", ssid3) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass3", pass3, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>This Web UI Login Credentials</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Username</TD>"
    "<TD>" + webInputField("username", loginUsername) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("password", loginPassword, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>Settings</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Scan Hidden SSIDs</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='wifiscanhidden' VALUE='on'" +
    (scanHidden? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Time Zone</TD>"
    "<TD>"
      "<SELECT NAME='utcoffset'>" + webUtcOffsetSelector() + "</SELECT>"
    "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Theme</TD>"
    "<TD>"
      "<SELECT NAME='theme'>" + webThemeSelector() + "</SELECT>"
    "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Reverse Scrolling</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='scroll' VALUE='on'" +
    (scrollDirection<0? " CHECKED ":"") + "></TD>"
  "</TR>"
   "<TR>"
    "<TD CLASS='LABEL'>Zoomed Menu</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='zoom' VALUE='on'" +
    (zoomMenu? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>"
    "<INPUT TYPE='SUBMIT' VALUE='Save'>"
  "</TH></TR>"
  "</TABLE>"
"</FORM>"
);
}
