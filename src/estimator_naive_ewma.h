#ifndef ESTIMATOR_NAIVE_EWMA_H
#define ESTIMATOR_NAIVE_EWMA_H

#include "rate_estimator.h"

#include <stdbool.h>
#include <stdint.h>

/* Start the naive-ewma estimator runtime (RTCP-feedback-only).
 * Returns 0 on success, -1 on failure. */
int estimator_naive_ewma_start(void);

/* Stop the monitor thread. Safe to call even if not started. */
void estimator_naive_ewma_stop(void);

/* Returns true if the monitor is running. */
bool estimator_naive_ewma_is_running(void);

/* Get the current target bitrate (from monitor cache). Returns BITRATE_INIT_BPS if not running. */
uint64_t estimator_naive_ewma_get_target_bitrate(void);

/* Enable logging to file. Call before estimator_naive_ewma_start(). */
void estimator_naive_ewma_set_log_path(const char *path);

/* Fill estimator output from naive-ewma monitor target bitrate. */
int estimator_naive_ewma_estimate(const struct rate_estimator_input *in,
                                  struct rate_estimator_output *out);

#endif
