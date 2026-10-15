/*
 * Native ESP-IDF driver for the Silicon Labs Si4732 / Si4735 (SI473x).
 *
 * Derived from the SI4735 Arduino library, Copyright (c) 2019 Ricardo Lima
 * Caratti (PU2CLR), MIT License (https://github.com/pu2clr/SI4735). See the
 * full license text in include/si473x.h. Command and property semantics
 * follow Silicon Labs AN332.
 */
#include "si473x.h"

#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "si473x";

namespace
{
// Commands (AN332)
constexpr uint8_t POWER_UP        = 0x01;
constexpr uint8_t GET_REV         = 0x10;
constexpr uint8_t POWER_DOWN      = 0x11;
constexpr uint8_t SET_PROPERTY    = 0x12;
constexpr uint8_t FM_TUNE_FREQ    = 0x20;
constexpr uint8_t FM_SEEK_START   = 0x21;
constexpr uint8_t FM_TUNE_STATUS  = 0x22;
constexpr uint8_t FM_RSQ_STATUS   = 0x23;
constexpr uint8_t FM_AGC_STATUS   = 0x27;
constexpr uint8_t FM_AGC_OVERRIDE = 0x28;
constexpr uint8_t AM_TUNE_FREQ    = 0x40; // Also SSB_TUNE_FREQ with the SSB patch
constexpr uint8_t AM_SEEK_START   = 0x41;
constexpr uint8_t AM_TUNE_STATUS  = 0x42;
constexpr uint8_t AM_RSQ_STATUS   = 0x43;
constexpr uint8_t AM_AGC_STATUS   = 0x47;
constexpr uint8_t AM_AGC_OVERRIDE = 0x48;
constexpr uint8_t GPIO_CTL        = 0x80;
constexpr uint8_t GPIO_SET        = 0x81;

// Properties (AN332)
constexpr uint16_t SSB_BFO                  = 0x0100;
constexpr uint16_t SSB_MODE                 = 0x0101;
constexpr uint16_t REFCLK_FREQ              = 0x0201;
constexpr uint16_t REFCLK_PRESCALE          = 0x0202;
constexpr uint16_t FM_DEEMPHASIS            = 0x1100;
constexpr uint16_t FM_CHANNEL_FILTER        = 0x1102;
constexpr uint16_t FM_SEEK_BAND_BOTTOM      = 0x1400;
constexpr uint16_t FM_SEEK_BAND_TOP         = 0x1401;
constexpr uint16_t FM_SEEK_FREQ_SPACING     = 0x1402;
constexpr uint16_t FM_SEEK_TUNE_SNR_THRESHOLD  = 0x1403;
constexpr uint16_t FM_SEEK_TUNE_RSSI_THRESHOLD = 0x1404;
constexpr uint16_t FM_RDS_CONFIG            = 0x1502;
constexpr uint16_t AM_CHANNEL_FILTER        = 0x3102;
constexpr uint16_t AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN = 0x3103;
constexpr uint16_t AM_SOFT_MUTE_MAX_ATTENUATION  = 0x3302; // Same property number for SSB
constexpr uint16_t AM_SEEK_BAND_BOTTOM      = 0x3400;
constexpr uint16_t AM_SEEK_BAND_TOP         = 0x3401;
constexpr uint16_t AM_SEEK_FREQ_SPACING     = 0x3402;
constexpr uint16_t AM_SEEK_SNR_THRESHOLD    = 0x3403;
constexpr uint16_t AM_SEEK_RSSI_THRESHOLD   = 0x3404;
constexpr uint16_t RX_VOLUME                = 0x4000;
constexpr uint16_t RX_HARD_MUTE             = 0x4001;
constexpr uint16_t FM_DEBUG_DISABLE         = 0xFF00; // Undocumented, as used by the PU2CLR library

constexpr uint8_t SI473X_ANALOG_AUDIO = 0b00000101; // POWER_UP OPMODE
constexpr uint8_t XOSCEN_CRYSTAL = 1;               // POWER_UP XOSCEN: crystal oscillator
constexpr uint8_t XOSCEN_RCLK = 0;                  // POWER_UP XOSCEN: external RCLK

constexpr uint8_t STATUS_CTS = 0x80;
constexpr uint8_t STATUS_ERR = 0x40;

constexpr uint8_t kAddrSenLow  = 0x11;
constexpr uint8_t kAddrSenHigh = 0x63;

constexpr uint8_t kModeFm  = 0;
constexpr uint8_t kModeAm  = 1;
constexpr uint8_t kModeSsb = 2;

constexpr uint16_t kMinDelayWaitSendLoopUs = 300;       // Poll interval for CTS
constexpr uint16_t kMaxDelayAfterSetFrequency = 30;     // ms; seek start waits 4x this
constexpr int kXferTimeoutMs = 100;                     // Single I2C transfer
constexpr int64_t kCtsTimeoutUs = 1000 * 1000;          // Wait for CTS
constexpr int64_t kErrRetryUs = 100 * 1000;             // Re-read of a response that reports ERR

inline int64_t nowUs() { return esp_timer_get_time(); }
inline uint32_t millisNow() { return static_cast<uint32_t>(nowUs() / 1000); }
inline void delayUs(uint32_t us) { esp_rom_delay_us(us); }

inline void delayMs(uint32_t ms)
{
  TickType_t ticks = pdMS_TO_TICKS(ms);
  if (ms && !ticks) ticks = 1;
  vTaskDelay(ticks);
}

void configOutputPin(int pin)
{
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << pin;
  cfg.mode = GPIO_MODE_OUTPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
}
} // namespace

