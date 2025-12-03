/**
 * DTC - Disciplined Timer Controller
 *
 * Preprocesses CRM data for use by DTR realtime ISR.
 * Converts picosecond timestamps to timer ticks and calculates base period.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize DTC module (slave mode only)
 * Registers callback with CRM for alignment updates
 *
 * @return ESP_OK on success
 */
esp_err_t dtc_init(void);

/**
 * Called when CRM model is updated (slave mode)
 * Preprocesses data for DTR ISR use and calculates alignment
 */
void dtc_crm_updated(void);

#ifdef __cplusplus
}
#endif
