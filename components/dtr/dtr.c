/**
 * DTR - Disciplined Timer Realtime
 *
 * MCPWM-based synchronized timer with ISR.
 */

#include "dtr.h"
#include "dtc.h"
#include "clock.h"
#include "driver/mcpwm_timer.h"
#include "driver/mcpwm_oper.h"
#include "driver/mcpwm_cmpr.h"
#include "driver/mcpwm_gen.h"
#include "hal/mcpwm_ll.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

#if CONFIG_FTS_MAC_TIMER_ALIGNMENT_TEST_CYCLES
#include "esp_random.h"
#endif // CONFIG_FTS_MAC_TIMER_ALIGNMENT_TEST_CYCLES

static const char *TAG = "dtr";

// DTR mode
static dtr_mode_t s_mode = DTR_MODE_SLAVE;

// State
static dtr_state_t s_state = DTR_STATE_NOT_STARTED;

// next 3 lines are specific to MCPWM with its ISR firing as soon as timer starts & shadow register mechanism
static int64_t cycle_counter = -1;  // First TEZ increments to 0
static uint16_t active_period_ticks = 0;   // What's in HW active register (0 at start: first TEZ fires immediately)
static uint16_t shadow_period_ticks = DTR_TIMER_PERIOD_TICKS;  // What's in HW shadow register

static int64_t timer_base_ticks = 0;
static int64_t period_ticks = DTR_TIMER_PERIOD_TICKS;
static uint32_t base_period_fp16 = (uint32_t)DTR_TIMER_PERIOD_TICKS * FP16_SCALE;
static int32_t period_ticks_frac_acc = 0;

// MCPWM handles
static mcpwm_timer_handle_t s_timer = NULL;
static mcpwm_oper_handle_t s_operator = NULL;
static mcpwm_cmpr_handle_t s_comparator = NULL;
static mcpwm_gen_handle_t s_generator = NULL;

// Application callback
static fts_callback_t s_app_callback = NULL;

// Alignment parameters (bidirectional communication between DTC and ISR)
typedef struct
{
    // Command from DTC → ISR
    bool pending;                     // Set by DTC when alignment requested, cleared by ISR
    int64_t aligned_local_ticks;      // Target tick position
    int64_t aligned_cycle_counter;    // Target cycle number
    int64_t aligned_base_period_fp16; // Fractional period for dithering (16-bit FP, 0-65535)
} align_request_t;

static align_request_t s_align_request = {0};
static align_feedback_t s_align_feedback = {0};

// Spinlock for ISR synchronization
static portMUX_TYPE s_spinlock = portMUX_INITIALIZER_UNLOCKED;

// Task handle for TEZ notifications (registered by DTC)
static TaskHandle_t s_tez_listener_task = NULL;

/**
 * Timer empty (TEZ) ISR callback - State Machine Implementation
 * Fires at start of each period
 */
