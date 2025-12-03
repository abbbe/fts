/**
 * Clock Utilities
 *
 * Provides:
 * - Unwrapped 64-bit microsecond counter based on WiFi MAC hardware clock
 * - Generic counter unwrapping
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Counter Unwrapping
// ============================================================================

/**
 * Unwrap state structure (uniform for all counter types)
 *
 * Tracks wrap detection state for counters that overflow periodically.
 * Supports dual-wrap detection for special cases (e.g., t1/t4 FTM timestamps).
 */
typedef struct {
    int64_t last_val;       ///< Last raw value seen
    int64_t offset;         ///< Cumulative offset from wraps
    uint32_t wrap_count;    ///< Number of wraps detected
    uint64_t wrap_value;    ///< Primary wrap period (e.g., 2^32 for 32-bit counters)
    uint64_t wrap_value2;   ///< Secondary wrap period (0 if not used, for abnormal wraps)
} unwrap_state_t;

/**
 * Unwrap counter value with optional dual-wrap detection
 *
 * Generic unwrapping function for counters that overflow periodically.
 * Detects wraps by comparing current value to last value.
 * Updates state and returns unwrapped value.
 *
 * @param val Current raw value
 * @param state Unwrap state structure (updated by this function)
 * @return Unwrapped value (value + offset)
 */
int64_t clock_unwrap(int64_t val, unwrap_state_t *state);

// ============================================================================
// MAC Clock
// ============================================================================

/**
 * Initialize MAC clock
 *
 * Must be called after WiFi is started and power saving is disabled.
 * Creates watchdog task to ensure wrap detection every hour.
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t clock_init(void);

/**
 * Get unwrapped MAC clock in microseconds
 *
 * Returns 64-bit monotonic microsecond counter.
 * Handles 32-bit MAC clock wraps transparently (~71 minutes).
 *
 * FATAL: Aborts if called before clock_init()
 *
 * @return Unwrapped MAC clock value in microseconds
 */
int64_t clock_get_us(void);

/**
 * Get MAC clock base (offset from wraps)
 *
 * Returns the cumulative offset from wraps, used for detecting wraps
 * during tight measurement loops.
 *
 * Typical usage: base = clock_get_base(); abs = base + raw_mac_value;
 *
 * FATAL: Aborts if called before clock_init()
 *
 * @return Cumulative wrap offset in microseconds
 */
int64_t clock_get_base_us(void);

#ifdef __cplusplus
}
#endif
