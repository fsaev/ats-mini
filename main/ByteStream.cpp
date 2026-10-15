#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ByteStream.h"

size_t ByteStream::print(const char *s)
{
  return write((const uint8_t *)s, strlen(s));
}

int ByteStream::printf(const char *format, ...)
{
  char buf[128];
  va_list args;

  va_start(args, format);
  int n = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);

  if(n < 0) return n;
  if((size_t)n < sizeof(buf)) return (int)write((const uint8_t *)buf, n);

  // Too long for the stack buffer, format again into the heap
  char *big = new char[n + 1];
  va_start(args, format);
  vsnprintf(big, n + 1, format, args);
  va_end(args);
  n = (int)write((const uint8_t *)big, n);
  delete[] big;
  return n;
}
