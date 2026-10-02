// Single-threaded host stand-ins for the FreeRTOS calls BaseOS headers use.
#pragma once
#include <stdint.h>
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFFu
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
static inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (SemaphoreHandle_t)1; }
static inline QueueHandle_t xQueueCreate(int, int) { return nullptr; }
static inline BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t) { return pdFALSE; }
static inline BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t) { return pdFALSE; }
static inline void vTaskDelay(TickType_t) {}
