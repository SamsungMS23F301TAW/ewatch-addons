// Host stand-in for Arduino's Print class (text output funnels into write()).
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *buffer, size_t size) {
    size_t n = 0;
    while (size--) n += write(*buffer++);
    return n;
  }
  size_t write(const char *s) { return s ? write((const uint8_t *)s, strlen(s)) : 0; }
  size_t print(const char *s) { return write(s); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); return write(b); }
  size_t print(unsigned v) { char b[16]; snprintf(b, sizeof(b), "%u", v); return write(b); }
  size_t print(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); return write(b); }
  size_t print(unsigned long v) { char b[24]; snprintf(b, sizeof(b), "%lu", v); return write(b); }
  size_t println(const char *s = "") { size_t n = write(s); return n + write("\n"); }
  size_t printf(const char *fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    return write(b);
  }
};
