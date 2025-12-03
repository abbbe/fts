/**
 * FTS (FineTimeSync) Example Application
 *
 * Demonstrates synchronized measurements using FTS framework.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "rom/ets_sys.h"
#include "dtr.h"
#include "ftm.h"
#include "build_info.h"

#ifdef CONFIG_FTS_ROLE_SLAVE
#include "crm.h"
#include "dtc.h"
#endif

static const char *TAG = "fts_main";

// --- LED for pulse output

// Toggle every 1s (@2.5kHz)
#define TOGGLE_LED_GPIO_DTR_CYCLES 2500

#ifdef CONFIG_FTS_ROLE_SLAVE
    // Waveshare ESP32-S3-LCD-1.47
    #define LED_GPIO GPIO_NUM_41
#elif defined(CONFIG_FTS_ROLE_MASTER)
    // Seeed Studio XIAO ESP32S3 Yellow User Led
    #define LED_GPIO GPIO_NUM_21
#else
    #error "CONFIG_FTS_ROLE_MASTER or CONFIG_FTS_ROLE_SLAVE must be defined"
#endif

// GPIO pulse output (2.5kHz 20% duty cycle)
#define TOGGLE_GPIO GPIO_NUM_7

/**
 * FTS callback - invoked in ISR context on each timer cycle
 * Runs at 2.5kHz (every 400µs)
 */
static void IRAM_ATTR fts_callback(uint32_t master_cycle)
{
#ifdef LED_GPIO
    // Blink at 1Hz, 20% on (active low), 80% off
    int led_phase = master_cycle % TOGGLE_LED_GPIO_DTR_CYCLES;
    int led_state = (led_phase < TOGGLE_LED_GPIO_DTR_CYCLES / 5) ? 0 : 1;
    gpio_set_level(LED_GPIO, led_state);
#else
    // No LED hardware - log pulse to console. CAUTION: ISR context!
    // ets_printf("P%lu\n", master_cycle);
#endif
}

void app_main(void)
{
    ESP_LOGI(TAG, "FTS built %s - %s - %s",
             BUILD_TIMESTAMP,
             BUILD_GIT_DIRTY ? "DIRTY" : "CLEAN",
             BUILD_GIT_HASH);

#ifdef LED_GPIO
    // Initialize LED
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);
    gpio_set_level(LED_GPIO, 1);  // LED off (active low)
#endif // LED_GPIO

#ifdef CONFIG_FTS_ROLE_SLAVE
    // ========== SLAVE MODE ==========
    // Initialize Wifi STA with FTM initiator (starts WiFi and MAC clock)
    ESP_ERROR_CHECK(ftm_slave_init(CONFIG_FTS_AP_SSID, CONFIG_FTS_AP_PASSWORD));

    // Initialize DTR (MCPWM timer hardware)
    ESP_ERROR_CHECK(dtr_init(DTR_MODE_SLAVE, fts_callback, TOGGLE_GPIO));

    // Start timer and measure MAC/timer relationship
    dtr_start_timer();

    // Initialize CRM (ready to receive FTM reports)
    ESP_ERROR_CHECK(crm_init());

    // Initialize DTC (registers with CRM)
    ESP_ERROR_CHECK(dtc_init());
#elif defined(CONFIG_FTS_ROLE_MASTER)
    // ========== MASTER MODE ==========
    // Initialize WiFi AP with FTM responder (starts WiFi, MAC clock, and sync broadcast)
    ESP_ERROR_CHECK(ftm_master_init(CONFIG_FTS_AP_SSID, CONFIG_FTS_AP_PASSWORD, CONFIG_FTS_AP_CHANNEL));

    // Initialize DTR (MCPWM timer hardware)
    ESP_ERROR_CHECK(dtr_init(DTR_MODE_MASTER, fts_callback, TOGGLE_GPIO));

    // Start timer and measure MAC/timer relationship
    dtr_start_timer();

    // Align timer to MAC clock epoch boundaries
    dtr_align_master_timer();
#else
    #error "CONFIG_FTS_ROLE_MASTER or CONFIG_FTS_ROLE_SLAVE must be defined"
#endif
    ESP_LOGI(TAG, "FTS started");
}