Si473x::~Si473x()
{
  detachDevice();
  if (bus_) i2c_del_master_bus(bus_);
}

// ---------------------------------------------------------------------------
// I2C plumbing
// ---------------------------------------------------------------------------

bool Si473x::begin(gpio_num_t sda, gpio_num_t scl)
{
  if (bus_) return true;

  i2c_master_bus_config_t cfg = {};
  cfg.i2c_port = I2C_NUM_0;
  cfg.sda_io_num = sda;
  cfg.scl_io_num = scl;
  cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  cfg.glitch_ignore_cnt = 7;
  cfg.flags.enable_internal_pullup = true;

  esp_err_t err = i2c_new_master_bus(&cfg, &bus_);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
    bus_ = nullptr;
    return false;
  }
  return true;
}

bool Si473x::attachDevice(uint8_t address)
{
  detachDevice();
  if (!bus_) return false;

  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = address;
  cfg.scl_speed_hz = i2cSpeedHz_;

  esp_err_t err = i2c_master_bus_add_device(bus_, &cfg, &dev_);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
    dev_ = nullptr;
    return false;
  }
  address_ = address;
  return true;
}

void Si473x::detachDevice()
{
  if (dev_)
  {
    i2c_master_bus_rm_device(dev_);
    dev_ = nullptr;
  }
}

void Si473x::setI2CFastModeCustom(long value)
{
  if (value <= 0) return;
  i2cSpeedHz_ = static_cast<uint32_t>(value);
  // The speed is a per-device setting in the IDF driver: re-add the device
  if (dev_) attachDevice(address_);
}

bool Si473x::writeBytes(const uint8_t *data, size_t len)
{
  if (!dev_) return false;
  esp_err_t err = i2c_master_transmit(dev_, data, len, kXferTimeoutMs);
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "I2C write (cmd 0x%02x) failed: %s", data[0], esp_err_to_name(err));
    return false;
  }
  return true;
}

bool Si473x::readBytes(uint8_t *data, size_t len)
{
  if (!dev_) return false;
  return i2c_master_receive(dev_, data, len, kXferTimeoutMs) == ESP_OK;
}

