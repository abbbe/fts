/**
 * CRM - Clock Relationship Model
 *
 * Performs regression analysis on FTM timestamps to determine
 * clock skew and offset between local and remote devices.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Global CRM state (read-only from other modules)
 * Updated by CRM after each successful regression
 */
extern bool crm_valid;                          // True when R² > 0.999
extern double crm_slope_lr_m1;                  // (local/remote - 1) slope (e.g., 0.000002 = 2ppm)
extern double crm_slope_rl_m1;                  // (remote/local - 1) slope (inverse, e.g., -0.000002)
extern int64_t crm_local_ref_ps;                // Local reference timestamp (picoseconds)
extern int64_t crm_remote_ref_ps;               // Remote reference timestamp (picoseconds)

/**
 * Callback invoked when CRM model is updated
 * Called from FTM task context (not ISR)
 */
typedef void (*crm_update_callback_t)(void);

/**
 * Initialize CRM module
 *
 * @return ESP_OK on success
 */
esp_err_t crm_init(void);

/**
 * Register callback for CRM updates
 *
 * @param callback Function to call when regression model is updated
 */
void crm_register_callback(crm_update_callback_t callback);

/**
 * Process FTM report and update regression model
 * Called by FTM module with unwrapped timestamps
 *
 * @param session_number FTM session number
 * @param t1_ps Array of master TX timestamps (picoseconds)
 * @param t2_ps Array of slave RX timestamps (picoseconds)
 * @param t3_ps Array of slave TX timestamps (picoseconds)
 * @param t4_ps Array of master RX timestamps (picoseconds)
 * @param count Number of entries in arrays
 */
void crm_process_ftm_report(uint32_t session_number,
                            const int64_t *t1_ps,
                            const int64_t *t2_ps,
                            const int64_t *t3_ps,
                            const int64_t *t4_ps,
                            uint8_t count);

#ifdef __cplusplus
}
#endif
