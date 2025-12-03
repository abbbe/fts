/**
 * FTM - Fine Timing Measurement Module
 *
 * Manages WiFi connection and FTM sessions with master device.
 * Unwraps MAC clock timestamps and provides them to CRM.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

// FTM session configuration
#define FTM_FRAMES_PER_SESSION 64    // Frames per FTM session (8/16/24/32/64)
#define FTM_PERIOD_MS 1000           // FTM measurement period in milliseconds

#ifdef __cplusplus
extern "C" {
#endif

/**
 * FTM timestamp entry (unwrapped to 64-bit picoseconds)
 */
typedef struct {
    uint64_t t1_ps;  // Master TX timestamp
    uint64_t t2_ps;  // Slave RX timestamp
    uint64_t t3_ps;  // Slave TX timestamp
    uint64_t t4_ps;  // Master RX timestamp
} ftm_entry_t;

/**
 * FTM session report
 */
typedef struct {
    uint32_t session_number;
    uint8_t entry_count;
    const ftm_entry_t *entries;
} ftm_report_t;

/**
 * Callback invoked when FTM report is ready
 * Called from FTM task context (not ISR)
 */
typedef void (*ftm_callback_t)(const ftm_report_t *report);

/**
 * Initialize FTM master (WiFi AP with FTM responder)
 *
 * @param ssid AP SSID
 * @param password AP password
 * @param channel AP channel
 * @return ESP_OK on success
 */
esp_err_t ftm_master_init(const char *ssid, const char *password, uint8_t channel);

/**
 * Initialize FTM slave (connect to master AP and run FTM sessions)
 *
 * @param master_ssid AP SSID to connect to
 * @param master_password AP password
 * @return ESP_OK on success
 */
esp_err_t ftm_slave_init(const char *master_ssid, const char *master_password);

/**
 * Register callback for FTM reports
 *
 * @param callback Function to call when FTM report is ready
 */
void ftm_register_callback(ftm_callback_t callback);

/**
 * Deinitialize FTM module
 */
esp_err_t ftm_deinit(void);

#ifdef __cplusplus
}
#endif