static bool IRAM_ATTR dtr_tez_handler(mcpwm_timer_handle_t timer,
                                      const mcpwm_timer_event_data_t *edata,
                                      void *user_ctx)
{
    portENTER_CRITICAL_ISR(&s_spinlock);

    // --- common beginning part
    cycle_counter++;
    timer_base_ticks += active_period_ticks;  // Account for the period that actually elapsed
    active_period_ticks = shadow_period_ticks;  // MCPWN hardware has loaded period from the shadow register

    if (s_align_request.pending) {
        // Capture old values for delta calculation (per slide 23)
        int64_t old_cycle_counter = cycle_counter;
        int64_t old_period_ticks = period_ticks;

        // Apply alignment - set cycle to target (increment already done above)
        cycle_counter = s_align_request.aligned_cycle_counter;
        period_ticks = s_align_request.aligned_local_ticks - timer_base_ticks;
        base_period_fp16 = s_align_request.aligned_base_period_fp16;
        period_ticks_frac_acc = 0;
        
        // roll forward if the period too short
        while (period_ticks < DTR_MIN_PERIOD_TICKS) {
            // --- recalculate next period_ticks, with dithering (copy paste from below)
            period_ticks += base_period_fp16 / FP16_SCALE;
            period_ticks_frac_acc += base_period_fp16 % FP16_SCALE;
            if (period_ticks_frac_acc >= FP16_SCALE)
            {
                period_ticks++;
                period_ticks_frac_acc -= FP16_SCALE;
            }

            cycle_counter++;
            // this creates discontinunity in cycle numbering, but should only happen on the initial alignment
        }

        // Provide feedback (per slide 23)
        s_align_request.pending = false;
        assert(!s_align_feedback.ready);

        s_align_feedback.cycle_counter = cycle_counter;
        s_align_feedback.cycle_delta = (int32_t)(cycle_counter - old_cycle_counter);
        s_align_feedback.period_ticks = period_ticks;
        s_align_feedback.period_ticks_delta = (int32_t)(period_ticks - old_period_ticks);
        s_align_feedback.ready = true;

        if (s_state == DTR_STATE_RUNNING) {
            s_state = DTR_STATE_ALIGNED;

#ifdef CONFIG_FTS_ROLE_SLAVE
            // Release GPIO force - allow hardware-generated pulses now that we're aligned
            mcpwm_generator_set_force_level(s_generator, -1, true);
#endif
        }
    } else {
        // --- recalculate next period_ticks, with dithering
        period_ticks = base_period_fp16 / FP16_SCALE;
        period_ticks_frac_acc += base_period_fp16 % FP16_SCALE;
        if (period_ticks_frac_acc >= FP16_SCALE)
        {
            period_ticks++;
            period_ticks_frac_acc -= FP16_SCALE;
        }
    }

    // common end part
    portEXIT_CRITICAL_ISR(&s_spinlock);

    // Load new period into shadow register (becomes active at next TEZ)
    // Safety check: MCPWM timer has 16-bit counter (max 65535)
    if (period_ticks <= 0 || period_ticks > 65535) {
        ets_printf("FATAL: period_ticks=%lld out of range [1,65535]\n", (long long)period_ticks);
        abort();
    }
    mcpwm_timer_set_period(timer, (uint16_t)period_ticks);
    shadow_period_ticks = (uint16_t)period_ticks;

    if (s_tez_listener_task != NULL)
    {
        xTaskNotifyFromISR(s_tez_listener_task, 0, eNoAction, NULL);
    }

    if ((s_state == DTR_STATE_ALIGNED) && s_app_callback)
    {
        s_app_callback(cycle_counter);
    }

    return false;
}

/**
 * Read MCPWM timer counter register directly (HAL layer)
 */
static inline uint32_t dtr_read_timer_count(void)
{
    // Access MCPWM group 0, timer 0 counter register
    return mcpwm_ll_timer_get_count_value(MCPWM_LL_GET_HW(0), 0);
}

// ============================================================================
// MAC Clock / MCPWM Timer Offset Measurement
// ============================================================================

/**
 * Catch MAC clock transition
 *
 * @param timer_abs_before Absolute timer ticks before MAC transition
 * @param mac_clock_transition_us MAC time in microseconds after transition
 * @param timer_abs_after Absolute timer ticks after MAC transition
 * @param iterations_out Number of loop iterations before catching edge (for debugging)
 * @return true if successful (no wrap), false if wrap detected (discard sample)
 */
