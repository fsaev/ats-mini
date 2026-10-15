#ifndef BLE_BYTE_RING_H
#define BLE_BYTE_RING_H

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

//
// Fixed size byte FIFO which is safe to use between tasks and cores, e.g. the
// NimBLE host task filling it while the application task drains it.
//
// Every operation is a short critical section (a memcpy of at most N bytes),
// which is much cheaper than a task switch and cannot block.
//
template <size_t N>
class BleByteRing {
public:
  size_t available() const
  {
    portENTER_CRITICAL(&lock);
    size_t n = count;
    portEXIT_CRITICAL(&lock);
    return n;
  }

  size_t room() const { return N - available(); }
  bool empty() const { return available() == 0; }

  // Append as many bytes as fit, returns the number of bytes stored
  size_t write(const uint8_t* data, size_t length)
  {
    portENTER_CRITICAL(&lock);
    size_t n = N - count;
    if (length < n) n = length;
    for (size_t i = 0; i < n; i++)
      buf[(head + count + i) % N] = data[i];
    count += n;
    portEXIT_CRITICAL(&lock);
    return n;
  }

  // Copy out up to max bytes without removing them
  size_t peek(uint8_t* out, size_t max) const
  {
    portENTER_CRITICAL(&lock);
    size_t n = count < max ? count : max;
    for (size_t i = 0; i < n; i++)
      out[i] = buf[(head + i) % N];
    portEXIT_CRITICAL(&lock);
    return n;
  }

  // Remove up to n bytes from the front, returns the number removed
  size_t discard(size_t n)
  {
    portENTER_CRITICAL(&lock);
    if (n > count) n = count;
    head = (head + n) % N;
    count -= n;
    portEXIT_CRITICAL(&lock);
    return n;
  }

  // Next byte without removing it, or -1
  int peekByte() const
  {
    uint8_t c;
    return peek(&c, 1) ? c : -1;
  }

  // Remove and return the next byte, or -1
  int readByte()
  {
    portENTER_CRITICAL(&lock);
    int c = -1;
    if (count)
    {
      c = buf[head];
      head = (head + 1) % N;
      count--;
    }
    portEXIT_CRITICAL(&lock);
    return c;
  }

  void flush()
  {
    portENTER_CRITICAL(&lock);
    head = 0;
    count = 0;
    portEXIT_CRITICAL(&lock);
  }

private:
  mutable portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
  uint8_t buf[N];
  size_t head = 0;
  size_t count = 0;
};

#endif