// Polls the status byte until CTS (clear to send) is set
bool Si473x::waitToSend()
{
  const int64_t start = nowUs();
  do
  {
    delayUs(kMinDelayWaitSendLoopUs);
    uint8_t status = 0;
    if (readBytes(&status, 1) && (status & STATUS_CTS)) return true;
  } while (nowUs() - start < kCtsTimeoutUs);

  ESP_LOGW(TAG, "Timeout waiting for CTS");
  return false;
}

// Reads a command response of len bytes (first byte is the status byte).
// With retryOnError, re-reads while the device reports ERR (bounded).
// The destination is only modified on success.
bool Si473x::readResponse(uint8_t *data, size_t len, bool retryOnError)
{
  uint8_t buf[16];
  if (len > sizeof(buf)) return false;

  const int64_t start = nowUs();
  for (;;)
  {
    if (!waitToSend() || !readBytes(buf, len)) return false;
    if (!retryOnError || !(buf[0] & STATUS_ERR)) break;
    if (nowUs() - start > kErrRetryUs)
    {
      ESP_LOGW(TAG, "Device keeps reporting ERR");
      return false;
    }
  }

  memcpy(data, buf, len);
  return true;
}

void Si473x::sendProperty(uint16_t propertyNumber, uint16_t parameter)
{
  const uint8_t cmd[] = {
    SET_PROPERTY, 0x00,
    static_cast<uint8_t>(propertyNumber >> 8), static_cast<uint8_t>(propertyNumber),
    static_cast<uint8_t>(parameter >> 8), static_cast<uint8_t>(parameter)
  };
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
  delayUs(550);
}

// ---------------------------------------------------------------------------
// Reset, probe, power control
// ---------------------------------------------------------------------------

void Si473x::reset()
{
  configOutputPin(resetPin_);
  delayMs(10);
  gpio_set_level(static_cast<gpio_num_t>(resetPin_), 0);
  delayMs(10);
  gpio_set_level(static_cast<gpio_num_t>(resetPin_), 1);
  delayMs(10);
}

int16_t Si473x::getDeviceI2CAddress(uint8_t resetPin)
{
  resetPin_ = resetPin;
  reset();

  if (!bus_) return 0;

  static const uint8_t addresses[] = {kAddrSenLow, kAddrSenHigh};
  for (uint8_t address : addresses)
  {
    if (i2c_master_probe(bus_, address, kXferTimeoutMs) == ESP_OK)
      return attachDevice(address) ? address : 0;
  }
  return 0;
}

void Si473x::setAudioMuteMcuPin(int8_t pin)
{
  audioMuteMcuPin_ = pin;
  if (pin >= 0) configOutputPin(pin);
}

void Si473x::setHardwareAudioMute(bool on)
{
  if (audioMuteMcuPin_ < 0) return;
  gpio_set_level(static_cast<gpio_num_t>(audioMuteMcuPin_), on ? 1 : 0);
  delayUs(300);
}

void Si473x::setPowerUp(uint8_t CTSIEN, uint8_t GPO2OEN, uint8_t PATCH, uint8_t XOSCEN, uint8_t FUNC, uint8_t OPMODE)
{
  powerUp_[0] = (FUNC & 0x0F) | ((XOSCEN & 1) << 4) | ((PATCH & 1) << 5) | ((GPO2OEN & 1) << 6) | ((CTSIEN & 1) << 7);
  powerUp_[1] = OPMODE;
  clockType_ = XOSCEN;

  if (FUNC == 0)
  {
    currentTune_ = FM_TUNE_FREQ;
    tuneFreeze_ = 1;
  }
  else
  {
    currentTune_ = AM_TUNE_FREQ;
    tuneFreeze_ = 0;
  }
  antCapH_ = 0;
  antCapL_ = 1;
}

void Si473x::radioPowerUp()
{
  const uint8_t cmd[] = {POWER_UP, powerUp_[0], powerUp_[1]};
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
  waitToSend();
  delayMs(maxDelayAfterPowerUp_);

  // Un-mute external audio hardware
  setHardwareAudioMute(false);

  if (clockType_ == XOSCEN_RCLK)
  {
    setRefClock(refClock_);
    setRefClockPrescaler(refClockPrescale_);
  }
}