static bool s_catch_mac_clock_transition(int64_t *timer_abs_before_ticks,
                                         int64_t *mac_clock_transition_us,
                                         int64_t *timer_abs_after_ticks,
                                         uint32_t *iterations_out)
{
    uint32_t timer_pre, timer_before, timer_after, timer_post;
    uint32_t mac_clock_us_pre, mac_clock_us_1, mac_clock_us_2, mac_clock_us_post;
    int64_t timer_base_ticks_pre, mac_clock_base_us;
    uint32_t iterations = 0;

    // Sample before critical section (gives pending ISR chance to run)
    timer_pre = dtr_read_timer_count();
    mac_clock_us_pre = esp_wifi_internal_get_mac_clock_time();
    mac_clock_base_us = clock_get_base_us();

    // Hold DTR ISR's spinlock for entire measurement (1-2µs) to ensure consistency
    portENTER_CRITICAL(&s_spinlock);

    timer_base_ticks_pre = timer_base_ticks;

    // Tight loop to catch MAC transition (inside critical section)
    do
    {
        iterations++;

        timer_before = dtr_read_timer_count();
        mac_clock_us_1 = esp_wifi_internal_get_mac_clock_time();
        mac_clock_us_2 = esp_wifi_internal_get_mac_clock_time();
        timer_after = dtr_read_timer_count();
    } while (mac_clock_us_2 == mac_clock_us_1);

    timer_post = dtr_read_timer_count();
    mac_clock_us_post = esp_wifi_internal_get_mac_clock_time();

    portEXIT_CRITICAL(&s_spinlock);

    // Check timer monotonicity (detect timer wrap)
    if (timer_before < timer_pre || timer_after < timer_before || timer_post < timer_after)
    {
        return false; // Timer wrapped, discard the sample
    }

    // Check MAC monotonicity (detect MAC wrap)
    if (mac_clock_us_1 < mac_clock_us_pre || mac_clock_us_2 < mac_clock_us_1 || mac_clock_us_post < mac_clock_us_2)
    {
        return false; // MAC wrapped, discard the sample
    }

    // Calculate absolute values using ISR-tracked bases
    *timer_abs_before_ticks = timer_base_ticks_pre + timer_before;
    *timer_abs_after_ticks = timer_base_ticks_pre + timer_after;
    *mac_clock_transition_us = mac_clock_base_us + mac_clock_us_2;
    *iterations_out = iterations;

    return true;
}

/**
 * Refine the estimate of the moment the timer was started
 * Updates min/max bounds (in ticks) passed by pointer
 */
static void s_refine_mac_clock_timer_start_offset(int64_t *min, int64_t *max,
                                                  int64_t timer_abs_before_ticks,
                                                  int64_t mac_clock_transition_us,
                                                  int64_t timer_abs_after_ticks)
{
    int64_t mac_at_transition_ticks = mac_clock_transition_us * TIMER_TICKS_PER_US;

    // Sanity check: timer should be less than MAC (positive offset)
    assert(timer_abs_after_ticks < mac_at_transition_ticks);

    int64_t new_min = mac_at_transition_ticks - timer_abs_after_ticks;
    int64_t new_max = mac_at_transition_ticks - timer_abs_before_ticks;

    // Sanity check: should always hold since timer_before < timer_after
    assert(new_min <= new_max);

    // Narrow the range
    if (new_min > *min)
        *min = new_min;
    if (new_max < *max)
        *max = new_max;
}

/**
 * Measure the offset betwen MAC clock start and Timer start, in timer ticks
 *
 * @param run_id Run identifier for test mode (0, unused during normal operation)
 */
