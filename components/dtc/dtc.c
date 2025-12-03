/**
 * DTC - Disciplined Timer Controller
 *
 * Preprocesses CRM data for DTR realtime ISR.
 */

#include "dtc.h"
#include "dtr.h"
#include "crm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "dtc";

// First update flag
static bool s_first_update = true;

esp_err_t dtc_init(void)
{
    // Register with CRM for updates
    crm_register_callback(dtc_crm_updated);

    ESP_LOGI(TAG, "DTC module initialized");

#ifdef CONFIG_FTS_CSV_OUTPUT
    printf("DTC,cycle,local_ticks,base_period_fp16\n");
#endif

    return ESP_OK;
}

// ============================================================================
// Helper functions for CRM transformations (per slides 16-19)
// ============================================================================

/**
 * Project local ticks to remote ticks using CRM model (Slide 16)
 */
static int64_t dtc_local_to_remote(int64_t local_ticks,
                                    int64_t crm_ref_local_ticks,
                                    int64_t crm_ref_remote_ticks)
{
    int64_t delta_local_ticks = local_ticks - crm_ref_local_ticks;
    int64_t delta_remote_ticks = delta_local_ticks +
                                  (int64_t)((double)delta_local_ticks * crm_slope_rl_m1);
    return crm_ref_remote_ticks + delta_remote_ticks;
}

/**
 * Project remote ticks back to local ticks using CRM model (Slide 18)
 */
static int64_t dtc_remote_to_local(int64_t remote_ticks,
                                    int64_t crm_ref_local_ticks,
                                    int64_t crm_ref_remote_ticks)
{
    int64_t delta_remote_ticks = remote_ticks - crm_ref_remote_ticks;
    int64_t delta_local_ticks = delta_remote_ticks +
                                 (int64_t)((double)delta_remote_ticks * crm_slope_lr_m1);
    return crm_ref_local_ticks + delta_local_ticks;
}

/**
 * Calculate adjusted timer period from CRM slope (Slide 19)
 * aligned_base_period_fp16 = master_base_period_fp16 + master_base_period_fp16 * crm_slope_lr_m1
 * Returns combined FP16 value: period_ticks * 65536 + fraction
 */
static int64_t dtc_calculate_period_fp16(void)
{
    const double master_base_period_fp16 = DTR_TIMER_PERIOD_TICKS * (double)FP16_SCALE;
    double aligned_base_period_fp16 = master_base_period_fp16 +
                                       master_base_period_fp16 * crm_slope_lr_m1;
    return (int64_t)aligned_base_period_fp16;
}

// ============================================================================
// Main DTC update function (new flow per slides)
// ============================================================================

void dtc_crm_updated(void)
{
    if (!crm_valid) {
        return;
    }

#ifdef CONFIG_FTS_TEST_ALIGN_ONCE
    // Test mode: skip re-alignment after initial alignment
    if (!s_first_update) {
        return;
    }
#endif

    // Register this task (FTM task) for TEZ notifications
    dtr_register_tez_listener(xTaskGetCurrentTaskHandle()); // FIXME repeated registrations

    // Clear any stale notification
    xTaskNotifyStateClear(NULL);

    // Step 1: Wait for TEZ to get fresh timer_base_ticks
    dtr_wait_for_tez();

    // Step 2: Sample timer_base_ticks immediately after TEZ
    int64_t timer_base = dtr_get_timer_base_ticks();

    // Step 3: Convert CRM refs to ticks
    int64_t crm_ref_local_ticks = crm_local_ref_ps / DTR_PS_PER_TICK;
    int64_t crm_ref_remote_ticks = crm_remote_ref_ps / DTR_PS_PER_TICK;

    // Step 4: Calculate remote_ticks at current timer position (Slide 16)
    int64_t remote_ticks = dtc_local_to_remote(timer_base,
                                                crm_ref_local_ticks,
                                                crm_ref_remote_ticks);

    // Step 5: Calculate aligned cycle counter (Slide 17)
    // Round up to next period boundary, then add 2 cycles
    // +2 because: current cycle is in progress, DTR applies alignment one cycle later
    int64_t aligned_cycle_counter = (remote_ticks + DTR_TIMER_PERIOD_TICKS / 2) / DTR_TIMER_PERIOD_TICKS + 2;
    int64_t aligned_remote_ticks = aligned_cycle_counter * DTR_TIMER_PERIOD_TICKS;

    // Step 6: Project back to local ticks (Slide 18)
    int64_t aligned_local_ticks = dtc_remote_to_local(aligned_remote_ticks,
                                                       crm_ref_local_ticks,
                                                       crm_ref_remote_ticks);

    // Apply compensation
    int32_t compensation_ticks = DTR_NS_TO_TICKS(DTR_COMPENSATION_NS);
    aligned_local_ticks += compensation_ticks;

    // Step 7: Calculate adjusted period (Slide 19)
    int64_t aligned_base_period_fp16 = dtc_calculate_period_fp16();

    // Step 8: Set alignment parameters (per Slide 19, logging is in DTR)
    dtr_set_align_request(aligned_cycle_counter,
                              aligned_local_ticks,
                              aligned_base_period_fp16);

#ifdef CONFIG_FTS_CSV_OUTPUT
    printf("DTC,%lld,%lld,%lld\n",
           (long long)aligned_cycle_counter,
           (long long)aligned_local_ticks,
           (long long)aligned_base_period_fp16);
#endif

    // Step 11: Wait for alignment to be applied
    dtr_wait_for_tez();

    // Step 12: Grab and log feedback
    dtr_grab_n_log_align_feedback();

    // Update state
    if (s_first_update) {
        s_first_update = false;
#ifdef CONFIG_FTS_TEST_ALIGN_ONCE
        ESP_LOGI(TAG, "No further realignments - CONFIG_FTS_TEST_ALIGN_ONCE enabled!");
#endif
    }
}