/**
 * CRM - Clock Relationship Model
 *
 * Performs regression analysis on FTM timestamps.
 */

#include "crm.h"
#include "ftm.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

// Regression configuration
#define MAX_REGRESSION_SAMPLES (2*FTM_FRAMES_PER_SESSION)
#define MIN_REGRESSION_SAMPLES (FTM_FRAMES_PER_SESSION / 2)
#define CRM_R_SQUARED_THRESHOLD 0.999f

static const char *TAG = "crm";

// Exported, updated by perform_regression() which is called from

// - crm_process_ftm_report() - is a callback invoked by FTM after each new session
// - calls add_regression_sample() to add data points from new FTM reports to s_regression_samples[] ring buffer
// - calls process_regression() to run the regression
// - calls the callback, which is normally DTC's dtc_crm_updated()
// - calls ESP_LOGI() to log results of the regression to console

// Global CRM state
bool crm_valid = false;
double crm_slope_lr_m1 = 0.0;  // Default: no frequency offset (slope = 1.0)
double crm_slope_rl_m1 = 0.0;  // Inverse of above
int64_t crm_local_ref_ps = 0;
int64_t crm_remote_ref_ps = 0;

static struct {
    int64_t local_ps[MAX_REGRESSION_SAMPLES];
    int64_t remote_ps[MAX_REGRESSION_SAMPLES];
    uint32_t count;
    uint32_t head;  // Circular buffer head
} s_regression_samples = {0};

// Callback
static crm_update_callback_t s_callback = NULL;

// Internal model state for logging
static float s_r_squared = 0.0f;
static float s_residual_std_ns = 0.0f;
static uint32_t s_sample_count = 0;

/**
 * Add sample to regression buffer (circular, keeps most recent MAX_REGRESSION_SAMPLES)
 */
static void add_regression_sample(int64_t local_ps, int64_t remote_ps)
{
    if (s_regression_samples.count < MAX_REGRESSION_SAMPLES) {
        s_regression_samples.local_ps[s_regression_samples.count] = local_ps;
        s_regression_samples.remote_ps[s_regression_samples.count] = remote_ps;
        s_regression_samples.count++;
    } else {
        // Circular buffer: overwrite oldest
        s_regression_samples.local_ps[s_regression_samples.head] = local_ps;
        s_regression_samples.remote_ps[s_regression_samples.head] = remote_ps;
        s_regression_samples.head = (s_regression_samples.head + 1) % MAX_REGRESSION_SAMPLES;
    }
}

/**
 * Perform linear regression: local_ps = slope * (remote_ps - t_ref_remote) + t_ref_local
 * Uses reference-point method to avoid precision loss
 * Returns true if successful
 */
static bool perform_regression(void)
{
    if (s_regression_samples.count < MIN_REGRESSION_SAMPLES) {
        ESP_LOGW(TAG, "Insufficient samples for regression: %lu", (unsigned long)s_regression_samples.count);
        return false;
    }

    uint32_t n = s_regression_samples.count;

    // Use first sample as reference for numerical stability
    int64_t ref_x = s_regression_samples.remote_ps[0];
    int64_t ref_y = s_regression_samples.local_ps[0];

    // Calculate means using deltas from reference
    double sum_dx = 0.0, sum_dy = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        sum_dx += (double)(s_regression_samples.remote_ps[i] - ref_x);
        sum_dy += (double)(s_regression_samples.local_ps[i] - ref_y);
    }
    double mean_dx = sum_dx / n;
    double mean_dy = sum_dy / n;
    double mean_x = (double)ref_x + mean_dx;
    double mean_y = (double)ref_y + mean_dy;

    // Calculate slope_minus_one directly: (num - den) / den
    double num = 0.0, den = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double dx = (double)s_regression_samples.remote_ps[i] - mean_x;
        double dy = (double)s_regression_samples.local_ps[i] - mean_y;
        num += dx * dy;
        den += dx * dx;
    }

    if ((den == 0.0) || (num == 0.0)) {
        ESP_LOGW(TAG, "Regression failed: zero denominator or numerator");
        return false;
    }

    // Calculate both slopes in "minus one" format for numerical precision
    crm_slope_lr_m1 = (num - den) / den;     // Forward: local/remote - 1
    crm_slope_rl_m1 = (den - num) / num;     // Inverse: remote/local - 1

    // Use centroid as reference point (lies on regression line, maximally stable)
    int64_t t_ref_local_ps = (int64_t)mean_y;
    int64_t t_ref_remote_ps = (int64_t)mean_x;

    // Calculate R² and residual std
    // FIXME - the computations below are very costly
    double ss_tot = 0.0, ss_res = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double delta_remote = (double)s_regression_samples.remote_ps[i] - (double)t_ref_remote_ps;
        double y_pred = (double)t_ref_local_ps + delta_remote + delta_remote * crm_slope_lr_m1;
        double residual = (double)s_regression_samples.local_ps[i] - y_pred;
        ss_res += residual * residual;
        ss_tot += ((double)s_regression_samples.local_ps[i] - mean_y) *
                  ((double)s_regression_samples.local_ps[i] - mean_y);
    }

    float r_squared = (float)((ss_tot > 0) ? (1.0 - ss_res / ss_tot) : 0.0);
    float residual_std = (float)(sqrt(ss_res / n) / 1e3);  // In nanoseconds
    crm_local_ref_ps = t_ref_local_ps;
    crm_remote_ref_ps = t_ref_remote_ps;
    crm_valid = (r_squared > CRM_R_SQUARED_THRESHOLD);

    // Update internal state for logging
    s_r_squared = r_squared;
    s_residual_std_ns = residual_std;
    s_sample_count = n;

    return true;
}

