#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

//
// Time helpers
//

// Milliseconds since boot (wraps after ~49 days, use unsigned differences)
static inline uint32_t nowMs()
{
  return (uint32_t)(esp_timer_get_time() / 1000);
}

// Microseconds since boot (wraps after ~71 minutes, use unsigned differences)
static inline uint32_t nowUs()
{
  return (uint32_t)esp_timer_get_time();
}

// Block the calling task for the given time
// (CONFIG_FREERTOS_HZ is 1000, so one tick is one millisecond)
static inline void delayMs(uint32_t ms)
{
  vTaskDelay(pdMS_TO_TICKS(ms));
}

#endif // PLATFORM_H
