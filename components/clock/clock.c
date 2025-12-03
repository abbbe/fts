/**
 * Clock Utilities Implementation
 */

#include "clock.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "clock";

// Watchdog task configuration
#define CLOCK_WATCHDOG_STACK_SIZE   2048
#define CLOCK_WATCHDOG_INTERVAL_MS  (60 * 60 * 1000)  // 1 hour
#define CLOCK_WATCHDOG_PRIORITY     5 // low priority

// ============================================================================
// Counter Unwrapping
// ============================================================================

int64_t clock_unwrap(int64_t val, unwrap_state_t *state)
{
    // Detect wrap: current < last
    if (state->last_val != 0 && val < state->last_val) {
        state->wrap_count++;

        // Dual-wrap detection (for t1/t4): check if last_val < wrap_value2
        if (state->wrap_value2 > 0 && state->last_val < (int64_t)state->wrap_value2) {
            state->offset += (int64_t)state->wrap_value2;
        } else {
            state->offset += (int64_t)state->wrap_value;
        }
    }

    state->last_val = val;
    return val + state->offset;
}

// ============================================================================
// MAC Clock
// ============================================================================

// MAC clock wrap detection (32-bit microsecond counter)
#define MAC_CLOCK_WRAP_US (1ULL << 32)  // 4,294,967,296 us (~71.6 minutes)

// State
static bool s_initialized = false;
static unwrap_state_t s_unwrap_state = {0, 0, 0, MAC_CLOCK_WRAP_US, 0};
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_watchdog_task_handle = NULL;

/**
 * Watchdog task to ensure MAC clock wrapping is detected
 *
 * Runs every hour to call clock_get_us() and ensure wrap detection
 * even if no other code calls it. This prevents missing the wrap event.
 */
static void watchdog_task(void *arg)
{
    ESP_LOGI(TAG, "MAC clock watchdog task started (runs every hour)");

    while (1) {
        // Call the clock to ensure wrap detection
        int64_t clock_us = clock_get_us();
        ESP_LOGD(TAG, "Watchdog: MAC clock = %lld us (wrap count = %lu)",
                 (long long)clock_us, (unsigned long)s_unwrap_state.wrap_count);

        // Sleep for 1 hour
        vTaskDelay(pdMS_TO_TICKS(CLOCK_WATCHDOG_INTERVAL_MS));
    }
}

esp_err_t clock_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Clock already initialized");
        return ESP_OK;
    }

    // Make sure MAC clock is running
    uint32_t mac_clock1_us= esp_wifi_internal_get_mac_clock_time();
    vTaskDelay(pdMS_TO_TICKS(1)); // Wait 1 ms, plenty of time for 1us clock to advance
    uint32_t mac_clock2_us = esp_wifi_internal_get_mac_clock_time();

    if (mac_clock2_us == mac_clock1_us) {
        ESP_LOGE(TAG, "MAC clock is not advancing, stays at %lu us", (unsigned long)mac_clock1_us);
        return ESP_FAIL;
    }
    
    // Create mutex for thread-safe access
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "Failed to create clock mutex");
        return ESP_FAIL;
    }

    // Initialize the unwrap state tracker
    s_unwrap_state.last_val = mac_clock2_us; 

    // Mark as initialized
    s_initialized = true;

    // Start watchdog task to ensure wrap detection every hour
    BaseType_t ret = xTaskCreate(
        watchdog_task,
        "clock_wdog",
        CLOCK_WATCHDOG_STACK_SIZE,
        NULL,
        CLOCK_WATCHDOG_PRIORITY,
        &s_watchdog_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create clock watchdog task");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        s_initialized = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

int64_t clock_get_us(void)
{
    // FATAL if not initialized
    if (!s_initialized) {
        ESP_LOGE(TAG, "FATAL: clock_get_us() called before clock_init()");
        abort();
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    uint32_t mac_clock_us = esp_wifi_internal_get_mac_clock_time();
    int64_t unwrapped = clock_unwrap((int64_t)mac_clock_us, &s_unwrap_state);

    xSemaphoreGive(s_lock);

    return unwrapped;
}

int64_t clock_get_base_us(void)
{
    // FATAL if not initialized
    if (!s_initialized) {
        ESP_LOGE(TAG, "FATAL: clock_get_base() called before clock_init()");
        abort();
    }

    // Thread-safe read of wrap offset
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t base = s_unwrap_state.offset;
    xSemaphoreGive(s_lock);

    return base;
}