static int64_t s_measure_mac_clock_timer_start_offset(uint32_t run_id)
{
    // Local bounds for this measurement (reentrant, no global state)
    int64_t offset_ticks_min = 0;         // Will be refined upward
    int64_t offset_ticks_max = INT64_MAX; // Will be refined downward

    // track statistics about the tight loop iterations
    uint32_t total_iterations = 0;
    uint32_t min_iterations = UINT32_MAX;
    uint32_t max_iterations = 0;

    ESP_LOGI(TAG, "Measuring MAC clock / timer start offset, %lu samples...",
             (unsigned long)DTR_MAC_TIMER_ALIGNMENT_MAX_SAMPLES);

    for (uint32_t samples = 0; samples < DTR_MAC_TIMER_ALIGNMENT_MAX_SAMPLES; samples++)
    {
        uint32_t iterations;
        int64_t mac_clock_transition_us, timer_abs_before, timer_abs_after;

        // Catch transition with wrap detection (uses ISR's timer_base_ticks and mac_clock_base_us)
        if (!s_catch_mac_clock_transition(&timer_abs_before, &mac_clock_transition_us, &timer_abs_after,
                                          &iterations))
        {
            // Timer or MAC wrap detected during this sample, discard and continue
            continue;
        }

        // Track tight loop iteration stats
        total_iterations += iterations;
        if (iterations < min_iterations)
            min_iterations = iterations;
        if (iterations > max_iterations)
            max_iterations = iterations;

        // Refine estimate using absolute ticks (ISR handles wraps)
        s_refine_mac_clock_timer_start_offset(&offset_ticks_min, &offset_ticks_max,
                                              timer_abs_before, mac_clock_transition_us, timer_abs_after);

        if ((samples & 0xFFFF) == 0)
            vTaskDelay(1); // let other tasks run every once in a while
    }

    // Use midpoint of the final range
    int64_t offset_ticks = (offset_ticks_min + offset_ticks_max) / 2;
    float avg_iterations = (float)total_iterations / DTR_MAC_TIMER_ALIGNMENT_MAX_SAMPLES;

    ESP_LOGI(TAG, "Offset: [%lld - %lld = %lld] avg %lld (%.3lf us), loop: avg=%.3f, min=%lu, max=%lu",
             (long long)offset_ticks_max, (long long)offset_ticks_min,
             (long long)(offset_ticks_max - offset_ticks_min),
             (long long)offset_ticks, (double)offset_ticks / TIMER_TICKS_PER_US,
             avg_iterations, min_iterations, max_iterations);

#if CONFIG_FTS_MAC_TIMER_ALIGNMENT_TEST_CYCLES
    // CSV output for test mode
    printf("MAC_TIMER_ALIGN,%lu,%lld,%lld,%lld\n", (unsigned long)run_id, (long long)offset_ticks, (long long)offset_ticks_min, (long long)offset_ticks_max);
#endif

    return offset_ticks;
}

/**
 * Initialize MCPWM hardware
 * Sets up timer, operator, comparator, and generator
 * Does NOT enable or start the timer
 *
 * @param pulse_gpio GPIO pin for hardware pulse generation
 * @return ESP_OK on success
 */
static esp_err_t s_init_hardware(gpio_num_t pulse_gpio, uint16_t init_period_ticks)
{
    // Create MCPWM timer
    mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = DTR_TIMER_RESOLUTION_HZ,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = init_period_ticks,
        .flags.update_period_on_empty = true  // Period updates at TEZ only (shadow register)
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &s_timer));

    // Register TEZ handler (must be done while timer is in init state)
    mcpwm_timer_event_callbacks_t cbs = {
        .on_empty = dtr_tez_handler,
    };
    ESP_ERROR_CHECK(mcpwm_timer_register_event_callbacks(s_timer, &cbs, NULL));

    // Create operator
    mcpwm_operator_config_t operator_config = {
        .group_id = 0,
    };
    ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &s_operator));

    // Connect timer to operator
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(s_operator, s_timer));

    // Create comparator for GPIO pulse generation
    mcpwm_comparator_config_t comparator_config = {
        .flags.update_cmp_on_tez = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_comparator(s_operator, &comparator_config, &s_comparator));

    // Set compare value for pulse width (duty cycle defined in dtr.h)
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(s_comparator, DTR_PULSE_WIDTH_TICKS));

    // Create generator
    mcpwm_generator_config_t generator_config = {
        .gen_gpio_num = pulse_gpio,
    };
    ESP_ERROR_CHECK(mcpwm_new_generator(s_operator, &generator_config, &s_generator));

    // Configure generator actions:
    // - Set HIGH on TEZ (timer empty, count = 0)
    // - Set LOW on compare match
    // Result: pulse at start of each period with configured duty cycle
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(s_generator,
                                                              MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(s_generator,
                                                                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_comparator, MCPWM_GEN_ACTION_LOW)));

