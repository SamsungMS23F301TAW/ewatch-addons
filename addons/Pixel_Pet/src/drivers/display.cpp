#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "pins.h"
#include "display.h"

static Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;

static const uint32_t BL_FREQ_HZ  = 12000;
static const uint8_t  BL_RES_BITS = 8;
static const uint8_t  BL_LEDC_CH  = 0;     // LEDC channel 0
static bool bl_attached = false;

bool displayBegin() {
  // Keep backlight OFF throughout panel init. The ST7789 power-up sequence
  // takes ~200 ms and shows random panel RAM during that window — that's the
  // distorted black/white pattern users saw on every cold boot and wake. The
  // caller must call backlightOn() (or backlightSet) only after the first
  // frame has been drawn, so the user only ever sees committed content.
  pinMode(PIN_DISP_BL, OUTPUT);
  digitalWrite(PIN_DISP_BL, LOW);

  bus = new Arduino_ESP32SPI(PIN_DISP_DC, PIN_DISP_CS,
                             PIN_DISP_SCK, PIN_DISP_MOSI,
                             PIN_DISP_MISO, FSPI);

  // 240x280 IPS, col offset 0, row offset 20 (matches the FPC panel).
  gfx = new Arduino_ST7789(bus, PIN_DISP_RST, /*rotation=*/0, /*IPS=*/true,
                           240, 280, 0, 20);

  if (!gfx->begin(60000000)) {
    Serial.println("Display: gfx->begin() FAILED");
    delete gfx; gfx = nullptr;
    delete bus; bus = nullptr;
    return false;  
}
  gfx->fillScreen(BLACK);
  Serial.println("Display: ST7789 ready");
  return true;
}

static uint8_t bl_current = 0;

void backlightSet(uint8_t duty) {
  if (!bl_attached) {
    ledcSetup(BL_LEDC_CH, BL_FREQ_HZ, BL_RES_BITS);
    ledcAttachPin(PIN_DISP_BL, BL_LEDC_CH);
    bl_attached = true;
  }
  ledcWrite(BL_LEDC_CH, duty);
  bl_current = duty;
}

void backlightOn()  { backlightSet(255); }
void backlightOff() { backlightSet(0); }
uint8_t backlightGet() { return bl_current; }

void backlightFadeTo(uint8_t target, uint16_t ms) {
  const uint8_t from = bl_current;
  if (from == target || ms < 16) { backlightSet(target); return; }
  const int steps = 10;
  for (int i = 1; i <= steps; i++) {
    backlightSet((uint8_t)((int)from + ((int)target - (int)from) * i / steps));
    vTaskDelay(pdMS_TO_TICKS(ms / steps));
  }
  backlightSet(target);
}

// ---- panel power ------------------------------------------------------------
// DISPOFF/DISPON carry no mandatory delay. SLPIN/SLPOUT do: 5 ms before the
// next command, and 120 ms between an SLPIN and the following SLPOUT.
static bool     s_panelSlept = false;
static uint32_t s_slpinMs = 0;

void panelSleep(bool deep) {
  if (!bus) return;
  bus->sendCommand(0x28);                 // DISPOFF
  if (deep && !s_panelSlept) {
    bus->sendCommand(0x10);               // SLPIN
    s_panelSlept = true;
    s_slpinMs = millis();
    vTaskDelay(pdMS_TO_TICKS(6));
  }
}

void panelWake() {
  if (!bus) return;
  if (s_panelSlept) {
    uint32_t since = millis() - s_slpinMs;
    if (since < 125) vTaskDelay(pdMS_TO_TICKS(125 - since));
    bus->sendCommand(0x11);               // SLPOUT
    s_panelSlept = false;
    vTaskDelay(pdMS_TO_TICKS(10));        // charge pumps settle while we paint
  }
}

void panelDispOn() {
  if (bus) bus->sendCommand(0x29);        // DISPON
}

bool panelAsleep() { return s_panelSlept; }

static Arduino_Canvas *gFrameCanvas = nullptr;

void displayFlushRows(int16_t y0, int16_t h) {
  if (!gfx || !gFrameCanvas) return;
  if (y0 < 0) { h += y0; y0 = 0; }
  if (y0 + h > 280) h = 280 - y0;
  if (h <= 0) return;
  uint16_t *fb = gFrameCanvas->getFramebuffer();
  if (!fb) return;
  gfx->draw16bitRGBBitmap(0, y0, fb + (int32_t)y0 * 240, 240, h);
}
Arduino_Canvas *frameCanvas() {
  if (gFrameCanvas) return gFrameCanvas;
  if (!gfx) return nullptr;
  gFrameCanvas = new Arduino_Canvas(240, 280, gfx, 0, 0);
  // GFX_SKIP_OUTPUT_BEGIN: the underlying ST7789 + SPI bus were already
  // started in displayBegin(). Letting Canvas re-run output->begin() registers
  // a second APB-change callback for the SPI bus and logs a "duplicate" warning
  // from esp32-hal-cpu.c the moment any view first allocates the canvas.
  if (!gFrameCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("frameCanvas: alloc failed");
    delete gFrameCanvas;
    gFrameCanvas = nullptr;
  }
  return gFrameCanvas;
}