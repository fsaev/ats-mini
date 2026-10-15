#ifndef USB_SERIAL_H
#define USB_SERIAL_H

#include "ByteStream.h"

//
// The USB Serial/JTAG port of the ESP32-S3, used for the remote control protocol
//
class UsbSerial : public ByteStream
{
public:
  // Install the USB Serial/JTAG driver
  bool begin();

  size_t write(const uint8_t *data, size_t length) override;
  int available() override;
  int read() override;
  int peek() override;
  void flush() override;

private:
  // Fetch one byte into 'pending' if there is one
  bool fill();

  bool started = false;
  int pending = -1;
};

extern UsbSerial usbSerial;

#endif // USB_SERIAL_H