#ifdef CONFIG_FTS_ROLE_SLAVE
    // Force GPIO LOW until slave timer is aligned
    ESP_ERROR_CHECK(mcpwm_generator_set_force_level(s_generator, 0, true));
#endif // CONFIG_FTS_ROLE_SLAVE

    return ESP_OK;
}

static esp_err_t s_enable_and_start_timer(void)
{
    // Initialize counters to 0 and enter RUNNING state
    portENTER_CRITICAL(&s_spinlock);
    timer_base_ticks = 0;
    cycle_counter = 0;
    s_state = DTR_STATE_RUNNING;
    portEXIT_CRITICAL(&s_spinlock);

    // Enable and start timer NOW (MAC is already running)
    ESP_ERROR_CHECK(mcpwm_timer_enable(s_timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP));

    return ESP_OK;
}

esp_err_t dtr_init(dtr_mode_t mode, fts_callback_t callback, gpio_num_t pulse_gpio)
{
    s_mode = mode;
    s_app_callback = callback;

    // Initialize state
    s_state = DTR_STATE_NOT_STARTED;
    cycle_counter = -1;  // First TEZ increments to 0
    timer_base_ticks = 0;
    period_ticks = DTR_TIMER_PERIOD_TICKS;
    active_period_ticks = 0;  // First TEZ: nothing elapsed yet
    shadow_period_ticks = DTR_TIMER_PERIOD_TICKS;  // Initial period in shadow
    base_period_fp16 = (uint32_t)DTR_TIMER_PERIOD_TICKS * FP16_SCALE;
    period_ticks_frac_acc = 0;

    // Initialize MCPWM hardware with operational period from start
    ESP_ERROR_CHECK(s_init_hardware(pulse_gpio, period_ticks));

    ESP_LOGI(TAG, "DTR initialized: %s, %llu MHz, period=%lu ticks, GPIO %d",
             (mode == DTR_MODE_MASTER ? "MASTER" : "SLAVE"),
             DTR_TIMER_RESOLUTION_HZ / 1000000,
             (unsigned long)DTR_TIMER_PERIOD_TICKS,
             pulse_gpio);
#ifdef CONFIG_FTS_ROLE_SLAVE
    ESP_LOGI(TAG, "Compensation: %d ns", DTR_COMPENSATION_NS);
#endif // CONFIG_FTS_ROLE_SLAVE

#ifdef CONFIG_FTS_CSV_OUTPUT
    printf("DTR,cycle,cycle_delta,period_ticks,period_ticks_delta\n");
#endif

    return ESP_OK;
}

void dtr_start_timer(void)
{
    if (s_state != DTR_STATE_NOT_STARTED)
    {
        ESP_LOGE(TAG, "Timer already started");
        abort();
    }

    // Enable and start timer (waits for MAC clock, sets UNDISCIPLINED state)
    ESP_ERROR_CHECK(s_enable_and_start_timer());

#if CONFIG_FTS_MAC_TIMER_ALIGNMENT_TEST_CYCLES
    // Test mode: perform multiple measurements and output CSV
    // Include a random session ID to distinguish runs
    uint32_t run_id = esp_random();

    printf("MAC_TIMER_ALIGN,run,offset_ticks,offset_ticks_min,offset_ticks_max\n");

    for (int i = 0; i < CONFIG_FTS_MAC_TIMER_ALIGNMENT_TEST_CYCLES; i++)
    {
        s_measure_mac_clock_timer_start_offset(run_id);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Done %d test cycles, restarting...", CONFIG_FTS_MAC_TIMER_ALIGNMENT_TEST_CYCLES);
    esp_restart();
#else
    int64_t offset = s_measure_mac_clock_timer_start_offset(0);
    portENTER_CRITICAL(&s_spinlock);
    timer_base_ticks += offset;
    portEXIT_CRITICAL(&s_spinlock);
#endif
}

void dtr_wait_for_tez(void)
{
    if (xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "TEZ notification timeout");
        abort();
    }
}