void Si473x::powerDown()
{
  setHardwareAudioMute(true);
  const uint8_t cmd[] = {POWER_DOWN};
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
  delayUs(2500);
}

void Si473x::getFirmware()
{
  uint8_t resp[9] = {};
  const uint8_t cmd[] = {GET_REV};
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
  if (readResponse(resp, sizeof(resp), true))
    ESP_LOGI(TAG, "Si47%02u firmware %c.%c, chip revision %c", resp[1], resp[2], resp[3], resp[8]);
}

void Si473x::setRefClock(uint16_t refclk)
{
  sendProperty(REFCLK_FREQ, refclk);
  refClock_ = refclk;
}

void Si473x::setRefClockPrescaler(uint16_t prescale)
{
  sendProperty(REFCLK_PRESCALE, prescale);
  refClockPrescale_ = prescale;
}

void Si473x::setup(uint8_t resetPin, uint8_t ctsIntEnable, uint8_t defaultFunction,
                   uint8_t audioMode, uint8_t clockType, uint8_t gpo2Enable)
{
  resetPin_ = resetPin;
  ctsIntEnable_ = (ctsIntEnable != 0) ? 1 : 0;
  gpo2Enable_ = gpo2Enable;
  audioMode_ = audioMode;

  setPowerUp(ctsIntEnable, gpo2Enable, 0, clockType, defaultFunction, audioMode);

  // If an external circuit is used to mute the audio, mute it now
  setHardwareAudioMute(true);

  reset();
  radioPowerUp();
  setVolume(30);
  getFirmware();
}

void Si473x::setup(uint8_t resetPin, uint8_t defaultFunction)
{
  setup(resetPin, 0, defaultFunction, SI473X_ANALOG_AUDIO, XOSCEN_CRYSTAL, 0);
  delayMs(250);
}

// ---------------------------------------------------------------------------
// Mode selection and tuning
// ---------------------------------------------------------------------------

void Si473x::setAM()
{
  if (lastMode_ != kModeAm)
  {
    powerDown();
    setPowerUp(ctsIntEnable_, 0, 0, clockType_, kModeAm, audioMode_);
    radioPowerUp();
    setAvcAmMaxGain(currentAvcAmMaxGain_);
    setVolume(volume_);
  }
  sideband_ = 0;
  lastMode_ = kModeAm;
}

void Si473x::setFM()
{
  powerDown();
  setPowerUp(ctsIntEnable_, gpo2Enable_, 0, clockType_, kModeFm, audioMode_);
  radioPowerUp();
  setVolume(volume_);
  sideband_ = 0;
  disableFmDebug();
  lastMode_ = kModeFm;
}

void Si473x::setSSB(uint8_t usblsb)
{
  // The patch must already have been loaded (loadPatch). If the device is
  // already powered up this POWER_UP is ignored by the chip, keeping the patch.
  setPowerUp(ctsIntEnable_, 0, 0, clockType_, kModeAm, audioMode_);
  radioPowerUp();
  setVolume(volume_);
  sideband_ = usblsb;
  lastMode_ = kModeSsb;
}

void Si473x::setAM(uint16_t fromFreq, uint16_t toFreq, uint16_t initialFreq, uint16_t step)
{
  step_ = step;
  if (initialFreq < fromFreq || initialFreq > toFreq) initialFreq = fromFreq;
  setAM();
  workFreq_ = initialFreq;
  setFrequency(workFreq_);
}

void Si473x::setFM(uint16_t fromFreq, uint16_t toFreq, uint16_t initialFreq, uint16_t step)
{
  step_ = step;
  if (initialFreq < fromFreq || initialFreq > toFreq) initialFreq = fromFreq;
  setFM();
  workFreq_ = initialFreq;
  setFrequency(workFreq_);
}