esp_err_t crm_init(void)
{
    // Reset all state
    memset(&s_regression_samples, 0, sizeof(s_regression_samples));
    crm_valid = false;
    crm_slope_lr_m1 = 0.0;  // No frequency offset initially (slope = 1.0)
    crm_slope_rl_m1 = 0.0;
    crm_local_ref_ps = 0;
    crm_remote_ref_ps = 0;
    s_callback = NULL;

    ESP_LOGI(TAG, "CRM module initialized");
    return ESP_OK;
}

void crm_register_callback(crm_update_callback_t callback)
{
    s_callback = callback;
    ESP_LOGI(TAG, "Callback %s", callback ? "registered" : "unregistered");
}

void crm_process_ftm_report(uint32_t session_number,
                            const int64_t *t1_ps,
                            const int64_t *t2_ps,
                            const int64_t *t3_ps,
                            const int64_t *t4_ps,
                            uint8_t count)
{
    if (count == 0) {
        ESP_LOGE(TAG, "No new FTM data to process");
        return;
    }

    // Process all entries
    for (uint8_t i = 0; i < count; i++) {
        // Calculate RTT: rtt = (t4 - t1) - (t3 - t2)
        int64_t rtt_ps = (t4_ps[i] - t1_ps[i]) - (t3_ps[i] - t2_ps[i]);

        // Remote time at slave RX (t2) = t1 + rtt/2
        int64_t local_at_t2_ps = t2_ps[i];
        int64_t remote_at_t2_ps = t1_ps[i] + rtt_ps / 2;

        // Add to regression data
        add_regression_sample(local_at_t2_ps, remote_at_t2_ps);
    }

    // Perform regression
    if (perform_regression()) {
        // Invoke callback if registered
        if (s_callback) {
            s_callback();
        }

#ifdef CONFIG_FTS_CSV_OUTPUT
        // CSV logging - FIXME header should be printed once at startup
        printf("REGR,%u,%lu,%lu,%.9e,%.3f,%.12f,%lld,%lld\n",
               count,
               (unsigned long)session_number,
               (unsigned long)s_sample_count,
               crm_slope_lr_m1,
               s_residual_std_ns,
               s_r_squared,
               (long long)crm_local_ref_ps,
               (long long)crm_remote_ref_ps);
#endif

        ESP_LOGI(TAG, "Regression: samples=%lu (+%u), R²=%.3lf, std=%.3lf ns, ppm_lr_m1=%.9lf, ppm_rl_m1=%.9lf",
                 (unsigned long)s_sample_count,
                 count,
                 s_r_squared,
                 s_residual_std_ns,
                 crm_slope_lr_m1 * 1e6f,
                 crm_slope_rl_m1 * 1e6f);
    }
}
