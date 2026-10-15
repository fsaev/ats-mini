/*
 * RDS decoding for the Si473x driver.
 *
 * Derived from the SI4735 Arduino library, Copyright (c) 2019 Ricardo Lima
 * Caratti (PU2CLR), MIT License (https://github.com/pu2clr/SI4735). See the
 * full license text in include/si473x.h.
 *
 * FM_RDS_STATUS response layout (13 bytes, AN332):
 *   [0]  status (CTS, ERR, ...)
 *   [1]  RDSRECV bit 0, RDSSYNCLOST bit 1, RDSSYNCFOUND bit 2,
 *        RDSNEWBLOCKA bit 4, RDSNEWBLOCKB bit 5
 *   [2]  RDSSYNC bit 0, GRPLOST bit 2
 *   [3]  RDS FIFO used
 *   [4..5] block A, [6..7] block B, [8..9] block C, [10..11] block D (high byte first)
 *   [12] block error counts
 */
#include "si473x.h"

#include <string.h>

#include "esp_rom_sys.h"

namespace
{
constexpr uint8_t FM_TUNE_FREQ = 0x20;
constexpr uint8_t FM_RDS_STATUS = 0x24;
constexpr uint8_t RDS_NEW_BLOCK_A = 0x10; // Bit 4 of response byte 1

inline void twoDigits(char *dst, unsigned value)
{
  dst[0] = static_cast<char>('0' + (value / 10) % 10);
  dst[1] = static_cast<char>('0' + value % 10);
}
} // namespace

void Si473x::RdsInit()
{
  memset(rdsBuffer2A_, 0, sizeof(rdsBuffer2A_));
  memset(rdsBuffer2B_, 0, sizeof(rdsBuffer2B_));
  memset(rdsBuffer0A_, 0, sizeof(rdsBuffer0A_));
  lastTextFlagAB_ = 0;
}

void Si473x::getRdsStatus(uint8_t INTACK, uint8_t MTFIFO, uint8_t STATUSONLY)
{
  // RDS is FM only
  if (currentTune_ != FM_TUNE_FREQ) return;

  // Clear the text buffers when the frequency has changed
  if (rdsLastFreq_ != workFreq_)
  {
    rdsLastFreq_ = workFreq_;
    RdsInit();
  }

  const uint8_t cmd[] = {
    FM_RDS_STATUS,
    static_cast<uint8_t>((INTACK & 1) | ((MTFIFO & 1) << 1) | ((STATUSONLY & 1) << 2))
  };
  waitToSend();
  writeBytes(cmd, sizeof(cmd));
  readResponse(rds_, sizeof(rds_), true);
  esp_rom_delay_us(550);
}

// Block B holds the group type (15:12), version (11), TP (10) and PTY (9:5)

uint8_t Si473x::getRdsVersionCode() const
{
  return (rdsBlockB() >> 11) & 1;
}

// Returns the full 5-bit PTY (the PU2CLR library only returned 3 bits)
uint8_t Si473x::getRdsProgramTypeX() const
{
  return (rdsBlockB() >> 5) & 0x1F;
}

// Returns the full PI code (the PU2CLR library only returned the low byte)
uint16_t Si473x::getRdsPI()
{
  if (getRdsReceived() && (rds_[1] & RDS_NEW_BLOCK_A))
    return static_cast<uint16_t>((rds_[4] << 8) | rds_[5]);
  return 0x0000;
}

// Programme Service name: group 0 (A or B), segment address in bits 1:0 of
// block B, two characters per segment in block D.
char *Si473x::getRdsStationName()
{
  if (!getRdsReceived() || rdsGroupType() != 0) return nullptr;

  // Bit 4 of block B (text A/B flag in group 2, TA in group 0): reset the
  // buffer when it toggles, as the PU2CLR library does
  const uint8_t flagAB = (rdsBlockB() >> 4) & 1;
  if (lastTextFlagAB_ != flagAB)
  {
    lastTextFlagAB_ = flagAB;
    memset(rdsBuffer0A_, 0, sizeof(rdsBuffer0A_));
  }

  const uint8_t address = rdsBlockB() & 0x03;
  rdsBuffer0A_[address * 2]     = static_cast<char>(rds_[10]);
  rdsBuffer0A_[address * 2 + 1] = static_cast<char>(rds_[11]);
  rdsBuffer0A_[8] = '\0';
  return rdsBuffer0A_;
}

// Radio text, version A: group 2A, segment address in bits 3:0 of block B,
// four characters per segment in blocks C and D. NULL if the group is version B.
char *Si473x::getRdsText2A()
{
  if (getRdsVersionCode()) return nullptr;
  if (!getRdsReceived() || rdsGroupType() != 2) return nullptr;

  const uint8_t address = rdsBlockB() & 0x0F;
  char *dst = &rdsBuffer2A_[address * 4];
  dst[0] = static_cast<char>(rds_[8]);
  dst[1] = static_cast<char>(rds_[9]);
  dst[2] = static_cast<char>(rds_[10]);
  dst[3] = static_cast<char>(rds_[11]);
  rdsBuffer2A_[63] = '\0';
  return rdsBuffer2A_;
}

// Radio text, version B: group 2B, two characters per segment in block D.
// NULL unless the group is version B.
char *Si473x::getRdsText2B()
{
  if (!getRdsVersionCode()) return nullptr;
  if (rdsGroupType() != 2) return nullptr;

  const uint8_t address = rdsBlockB() & 0x0F;
  rdsBuffer2B_[address * 2]     = static_cast<char>(rds_[10]);
  rdsBuffer2B_[address * 2 + 1] = static_cast<char>(rds_[11]);
  rdsBuffer2B_[32] = '\0';
  return rdsBuffer2B_;
}

// Clock time and date: group 4A. UTC hour/minute and local offset in
// half-hours (sign bit 1 = negative). Formats "HH:MM +HH:MM".
char *Si473x::getRdsTime()
{
  if (rdsGroupType() != 4) return nullptr;

  const uint16_t blockC = static_cast<uint16_t>((rds_[8] << 8) | rds_[9]);
  const uint16_t blockD = static_cast<uint16_t>((rds_[10] << 8) | rds_[11]);

  const unsigned offset = blockD & 0x1F;
  const bool negative = (blockD >> 5) & 1;
  const unsigned minute = (blockD >> 6) & 0x3F;
  const unsigned hour = ((blockD >> 12) & 0x0F) | ((blockC & 1) << 4);
  const unsigned offsetH = (offset * 30) / 60;
  const unsigned offsetM = offset * 30 - offsetH * 60;

  if (offsetH > 12 || hour > 23 || minute > 59) return nullptr;

  twoDigits(&rdsTime_[0], hour);
  rdsTime_[2] = ':';
  twoDigits(&rdsTime_[3], minute);
  rdsTime_[5] = ' ';
  rdsTime_[6] = negative ? '-' : '+';
  twoDigits(&rdsTime_[7], offsetH);
  rdsTime_[9] = ':';
  twoDigits(&rdsTime_[10], offsetM);
  rdsTime_[12] = '\0';
  return rdsTime_;
}