void Si473x::setSSB(uint16_t fromFreq, uint16_t toFreq, uint16_t initialFreq, uint16_t step, uint8_t usblsb)
{
  step_ = step;
  if (initialFreq < fromFreq || initialFreq > toFreq) initialFreq = fromFreq;
  setSSB(usblsb);
  workFreq_ = initialFreq;
  setFrequency(workFreq_);
}

void Si473x::disableFmDebug()
{
  sendProperty(FM_DEBUG_DISABLE, 0x0000);
  delayUs(2500);
}

void Si473x::setFrequency(uint16_t freq)
{
  const bool am = (currentTune_ == AM_TUNE_FREQ);

  // ARG1: bit 0 FAST, bit 1 FREEZE (FM only), bits 7:6 USBLSB (SSB only)
  uint8_t arg1 = 0x01;
  if (sideband_)
    arg1 |= static_cast<uint8_t>(sideband_ << 6);
  else if (tuneFreeze_)
    arg1 |= 0x02;

  const uint8_t cmd[] = {
    currentTune_, arg1,
    static_cast<uint8_t>(freq >> 8), static_cast<uint8_t>(freq),
    antCapH_, antCapL_
  };

  waitToSend();
  writeBytes(cmd, am ? 6 : 5); // ANTCAPL is only sent for AM/SSB
  waitToSend();
  workFreq_ = freq;
  delayMs(maxDelaySetFrequency_);
}

void Si473x::setTuneFrequencyAntennaCapacitor(uint16_t capacitor)
{
  if (currentTune_ != AM_TUNE_FREQ)
  {
    antCapH_ = (capacitor <= 191) ? static_cast<uint8_t>(capacitor) : 0;
  }
  else if (capacitor <= 6143)
  {
    tuneFreeze_ = 0;
    antCapH_ = static_cast<uint8_t>(capacitor >> 8);
    antCapL_ = static_cast<uint8_t>(capacitor);
  }
  setFrequency(workFreq_);
}

void Si473x::getStatus(uint8_t INTACK, uint8_t CANCEL)
{
  const uint8_t cmd[] = {
    static_cast<uint8_t>(currentTune_ == FM_TUNE_FREQ ? FM_TUNE_STATUS : AM_TUNE_STATUS),
    static_cast<uint8_t>((INTACK & 1) | ((CANCEL & 1) << 1))
  };
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
  readResponse(tuneStatus_, sizeof(tuneStatus_), true);
  waitToSend();
}

uint16_t Si473x::getFrequency()
{
  getStatus(0, 1);
  workFreq_ = static_cast<uint16_t>((tuneStatus_[2] << 8) | tuneStatus_[3]);
  return workFreq_;
}

uint16_t Si473x::getAntennaTuningCapacitor() const
{
  if (currentTune_ == FM_TUNE_FREQ) return tuneStatus_[7];
  return static_cast<uint16_t>((tuneStatus_[6] << 8) | tuneStatus_[7]);
}

// ---------------------------------------------------------------------------
// Seek
// ---------------------------------------------------------------------------

void Si473x::seekStation(uint8_t SEEKUP, uint8_t WRAP)
{
  const bool fm = (currentTune_ == FM_TUNE_FREQ);
  // ARG1: bit 3 SEEKUP, bit 2 WRAP. AM additionally takes ARG2/3 = 0 and the
  // antenna capacitor in ARG4/5 (0, or 1 above 1800 kHz to select the SW path)
  const uint8_t cmd[] = {
    static_cast<uint8_t>(fm ? FM_SEEK_START : AM_SEEK_START),
    static_cast<uint8_t>(((SEEKUP & 1) << 3) | ((WRAP & 1) << 2)),
    0x00, 0x00, 0x00,
    static_cast<uint8_t>(workFreq_ > 1800 ? 1 : 0)
  };

  waitToSend();
  writeBytes(cmd, fm ? 2 : 6);
  delayMs(kMaxDelayAfterSetFrequency << 2);
}

