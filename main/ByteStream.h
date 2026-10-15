#ifndef BYTE_STREAM_H
#define BYTE_STREAM_H

#include <stddef.h>
#include <stdint.h>

//
// Minimal byte-oriented transport used by the remote control protocol
// (see Remote.cpp). Implemented by the USB serial port and the BLE UART.
//
class ByteStream
{
public:
  virtual ~ByteStream() = default;

  // Write raw bytes, returns the number of bytes accepted
  virtual size_t write(const uint8_t *data, size_t length) = 0;

  // Number of bytes ready to be read
  virtual int available() = 0;

  // Read one byte, or return -1 if there is none
  virtual int read() = 0;

  // Look at the next byte without consuming it, or return -1 if there is none
  virtual int peek() = 0;

  // Push out any buffered output
  virtual void flush() {}

  size_t write(uint8_t c) { return write(&c, 1); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(const char *s);
  size_t println() { return print("\r\n"); }
  size_t println(const char *s) { return print(s) + println(); }
  int printf(const char *format, ...) __attribute__((format(printf, 2, 3)));
};

#endif // BYTE_STREAM_H
