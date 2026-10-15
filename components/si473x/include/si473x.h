/*
 * Native ESP-IDF driver for the Silicon Labs Si4732 / Si4735 (SI473x)
 * AM/FM/SW/LW/SSB radio receiver, programmed over I2C per AN332.
 *
 * Derived from the SI4735 Arduino library by Ricardo Lima Caratti (PU2CLR),
 * https://github.com/pu2clr/SI4735, MIT License:
 *
 *   MIT License
 *   Copyright (c) 2019 Ricardo Lima Caratti
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to permit
 *   persons to whom the Software is furnished to do so, subject to the
 *   following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 *   NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 *   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 *   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 *   USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * This is a re-implementation of the subset of that library used by the
 * ATS-Mini firmware (including the fixes that firmware previously applied in
 * its SI4735_fixed subclass), using the ESP-IDF I2C master driver instead of
 * Arduino Wire. It is not thread-safe: use it from a single task.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

class Si473x
{
public:
  Si473x() = default;
  ~Si473x();
  Si473x(const Si473x &) = delete;
  Si473x &operator=(const Si473x &) = delete;

  // ---- Bus / device bring-up -------------------------------------------

  // Creates the I2C master bus (I2C_NUM_0). Replaces Arduino Wire.begin(SDA, SCL).
  // Internal pull-ups are enabled (believed to match Arduino-ESP32's Wire; they
  // are weak, so external pull-ups on the board still matter at 800 kHz).
  bool begin(gpio_num_t sda, gpio_num_t scl);
  bool begin(int sda, int scl) { return begin(static_cast<gpio_num_t>(sda), static_cast<gpio_num_t>(scl)); }

  // Sets the I2C bus clock (Hz). May be called before or after the device
  // has been attached; takes effect on the next transfer.
  void setI2CFastModeCustom(long value = 500000);

  // Pulses the reset pin, then probes 0x11 (SEN low) and 0x63 (SEN high).
  // Returns the 7-bit address found (and attaches the device), or 0 if none.
  int16_t getDeviceI2CAddress(uint8_t resetPin);

  // Resets and powers up the device. defaultFunction: 0 = FM, 1 = AM/SW/MW/LW.
  void setup(uint8_t resetPin, uint8_t defaultFunction);
  void setRefClock(uint16_t refclk);
  void setRefClockPrescaler(uint16_t prescale);

  // Pin driven high to mute external audio hardware during power-down and
  // low again after power-up (the pin is configured as an output).
  void setAudioMuteMcuPin(int8_t pin);

  // ---- Mode / tuning ---------------------------------------------------

  void setFM(uint16_t fromFreq, uint16_t toFreq, uint16_t initialFreq, uint16_t step);
  void setAM(uint16_t fromFreq, uint16_t toFreq, uint16_t initialFreq, uint16_t step);
  // usblsb: 1 = LSB, 2 = USB. The SSB patch must have been loaded with loadPatch().
  void setSSB(uint16_t fromFreq, uint16_t toFreq, uint16_t initialFreq, uint16_t step, uint8_t usblsb);

  void setFrequency(uint16_t freq);
  void setFrequencyStep(uint16_t step) { step_ = step; }
  void setTuneFrequencyAntennaCapacitor(uint16_t capacitor);
  uint16_t getFrequency();                          // Queries the device (tune status)
  uint16_t getCurrentFrequency() const { return workFreq_; } // Last known, no I2C

  // Tune status (TUNE_STATUS command). Updates the values read by
  // getTuneCompleteTriggered() and getAntennaTuningCapacitor().
  void getStatus(uint8_t INTACK, uint8_t CANCEL);
  bool getTuneCompleteTriggered() const { return tuneStatus_[0] & 0x01; }
  uint16_t getAntennaTuningCapacitor() const;

  // Seek. SSB is not supported by the seek command (returns immediately).
  // showFunc is called with the current frequency after every poll;
  // stopSeeking is polled and aborts the wait when it returns true.
  void seekStationProgress(void (*showFunc)(uint16_t f), bool (*stopSeeking)(), uint8_t up_down);
  void setMaxSeekTime(long time_in_ms) { maxSeekTime_ = time_in_ms; }
  void setSeekAmLimits(uint16_t bottom, uint16_t top);
  void setSeekFmLimits(uint16_t bottom, uint16_t top);
  void setSeekAmSpacing(uint16_t spacing);
  void setSeekFmSpacing(uint16_t spacing);
  void setSeekAmRssiThreshold(uint16_t value);
  void setSeekFmRssiThreshold(uint16_t value);
  void setSeekAmSNRThreshold(uint16_t value);
  void setSeekFmSNRThreshold(uint16_t value);

  // Delay (ms) applied after the set-frequency command and between seek polls.
  void setMaxDelaySetFrequency(uint16_t ms) { maxDelaySetFrequency_ = ms; }

  // ---- Signal quality / AGC --------------------------------------------

  void getCurrentReceivedSignalQuality();
  uint8_t getCurrentRSSI() const { return rsq_[4]; }
  uint8_t getCurrentSNR() const { return rsq_[5]; }
  bool getCurrentPilot() const { return rsq_[3] & 0x80; }

  void getAutomaticGainControl();   // Queries AGC status (stored; no accessors, as the app does not use the result)
  void setAutomaticGainControl(uint8_t AGCDIS, uint8_t AGCIDX);
  void setAvcAmMaxGain(uint8_t gain);

  // ---- Audio / filters -------------------------------------------------

  void setVolume(uint8_t volume);
  void setAudioMute(bool off); // true mutes the audio, false unmutes
  void setBandwidth(uint8_t AMCHFLT, uint8_t AMPLFLT);
  void setFmBandwidth(uint8_t filter_value = 0);
  void setFMDeEmphasis(uint8_t parameter);
  void setAmSoftMuteMaxAttenuation(uint8_t smattn = 0);
  void setSsbSoftMuteMaxAttenuation(uint8_t smattn = 0);
  void setGpioCtl(uint8_t GPO1OEN, uint8_t GPO2OEN, uint8_t GPO3OEN);
  void setGpio(uint8_t GPO1LEVEL, uint8_t GPO2LEVEL, uint8_t GPO3LEVEL);

  // ---- SSB -------------------------------------------------------------

  // Loads the SSB patch (sequence of 8-byte commands) and configures SSB.
  // audiobw: 0 = 1.2kHz, 1 = 2.2kHz, 2 = 3kHz, 3 = 4kHz, 4 = 0.5kHz, 5 = 1kHz.
  // Returns false if the device rejected the patch.
  bool loadPatch(const uint8_t *content, uint16_t size, uint8_t audiobw);
  void setSSBBfo(int offset);
  void setSSBAudioBandwidth(uint8_t AUDIOBW);
  void setSSBAutomaticVolumeControl(uint8_t AVCEN);
  void setSSBSidebandCutoffFilter(uint8_t SBCUTFLT);

  // ---- RDS (FM only) ---------------------------------------------------

  void RdsInit();                                   // Clears the RDS text buffers
  void setRdsConfig(uint8_t RDSEN, uint8_t BLETHA, uint8_t BLETHB, uint8_t BLETHC, uint8_t BLETHD);
  void getRdsStatus() { getRdsStatus(0, 0, 0); }    // Reads one group from the RDS FIFO
  bool getRdsReceived() const { return rds_[1] & 0x01; }
  bool getRdsSyncFound() const { return rds_[1] & 0x04; }
  bool getRdsSync() const { return rds_[2] & 0x01; }
  uint16_t getRdsPI();
  uint8_t getRdsProgramTypeX() const;
  uint8_t getRdsVersionCode() const;                // 0 = version A, 1 = version B
  char *getRdsStationName();                        // PS name (group 0), 8 chars, or NULL
  char *getRdsText2A();                             // Radio text (group 2A), or NULL
  char *getRdsText2B();                             // Radio text (group 2B), or NULL
  char *getRdsTime();                               // "HH:MM +HH:MM" (group 4A), or NULL

private:
  // I/O helpers
  bool writeBytes(const uint8_t *data, size_t len);
  bool readBytes(uint8_t *data, size_t len);
  bool waitToSend();
  bool readResponse(uint8_t *data, size_t len, bool retryOnError);
  void sendProperty(uint16_t propertyNumber, uint16_t parameter);
  bool attachDevice(uint8_t address);
  void detachDevice();

  // Device control
  void reset();
  void setPowerUp(uint8_t CTSIEN, uint8_t GPO2OEN, uint8_t PATCH, uint8_t XOSCEN, uint8_t FUNC, uint8_t OPMODE);
  void radioPowerUp();
  void powerDown();
  void getFirmware();
  void setHardwareAudioMute(bool on);
  void disableFmDebug();
  void setAM();
  void setFM();
  void setSSB(uint8_t usblsb);
  void seekStation(uint8_t SEEKUP, uint8_t WRAP);
  void queryLibraryId();
  void patchPowerUp();
  bool downloadPatch(const uint8_t *content, uint16_t size);
  void setSSBConfig(uint8_t AUDIOBW, uint8_t SBCUTFLT, uint8_t AVC_DIVIDER, uint8_t AVCEN,
                    uint8_t SMUTESEL, uint8_t DSP_AFCDIS);
  void setSsbModeBits(uint8_t shift, uint16_t mask, uint8_t value);
  void getRdsStatus(uint8_t INTACK, uint8_t MTFIFO, uint8_t STATUSONLY);
  uint16_t rdsBlockB() const { return static_cast<uint16_t>((rds_[6] << 8) | rds_[7]); }
  uint8_t rdsGroupType() const { return static_cast<uint8_t>(rdsBlockB() >> 12); }

  // Full power-up/setup implementation behind setup(resetPin, defaultFunction)
  void setup(uint8_t resetPin, uint8_t ctsIntEnable, uint8_t defaultFunction,
             uint8_t audioMode, uint8_t clockType, uint8_t gpo2Enable);

  // Bus
  i2c_master_bus_handle_t bus_ = nullptr;
  i2c_master_dev_handle_t dev_ = nullptr;
  uint32_t i2cSpeedHz_ = 100000;
  uint8_t address_ = 0;

  // Pins
  int resetPin_ = -1;
  int audioMuteMcuPin_ = -1;

  // Configuration / state
  uint16_t maxDelaySetFrequency_ = 30; // ms
  uint16_t maxDelayAfterPowerUp_ = 10; // ms
  long maxSeekTime_ = 8000;            // ms
  uint8_t lastMode_ = 0xFF;            // 0 = FM, 1 = AM, 2 = SSB
  uint8_t currentTune_ = 0x20;         // FM_TUNE_FREQ or AM_TUNE_FREQ command
  uint16_t workFreq_ = 0;
  uint16_t step_ = 0;
  uint8_t currentAvcAmMaxGain_ = 36;
  uint8_t clockType_ = 1;              // POWER_UP XOSCEN: 1 = crystal, 0 = external RCLK
  uint8_t ctsIntEnable_ = 0;
  uint8_t gpo2Enable_ = 0;
  uint8_t audioMode_ = 0x05;           // POWER_UP OPMODE: analog audio out
  uint16_t refClock_ = 32768;
  uint16_t refClockPrescale_ = 1;
  uint8_t volume_ = 32;
  uint8_t sideband_ = 0;               // 0 = not SSB, 1 = LSB, 2 = USB
  uint16_t ssbModeProp_ = 0x8011;      // SSB_MODE property value (see setSSBConfig)

  // Tune command parameters
  uint8_t tuneFreeze_ = 0;
  uint8_t antCapH_ = 0;
  uint8_t antCapL_ = 1;

  // Power-up command parameters
  uint8_t powerUp_[2] = {0, 0};

  // Last responses
  uint8_t tuneStatus_[8] = {};
  uint8_t rsq_[8] = {};
  uint8_t agcStatus_[3] = {};
  uint8_t rds_[13] = {};

  // RDS decode state
  char rdsBuffer2A_[65] = {};
  char rdsBuffer2B_[33] = {};
  char rdsBuffer0A_[9] = {};
  char rdsTime_[25] = {};
  uint8_t lastTextFlagAB_ = 0;
  uint16_t rdsLastFreq_ = 0;
};