void Si473x::seekStationProgress(void (*showFunc)(uint16_t f), bool (*stopSeeking)(), uint8_t up_down)
{
  // The seek command does not work in SSB mode
  if (lastMode_ == kModeSsb) return;

  const uint32_t startMs = millisNow();

  seekStation(up_down, 0);
  do
  {
    delayMs(maxDelaySetFrequency_);
    getStatus(0, 0);
    delayMs(maxDelaySetFrequency_);

    workFreq_ = static_cast<uint16_t>((tuneStatus_[2] << 8) | tuneStatus_[3]);
    if (showFunc) showFunc(workFreq_);
    if (stopSeeking && stopSeeking()) return;

    const bool valid = tuneStatus_[1] & 0x01;
    const bool bandLimit = tuneStatus_[1] & 0x80;
    if (valid || bandLimit) break;
  } while (static_cast<long>(millisNow() - startMs) < maxSeekTime_);
}

void Si473x::setSeekAmLimits(uint16_t bottom, uint16_t top)
{
  sendProperty(AM_SEEK_BAND_BOTTOM, bottom);
  sendProperty(AM_SEEK_BAND_TOP, top);
}

void Si473x::setSeekFmLimits(uint16_t bottom, uint16_t top)
{
  sendProperty(FM_SEEK_BAND_BOTTOM, bottom);
  sendProperty(FM_SEEK_BAND_TOP, top);
}

void Si473x::setSeekAmSpacing(uint16_t spacing) { sendProperty(AM_SEEK_FREQ_SPACING, spacing); }
void Si473x::setSeekFmSpacing(uint16_t spacing) { sendProperty(FM_SEEK_FREQ_SPACING, spacing); }
void Si473x::setSeekAmRssiThreshold(uint16_t value) { sendProperty(AM_SEEK_RSSI_THRESHOLD, value); }
void Si473x::setSeekFmRssiThreshold(uint16_t value) { sendProperty(FM_SEEK_TUNE_RSSI_THRESHOLD, value); }
void Si473x::setSeekAmSNRThreshold(uint16_t value) { sendProperty(AM_SEEK_SNR_THRESHOLD, value); }
void Si473x::setSeekFmSNRThreshold(uint16_t value) { sendProperty(FM_SEEK_TUNE_SNR_THRESHOLD, value); }

// ---------------------------------------------------------------------------
// Signal quality, AGC, audio
// ---------------------------------------------------------------------------

void Si473x::getCurrentReceivedSignalQuality()
{
  const bool fm = (currentTune_ == FM_TUNE_FREQ);
  const uint8_t cmd[] = {static_cast<uint8_t>(fm ? FM_RSQ_STATUS : AM_RSQ_STATUS), 0x00 /* INTACK = 0 */};
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
  // FM: 8 bytes (RSSI, SNR, MULT, FREQOFF in 4..7); AM/SSB: 6 bytes (RSSI, SNR in 4..5)
  readResponse(rsq_, fm ? 8 : 6, false);
}

void Si473x::getAutomaticGainControl()
{
  const uint8_t cmd[] = {static_cast<uint8_t>(currentTune_ == FM_TUNE_FREQ ? FM_AGC_STATUS : AM_AGC_STATUS)};
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
  readResponse(agcStatus_, sizeof(agcStatus_), true);
}

void Si473x::setAutomaticGainControl(uint8_t AGCDIS, uint8_t AGCIDX)
{
  const uint8_t cmd[] = {
    static_cast<uint8_t>(currentTune_ == FM_TUNE_FREQ ? FM_AGC_OVERRIDE : AM_AGC_OVERRIDE),
    static_cast<uint8_t>(AGCDIS & 1), AGCIDX
  };
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
  waitToSend();
}

void Si473x::setAvcAmMaxGain(uint8_t gain)
{
  if (gain < 12 || gain > 90) return;
  currentAvcAmMaxGain_ = gain;
  sendProperty(AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN, static_cast<uint16_t>(gain * 340));
}

