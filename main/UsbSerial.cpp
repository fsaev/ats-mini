#include "freertos/FreeRTOS.h"
#include "driver/usb_serial_jtag.h"
#include "UsbSerial.h"

// How long to wait for the host to take output, in milliseconds
#define TX_TIMEOUT_MS 100

UsbSerial usbSerial;

bool UsbSerial::begin()
{
  if(started) return true;

  usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  config.rx_buffer_size = 512;
  config.tx_buffer_size = 1024;
  started = usb_serial_jtag_driver_install(&config) == ESP_OK;
  return started;
}

bool UsbSerial::fill()
{
  if(pending >= 0) return true;
  if(!started) return false;

  uint8_t c;
  if(usb_serial_jtag_read_bytes(&c, 1, 0) != 1) return false;

  pending = c;
  return true;
}

size_t UsbSerial::write(const uint8_t *data, size_t length)
{
  if(!started) return 0;

  size_t written = 0;
  while(written < length)
  {
    int n = usb_serial_jtag_write_bytes(data + written, length - written, pdMS_TO_TICKS(TX_TIMEOUT_MS));
    // Nobody is reading the port, drop the rest
    if(n <= 0) break;
    written += n;
  }
  return written;
}

int UsbSerial::available()
{
  return fill() ? 1 : 0;
}

int UsbSerial::read()
{
  if(!fill()) return -1;

  int c = pending;
  pending = -1;
  return c;
}

int UsbSerial::peek()
{
  return fill() ? pending : -1;
}

void UsbSerial::flush()
{
  if(started) usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(TX_TIMEOUT_MS));
}
