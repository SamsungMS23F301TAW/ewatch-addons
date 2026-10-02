// Minimal Arduino core shim so Arduino_GFX's drawing code (and the face
// renderer) compile on the host for previews and pixel tests. Only what the
// GFX core, Arduino_Canvas and src/apps/face/* actually use.
#pragma once
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Print.h"

#include <string>

#define PROGMEM
#define PSTR(s) (s)
#define F(s) (s)
typedef bool boolean;
typedef uint8_t byte;

// Just enough of Arduino's String / F() types for Arduino_GFX's overloads.
class __FlashStringHelper;
class String {
public:
  String(const char *s = "") : s_(s ? s : "") {}
  unsigned int length() const { return (unsigned int)s_.size(); }
  const char *c_str() const { return s_.c_str(); }
private:
  std::string s_;
};

// Host time is whatever the preview harness says it is (see host_time.cpp).
extern uint32_t g_hostMillis;
static inline uint32_t millis() { return g_hostMillis; }
static inline uint32_t micros() { return g_hostMillis * 1000u; }
static inline void delay(uint32_t) {}
static inline void yield() {}