void Si473x::setVolume(uint8_t volume)
{
  sendProperty(RX_VOLUME, volume);
  volume_ = volume;
}

void Si473x::setAudioMute(bool off)
{
  sendProperty(RX_HARD_MUTE, off ? 3 : 0); // 3 = mute both channels, 0 = unmute
}

void Si473x::setBandwidth(uint8_t AMCHFLT, uint8_t AMPLFLT)
{
  // Only for AM/SSB
  if (currentTune_ != AM_TUNE_FREQ || AMCHFLT > 6) return;

  // AMCHFLT in bits 3:0, AMPLFLT (power line noise rejection) in bit 8
  sendProperty(AM_CHANNEL_FILTER, static_cast<uint16_t>((AMCHFLT & 0x0F) | ((AMPLFLT & 1) << 8)));
  waitToSend();
}

void Si473x::setFmBandwidth(uint8_t filter_value) { sendProperty(FM_CHANNEL_FILTER, filter_value); }
void Si473x::setFMDeEmphasis(uint8_t parameter) { sendProperty(FM_DEEMPHASIS, parameter); }
void Si473x::setAmSoftMuteMaxAttenuation(uint8_t smattn) { sendProperty(AM_SOFT_MUTE_MAX_ATTENUATION, smattn); }
void Si473x::setSsbSoftMuteMaxAttenuation(uint8_t smattn) { sendProperty(AM_SOFT_MUTE_MAX_ATTENUATION, smattn); }

void Si473x::setGpioCtl(uint8_t GPO1OEN, uint8_t GPO2OEN, uint8_t GPO3OEN)
{
  const uint8_t cmd[] = {
    GPIO_CTL,
    static_cast<uint8_t>(((GPO1OEN & 1) << 1) | ((GPO2OEN & 1) << 2) | ((GPO3OEN & 1) << 3))
  };
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
}

void Si473x::setGpio(uint8_t GPO1LEVEL, uint8_t GPO2LEVEL, uint8_t GPO3LEVEL)
{
  const uint8_t cmd[] = {
    GPIO_SET,
    static_cast<uint8_t>(((GPO1LEVEL & 1) << 1) | ((GPO2LEVEL & 1) << 2) | ((GPO3LEVEL & 1) << 3))
  };
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
}

// ---------------------------------------------------------------------------
// SSB
// ---------------------------------------------------------------------------

void Si473x::setSsbModeBits(uint8_t shift, uint16_t mask, uint8_t value)
{
  ssbModeProp_ = static_cast<uint16_t>((ssbModeProp_ & ~(mask << shift)) | ((value & mask) << shift));
  sendProperty(SSB_MODE, ssbModeProp_);
}

// SSB_MODE: AUDIOBW 3:0, SBCUTFLT 7:4, AVC_DIVIDER 11:8, AVCEN 12, SMUTESEL 13, DSP_AFCDIS 15
void Si473x::setSSBConfig(uint8_t AUDIOBW, uint8_t SBCUTFLT, uint8_t AVC_DIVIDER, uint8_t AVCEN,
                          uint8_t SMUTESEL, uint8_t DSP_AFCDIS)
{
  // Only AM/SSB
  if (currentTune_ == FM_TUNE_FREQ) return;

  ssbModeProp_ = static_cast<uint16_t>((AUDIOBW & 0x0F) | ((SBCUTFLT & 0x0F) << 4) |
                                       ((AVC_DIVIDER & 0x0F) << 8) | ((AVCEN & 1) << 12) |
                                       ((SMUTESEL & 1) << 13) | ((DSP_AFCDIS & 1) << 15));
  sendProperty(SSB_MODE, ssbModeProp_);
}

void Si473x::setSSBAudioBandwidth(uint8_t AUDIOBW) { setSsbModeBits(0, 0x0F, AUDIOBW); }
void Si473x::setSSBSidebandCutoffFilter(uint8_t SBCUTFLT) { setSsbModeBits(4, 0x0F, SBCUTFLT); }
void Si473x::setSSBAutomaticVolumeControl(uint8_t AVCEN) { setSsbModeBits(12, 0x01, AVCEN); }