void dtr_align_master_timer(void)
{
    // Register this task for TEZ notifications
    dtr_register_tez_listener(xTaskGetCurrentTaskHandle());

    // Clear any stale notification
    xTaskNotifyStateClear(NULL);

    // Wait for TEZ to get fresh timer_base_ticks
    dtr_wait_for_tez();

    // Sample timer_base_ticks immediately after TEZ
    int64_t timer_base_ticks = dtr_get_timer_base_ticks();

    // Find next aligned TEZ time from CURRENT position
    int64_t current_cycle_count = timer_base_ticks / DTR_TIMER_PERIOD_TICKS;
    int64_t aligned_cycle_count = current_cycle_count + 2;
    int64_t aligned_local_ticks = aligned_cycle_count * DTR_TIMER_PERIOD_TICKS;

    // Set alignment parameters (no frequency adjustment, nominal period)
    dtr_set_align_request(aligned_cycle_count,
                          aligned_local_ticks,
                          DTR_TIMER_PERIOD_TICKS * FP16_SCALE);

    ESP_LOGI(TAG, "Master alignment baseline: %lld ticks (%.3f us), cycle=%lld",
             (long long)timer_base_ticks, (float)timer_base_ticks / TIMER_TICKS_PER_US, (long long)current_cycle_count);

    // Wait for alignment to be applied
    dtr_wait_for_tez();

    dtr_grab_n_log_align_feedback();

    // Unregister from TEZ notifications
    dtr_register_tez_listener(NULL);
}

void dtr_set_align_request(int64_t aligned_cycle_counter,
                           int64_t aligned_local_ticks,
                           int64_t aligned_base_period_fp16)
{
    portENTER_CRITICAL(&s_spinlock);
    s_align_request.aligned_cycle_counter = aligned_cycle_counter;
    s_align_request.aligned_local_ticks = aligned_local_ticks;
    s_align_request.aligned_base_period_fp16 = aligned_base_period_fp16;
    s_align_request.pending = true;

    s_align_feedback.ready = false;
    portEXIT_CRITICAL(&s_spinlock);

    // log the alignment request - outside of the critical section, after updating shared state
    // float base_period_float = (float)aligned_base_period_fp16 / FP16_SCALE;
    ESP_LOGI(TAG, "Alignment request: cycle=%lld, ticks=%lld, base_period=%lld FP16 %ld",
             (long long)aligned_cycle_counter,
             (long long)aligned_local_ticks,
             // base_period_float,
             (long long)aligned_base_period_fp16 / FP16_SCALE,
             (long long)aligned_base_period_fp16 % FP16_SCALE);
}

void dtr_grab_n_log_align_feedback(void)
{
    // Grab the feedback structure under spinlock
    align_feedback_t feedback;
    portENTER_CRITICAL(&s_spinlock);
    feedback = s_align_feedback;
    portEXIT_CRITICAL(&s_spinlock);

    assert(feedback.ready);

    ESP_LOGI(TAG, "Alignment feedback: period_ticks=%ld (%+ld), cycle=%lld (%+ld)",
             feedback.period_ticks, feedback.period_ticks_delta,
             (long long)feedback.cycle_counter, feedback.cycle_delta);

#ifdef CONFIG_FTS_CSV_OUTPUT
    printf("DTR,%lld,%ld,%ld,%ld\n",
           (long long)feedback.cycle_counter,
           feedback.cycle_delta,
           feedback.period_ticks,
           feedback.period_ticks_delta);
#endif
}

int64_t dtr_get_timer_base_ticks(void)
{
    portENTER_CRITICAL(&s_spinlock);
    int64_t ticks = timer_base_ticks;
    portEXIT_CRITICAL(&s_spinlock);
    return ticks;
}

uint32_t dtr_get_master_cycle(void)
{
    portENTER_CRITICAL(&s_spinlock);
    uint32_t cycle = cycle_counter;
    portEXIT_CRITICAL(&s_spinlock);
    return cycle;
}

void dtr_register_tez_listener(TaskHandle_t task_handle)
{
    s_tez_listener_task = task_handle;
}
