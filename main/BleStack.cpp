#include "BleStack.h"
#include "BleUartPeripheral.h"
#include "Platform.h"

#include <string.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// Defined by the NimBLE config store, but not declared in its public header
extern "C" void ble_store_config_init(void);

static const char* TAG = "BleStack";

// Switch the controller off between BLE sessions to save power.
// Disable ATS_BLE_DISABLE_CONTROLLER_WHEN_IDLE to keep the controller running instead, if switching it back
// on turns out to be unreliable on a given IDF version.
#ifdef CONFIG_ATS_BLE_DISABLE_CONTROLLER_WHEN_IDLE
#define BLE_DISABLE_CONTROLLER_WHEN_IDLE 1
#else
#define BLE_DISABLE_CONTROLLER_WHEN_IDLE 0
#endif

static constexpr uint32_t BLE_FIRST_SYNC_TIMEOUT_MS = 5000;
static constexpr uint32_t BLE_RESYNC_TIMEOUT_MS = 3000;
static constexpr uint32_t BLE_SYNC_POLL_MS = 10;

static bool stackInitialised = false;
static volatile bool hostSynced = false;
static uint8_t ownAddrType = BLE_OWN_ADDR_PUBLIC;

// TX power levels, the index is the esp_power_level_t value
static const int8_t txPowerDbm[] = {-24, -21, -18, -15, -12, -9, -6, -3, 0, 3, 6, 9, 12, 15, 18, 20};

// Highest level which does not exceed the requested power (the lowest one
// if even that is too high)
static esp_power_level_t txPowerLevelFor(int dbm)
{
  int level = 0;
  for(int i = 0; i < (int)(sizeof(txPowerDbm) / sizeof(txPowerDbm[0])); i++)
    if(txPowerDbm[i] <= dbm) level = i;
  return (esp_power_level_t)level;
}

static void applyTxPower()
{
  // The default type covers advertising, scanning, initiating and connections
  // which were not given a level of their own
  esp_power_level_t level = txPowerLevelFor(CONFIG_ATS_BLE_TX_POWER_DBM);
  esp_err_t err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, level);
  if(err != ESP_OK)
    ESP_LOGW(TAG, "Setting TX power failed: %s", esp_err_to_name(err));
}

static void onReset(int reason)
{
  ESP_LOGW(TAG, "Host reset, reason=%d", reason);
  hostSynced = false;
}

static void onSync()
{
  // The identity address is the public one, derived from the factory MAC
  int rc = ble_hs_util_ensure_addr(0);
  if(rc == 0) rc = ble_hs_id_infer_auto(0, &ownAddrType);
  if(rc != 0)
  {
    ESP_LOGE(TAG, "Address setup failed, rc=%d", rc);
    return;
  }

  hostSynced = true;
}

static void hostTask(void* param)
{
  (void)param;
  // Returns only when nimble_port_stop() is called, which we never do
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static bool waitForSync(uint32_t timeoutMs)
{
  for(uint32_t waited = 0; !hostSynced && waited < timeoutMs; waited += BLE_SYNC_POLL_MS)
    delayMs(BLE_SYNC_POLL_MS);
  return hostSynced;
}

static bool stackInit()
{
  esp_err_t err = nimble_port_init();
  if(err != ESP_OK)
  {
    ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
    return false;
  }

  ble_hs_cfg.reset_cb = onReset;
  ble_hs_cfg.sync_cb = onSync;
  // When the bond storage is full, overwrite the oldest bond
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

  // The GATT services can be added only before the host is started, so the
  // UART service is registered here regardless of the mode in use
  ble_svc_gap_init();
  ble_svc_gatt_init();
  int rc = bleUartGattInit();
  if(rc != 0)
  {
    ESP_LOGE(TAG, "GATT setup failed, rc=%d", rc);
    return false;
  }

  // Persists the bonds to NVS if CONFIG_BT_NIMBLE_NVS_PERSIST is set
  ble_store_config_init();

  nimble_port_freertos_init(hostTask);
  stackInitialised = true;
  return true;
}

bool bleStackEnable(const char* deviceName)
{
  uint32_t timeoutMs = BLE_RESYNC_TIMEOUT_MS;

  if(!stackInitialised)
  {
    if(!stackInit()) return false;
    timeoutMs = BLE_FIRST_SYNC_TIMEOUT_MS;
  }
  else if(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
  {
    esp_err_t err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(err != ESP_OK)
    {
      ESP_LOGE(TAG, "Controller enable failed: %s", esp_err_to_name(err));
      return false;
    }

    // The controller lost its state while it was off, make the host reset it
    // and synchronise again. This calls the reset and sync callbacks.
    hostSynced = false;
    ble_hs_sched_reset(BLE_HS_ECONTROLLER);
  }
  else if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED)
  {
    ESP_LOGE(TAG, "Unexpected controller state");
    return false;
  }

  if(!waitForSync(timeoutMs))
  {
    ESP_LOGE(TAG, "Host did not synchronise with the controller");
    bleStackDisable();
    return false;
  }

  applyTxPower();

  if(deviceName && *deviceName)
    ble_svc_gap_device_name_set(deviceName);

  return true;
}

void bleStackDisable()
{
#if BLE_DISABLE_CONTROLLER_WHEN_IDLE
  // Experimental attempt to stop consuming power
  // https://github.com/esp32-si4732/ats-mini/discussions/324#discussioncomment-17123023
  if(stackInitialised && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)
  {
    hostSynced = false;
    esp_bt_controller_disable();
  }
#endif
}

bool bleStackIsReady()
{
  return hostSynced;
}

uint8_t bleStackOwnAddrType()
{
  return ownAddrType;
}