void Si473x::setSSBBfo(int offset)
{
  // Only AM/SSB
  if (currentTune_ == FM_TUNE_FREQ) return;

  // Signed 16 bit offset in Hz
  sendProperty(SSB_BFO, static_cast<uint16_t>(offset));
}

// Power up in "query library ID" mode (resets the chip, dropping any patch)
void Si473x::queryLibraryId()
{
  uint8_t resp[8];
  powerDown();

  // FUNC = 15 (query library ID), XOSCEN = 1, no interrupts, boot normally
  const uint8_t cmd[] = {POWER_UP, 0b00011111, SI473X_ANALOG_AUDIO};
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
  readResponse(resp, sizeof(resp), true);
  delayUs(2500);
}

// Power up for patch loading: FUNC = 1 (AM), XOSCEN = 1, PATCH = 1
void Si473x::patchPowerUp()
{
  const uint8_t cmd[] = {POWER_UP, 0b00110001, SI473X_ANALOG_AUDIO};
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
  delayMs(maxDelayAfterPowerUp_);

  // The device is now running its AM function, so AM/SSB-only commands
  // (e.g. the SSB_MODE configuration sent at the end of loadPatch) apply
  currentTune_ = AM_TUNE_FREQ;
}

// The patch is a sequence of 8-byte commands. After each one, wait for CTS
// and require a clean 0x80 status.
bool Si473x::downloadPatch(const uint8_t *content, uint16_t size)
{
  if (size % 8) ESP_LOGW(TAG, "Patch size %u is not a multiple of 8, ignoring tail", size);

  for (uint32_t offset = 0; offset + 8 <= size; offset += 8)
  {
    if (!writeBytes(content + offset, 8)) return false;
    delayUs(kMinDelayWaitSendLoopUs);

    const int64_t start = nowUs();
    uint8_t status = 0;
    for (;;)
    {
      delayUs(150);
      if (readBytes(&status, 1) && (status & STATUS_CTS)) break;
      if (nowUs() - start > kCtsTimeoutUs)
      {
        ESP_LOGE(TAG, "Patch download: timeout at offset %u", static_cast<unsigned>(offset));
        return false;
      }
    }

    if (status != STATUS_CTS)
    {
      ESP_LOGE(TAG, "Patch download: status 0x%02x at offset %u", status, static_cast<unsigned>(offset));
      return false;
    }
  }

  delayUs(250);
  return true;
}

bool Si473x::loadPatch(const uint8_t *content, uint16_t size, uint8_t audiobw)
{
  queryLibraryId();
  patchPowerUp();
  delayMs(50);
  const bool ok = downloadPatch(content, size);

  // AUDIOBW as requested, SBCUTFLT = 1, AVC_DIVIDER = 0 (SSB mode), AVCEN = 0,
  // SMUTESEL = 0, DSP_AFCDIS = 1 (SSB mode, AFC disabled)
  setSSBConfig(audiobw, 1, 0, 0, 0, 1);
  delayMs(25);
  return ok;
}

// ---------------------------------------------------------------------------
// RDS configuration
// ---------------------------------------------------------------------------

void Si473x::setRdsConfig(uint8_t RDSEN, uint8_t BLETHA, uint8_t BLETHB, uint8_t BLETHC, uint8_t BLETHD)
{
  // RDSEN bit 0; BLETHD 9:8, BLETHC 11:10, BLETHB 13:12, BLETHA 15:14
  const uint16_t value = static_cast<uint16_t>((RDSEN & 1) | ((BLETHD & 3) << 8) | ((BLETHC & 3) << 10) |
                                               ((BLETHB & 3) << 12) | ((BLETHA & 3) << 14));
  sendProperty(FM_RDS_CONFIG, value);
  RdsInit();
}
