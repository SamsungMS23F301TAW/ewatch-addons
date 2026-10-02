#include "parallax_cmd.h"
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static QueueHandle_t sQ = nullptr;
static char sBuf[kCmdLen];
static int sLen = 0;

void parallaxSerialPoll() {
  if (!sQ) sQ = xQueueCreate(4, sizeof(ParallaxCmd));
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      if (sLen > 0) {
        ParallaxCmd cmd;
        memcpy(cmd.line, sBuf, (size_t)sLen);
        cmd.line[sLen] = 0;
        if (xQueueSend(sQ, &cmd, 0) != pdPASS) Serial.println("tp: busy, command dropped");
        else if (strcmp(cmd.line, "help") != 0) Serial.printf("tp> %s (runs on the face)\n", cmd.line);
      }
      sLen = 0;
      continue;
    }
    if (sLen < kCmdLen - 1) sBuf[sLen++] = (char)c;
  }
}

bool parallaxCmdPop(ParallaxCmd &out) {
  if (!sQ) return false;
  return xQueueReceive(sQ, &out, 0) == pdPASS;
}
