#include "estimator_gcc_REMB.h"

#include "proto.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BURST_DELTA_THRESHOLD_MS 5ll
#define TIMESTAMP_GROUP_LENGTH_MS 5ull

#define MAX_ADAPT_OFFSET_MS 15.0
#define MIN_NUM_DELTAS 60u
#define DELTA_COUNTER_MAX 1000u
#define MIN_FRAME_PERIOD_HISTORY_LENGTH 60u

#define RATE_COUNTER_WINDOW_MS 1000u
#define RATE_COUNTER_SCALE 8000ull

enum bandwidth_usage {
    BW_NORMAL = 0,
    BW_UNDERUSING = 1,
    BW_OVERUSING = 2,
};

enum rate_control_state {
    RATE_HOLD = 0,
    RATE_INCREASE = 1,
    RATE_DECREASE = 2,
};

struct rate_bucket {
    uint32_t count;
    uint64_t value;
};

struct rate_counter {
    struct rate_bucket buckets[RATE_COUNTER_WINDOW_MS];
    struct rate_bucket total;
    uint32_t origin_index;
    uint64_t origin_ms;
    bool origin_valid;
};

struct timestamp_group {
    bool valid;
    uint64_t first_timestamp_ms;
    uint64_t last_timestamp_ms;
    uint64_t arrival_time_ms;
    int64_t size;
};

struct inter_arrival_delta {
    uint64_t timestamp_delta_ms;
    int64_t arrival_time_delta_ms;
    int64_t size_delta;
};

struct inter_arrival {
    struct timestamp_group current_group;
    struct timestamp_group previous_group;
    bool has_current_group;
    bool has_previous_group;
};

struct overuse_estimator {
    double e[2][2];
    uint32_t num_of_deltas;
    double offset;
    double previous_offset;
    double slope;
    double avg_noise;
    double var_noise;
    double process_noise[2];
    double ts_delta_hist[MIN_FRAME_PERIOD_HISTORY_LENGTH];
    uint32_t ts_hist_len;
};

struct overuse_detector {
    enum bandwidth_usage hypothesis;
    bool has_last_update_ms;
    uint64_t last_update_ms;
    double k_up;
    double k_down;
    uint32_t overuse_counter;
    bool has_overuse_time;
    double overuse_time;
    double previous_offset;
    double threshold;
    double overuse_time_threshold;
};

struct aimd_rate_control {
    bool has_avg_max_bitrate_kbps;
    double avg_max_bitrate_kbps;
    double var_max_bitrate_kbps;
    uint64_t current_bitrate;
    bool current_bitrate_initialized;
    bool has_first_estimated_throughput_time;
    uint64_t first_estimated_throughput_time;
    bool has_last_change_ms;
    uint64_t last_change_ms;
    bool near_max;
    uint64_t latest_estimated_throughput;
    uint32_t rtt_ms;
    enum rate_control_state state;
};

struct remote_bitrate_estimator {
    struct rate_counter incoming_bitrate;
    bool incoming_bitrate_initialized;
    struct inter_arrival inter_arrival;
    struct overuse_estimator estimator;
    struct overuse_detector detector;
    struct aimd_rate_control rate_control;
    bool has_last_update_ms;
    uint64_t last_update_ms;
};

static struct {
    bool started;
    uint64_t target_bitrate_bps;
    struct remote_bitrate_estimator rbe;
    FILE *log_file;
    char log_path[512];
    uint64_t last_acked_bitrate_bps;
} g_state;

static const char *usage_to_str(enum bandwidth_usage usage)
{
    switch (usage) {
    case BW_OVERUSING:
        return "overuse";
    case BW_UNDERUSING:
        return "underuse";
    default:
        return "normal";
    }
}

static void rate_counter_reset(struct rate_counter *rc)
{
    memset(rc, 0, sizeof(*rc));
}

static void rate_counter_erase_old(struct rate_counter *rc, uint64_t now_ms)
{
    if (!rc->origin_valid) {
        return;
    }

    uint64_t new_origin_ms = now_ms >= (RATE_COUNTER_WINDOW_MS - 1u)
                                 ? now_ms - (RATE_COUNTER_WINDOW_MS - 1u)
                                 : 0u;
    while (rc->origin_ms < new_origin_ms) {
        struct rate_bucket *bucket = &rc->buckets[rc->origin_index];
        if (rc->total.count >= bucket->count) {
            rc->total.count -= bucket->count;
        } else {
            rc->total.count = 0;
        }
        if (rc->total.value >= bucket->value) {
            rc->total.value -= bucket->value;
        } else {
            rc->total.value = 0;
        }
        bucket->count = 0;
        bucket->value = 0;
        rc->origin_index = (rc->origin_index + 1u) % RATE_COUNTER_WINDOW_MS;
        rc->origin_ms++;
    }
}

static void rate_counter_add(struct rate_counter *rc, uint32_t value, uint64_t now_ms)
{
    if (!rc->origin_valid) {
        rc->origin_valid = true;
        rc->origin_ms = now_ms;
        rc->origin_index = 0u;
    } else {
        rate_counter_erase_old(rc, now_ms);
    }

    uint32_t index = (rc->origin_index + (uint32_t)(now_ms - rc->origin_ms)) % RATE_COUNTER_WINDOW_MS;
    rc->buckets[index].count++;
    rc->buckets[index].value += value;
    rc->total.count++;
    rc->total.value += value;
}

static bool rate_counter_rate(struct rate_counter *rc, uint64_t now_ms, uint64_t *rate_bps_out)
{
    if (!rc->origin_valid) {
        return false;
    }
    rate_counter_erase_old(rc, now_ms);
    uint64_t active_window = now_ms - rc->origin_ms + 1u;
    if (rc->total.count > 0u && active_window > 1u) {
        if (rate_bps_out) {
            *rate_bps_out = (RATE_COUNTER_SCALE * rc->total.value + (active_window / 2u)) / active_window;
        }
        return true;
    }
    return false;
}

static void inter_arrival_reset(struct inter_arrival *ia)
{
    memset(ia, 0, sizeof(*ia));
}

static bool inter_arrival_packet_out_of_order(const struct inter_arrival *ia, uint64_t timestamp_ms)
{
    return ia->has_current_group && timestamp_ms < ia->current_group.first_timestamp_ms;
}

static bool inter_arrival_belongs_to_burst(const struct inter_arrival *ia,
                                           uint64_t timestamp_ms,
                                           uint64_t arrival_time_ms)
{
    uint64_t timestamp_delta_ms = timestamp_ms - ia->current_group.last_timestamp_ms;
    int64_t arrival_delta_ms = (int64_t)arrival_time_ms - (int64_t)ia->current_group.arrival_time_ms;
    return timestamp_delta_ms == 0u ||
           (((arrival_delta_ms - (int64_t)timestamp_delta_ms) < 0) &&
            arrival_delta_ms <= BURST_DELTA_THRESHOLD_MS);
}

static bool inter_arrival_new_timestamp_group(const struct inter_arrival *ia,
                                              uint64_t timestamp_ms,
                                              uint64_t arrival_time_ms)
{
    if (inter_arrival_belongs_to_burst(ia, timestamp_ms, arrival_time_ms)) {
        return false;
    }
    return (timestamp_ms - ia->current_group.first_timestamp_ms) > TIMESTAMP_GROUP_LENGTH_MS;
}

static bool inter_arrival_compute_deltas(struct inter_arrival *ia,
                                         uint64_t timestamp_ms,
                                         uint64_t arrival_time_ms,
                                         uint32_t packet_size,
                                         struct inter_arrival_delta *out)
{
    bool has_delta = false;
    if (!ia->has_current_group) {
        memset(&ia->current_group, 0, sizeof(ia->current_group));
        ia->has_current_group = true;
        ia->current_group.valid = true;
        ia->current_group.first_timestamp_ms = timestamp_ms;
        ia->current_group.last_timestamp_ms = timestamp_ms;
    } else if (inter_arrival_packet_out_of_order(ia, timestamp_ms)) {
        return false;
    } else if (inter_arrival_new_timestamp_group(ia, timestamp_ms, arrival_time_ms)) {
        if (ia->has_previous_group) {
            out->timestamp_delta_ms =
                ia->current_group.last_timestamp_ms - ia->previous_group.last_timestamp_ms;
            out->arrival_time_delta_ms =
                (int64_t)ia->current_group.arrival_time_ms - (int64_t)ia->previous_group.arrival_time_ms;
            out->size_delta = ia->current_group.size - ia->previous_group.size;
            has_delta = true;
        }
        ia->previous_group = ia->current_group;
        ia->has_previous_group = true;
        memset(&ia->current_group, 0, sizeof(ia->current_group));
        ia->current_group.valid = true;
        ia->current_group.first_timestamp_ms = timestamp_ms;
        ia->current_group.last_timestamp_ms = timestamp_ms;
    } else if (timestamp_ms > ia->current_group.last_timestamp_ms) {
        ia->current_group.last_timestamp_ms = timestamp_ms;
    }

    ia->current_group.size += packet_size;
    ia->current_group.arrival_time_ms = arrival_time_ms;
    return has_delta;
}

static void overuse_estimator_reset(struct overuse_estimator *oe)
{
    memset(oe, 0, sizeof(*oe));
    oe->e[0][0] = 100.0;
    oe->e[1][1] = 0.1;
    oe->slope = 1.0 / 64.0;
    oe->var_noise = 50.0;
    oe->process_noise[0] = 1e-13;
    oe->process_noise[1] = 1e-3;
}

static double overuse_estimator_update_min_frame_period(struct overuse_estimator *oe, double ts_delta_ms)
{
    double min_frame_period = ts_delta_ms;
    if (oe->ts_hist_len >= MIN_FRAME_PERIOD_HISTORY_LENGTH) {
        memmove(&oe->ts_delta_hist[0],
                &oe->ts_delta_hist[1],
                sizeof(oe->ts_delta_hist[0]) * (MIN_FRAME_PERIOD_HISTORY_LENGTH - 1u));
        oe->ts_hist_len = MIN_FRAME_PERIOD_HISTORY_LENGTH - 1u;
    }
    for (uint32_t i = 0; i < oe->ts_hist_len; ++i) {
        if (oe->ts_delta_hist[i] < min_frame_period) {
            min_frame_period = oe->ts_delta_hist[i];
        }
    }
    oe->ts_delta_hist[oe->ts_hist_len++] = ts_delta_ms;
    return min_frame_period;
}

static void overuse_estimator_update_noise_estimate(struct overuse_estimator *oe,
                                                    double residual,
                                                    double ts_delta_ms)
{
    double alpha = oe->num_of_deltas > 300u ? 0.002 : 0.01;
    double beta = pow(1.0 - alpha, ts_delta_ms * 30.0 / 1000.0);
    oe->avg_noise = beta * oe->avg_noise + (1.0 - beta) * residual;
    oe->var_noise = beta * oe->var_noise + (1.0 - beta) * (oe->avg_noise - residual) * (oe->avg_noise - residual);
    if (oe->var_noise < 1.0) {
        oe->var_noise = 1.0;
    }
}

static void overuse_estimator_update(struct overuse_estimator *oe,
                                     int64_t time_delta_ms,
                                     double timestamp_delta_ms,
                                     int64_t size_delta,
                                     enum bandwidth_usage current_hypothesis)
{
    double min_frame_period = overuse_estimator_update_min_frame_period(oe, timestamp_delta_ms);
    double t_ts_delta = (double)time_delta_ms - timestamp_delta_ms;
    double fs_delta = (double)size_delta;

    if (oe->num_of_deltas < DELTA_COUNTER_MAX) {
        oe->num_of_deltas++;
    }

    oe->e[0][0] += oe->process_noise[0];
    oe->e[1][1] += oe->process_noise[1];
    if ((current_hypothesis == BW_OVERUSING && oe->offset < oe->previous_offset) ||
        (current_hypothesis == BW_UNDERUSING && oe->offset > oe->previous_offset)) {
        oe->e[1][1] += 10.0 * oe->process_noise[1];
    }

    double h0 = fs_delta;
    double h1 = 1.0;
    double eh0 = oe->e[0][0] * h0 + oe->e[0][1] * h1;
    double eh1 = oe->e[1][0] * h0 + oe->e[1][1] * h1;

    double residual = t_ts_delta - oe->slope * h0 - oe->offset;
    if (current_hypothesis == BW_NORMAL) {
        double max_residual = 3.0 * sqrt(oe->var_noise);
        if (fabs(residual) < max_residual) {
            overuse_estimator_update_noise_estimate(oe, residual, min_frame_period);
        } else {
            overuse_estimator_update_noise_estimate(oe, residual < 0.0 ? -max_residual : max_residual, min_frame_period);
        }
    }

    double denom = oe->var_noise + h0 * eh0 + h1 * eh1;
    if (denom <= 0.0) {
        return;
    }
    double k0 = eh0 / denom;
    double k1 = eh1 / denom;

    double ikh00 = 1.0 - k0 * h0;
    double ikh01 = -k0 * h1;
    double ikh10 = -k1 * h0;
    double ikh11 = 1.0 - k1 * h1;

    double e00 = oe->e[0][0];
    double e01 = oe->e[0][1];

    oe->e[0][0] = e00 * ikh00 + oe->e[1][0] * ikh01;
    oe->e[0][1] = e01 * ikh00 + oe->e[1][1] * ikh01;
    oe->e[1][0] = e00 * ikh10 + oe->e[1][0] * ikh11;
    oe->e[1][1] = e01 * ikh10 + oe->e[1][1] * ikh11;

    oe->previous_offset = oe->offset;
    oe->slope += k0 * residual;
    oe->offset += k1 * residual;
}

static void overuse_detector_reset(struct overuse_detector *od)
{
    memset(od, 0, sizeof(*od));
    od->hypothesis = BW_NORMAL;
    od->k_up = 0.0087;
    od->k_down = 0.039;
    od->overuse_time_threshold = 10.0;
    od->threshold = 12.5;
}

static void overuse_detector_update_threshold(struct overuse_detector *od,
                                              double modified_offset,
                                              uint64_t now_ms)
{
    if (!od->has_last_update_ms) {
        od->last_update_ms = now_ms;
        od->has_last_update_ms = true;
    }

    if (fabs(modified_offset) > od->threshold + MAX_ADAPT_OFFSET_MS) {
        od->last_update_ms = now_ms;
        return;
    }

    double k = fabs(modified_offset) < od->threshold ? od->k_down : od->k_up;
    uint64_t delta_ms = now_ms >= od->last_update_ms ? now_ms - od->last_update_ms : 0u;
    if (delta_ms > 100u) {
        delta_ms = 100u;
    }
    od->threshold += k * (fabs(modified_offset) - od->threshold) * (double)delta_ms;
    if (od->threshold < 6.0) {
        od->threshold = 6.0;
    } else if (od->threshold > 600.0) {
        od->threshold = 600.0;
    }
    od->last_update_ms = now_ms;
}

static enum bandwidth_usage overuse_detector_detect(struct overuse_detector *od,
                                                    double offset,
                                                    double timestamp_delta_ms,
                                                    uint32_t num_of_deltas,
                                                    uint64_t now_ms)
{
    if (num_of_deltas < 2u) {
        od->hypothesis = BW_NORMAL;
        return od->hypothesis;
    }

    double t = (double)(num_of_deltas < MIN_NUM_DELTAS ? num_of_deltas : MIN_NUM_DELTAS) * offset;
    if (t > od->threshold) {
        if (!od->has_overuse_time) {
            od->overuse_time = timestamp_delta_ms / 2.0;
            od->has_overuse_time = true;
        } else {
            od->overuse_time += timestamp_delta_ms;
        }
        od->overuse_counter++;
        if (od->overuse_time > od->overuse_time_threshold && od->overuse_counter > 1u &&
            offset >= od->previous_offset) {
            od->overuse_counter = 0u;
            od->overuse_time = 0.0;
            od->hypothesis = BW_OVERUSING;
        }
    } else if (t < -od->threshold) {
        od->overuse_counter = 0u;
        od->has_overuse_time = false;
        od->hypothesis = BW_UNDERUSING;
    } else {
        od->overuse_counter = 0u;
        od->has_overuse_time = false;
        od->hypothesis = BW_NORMAL;
    }

    od->previous_offset = offset;
    overuse_detector_update_threshold(od, t, now_ms);
    return od->hypothesis;
}

static void aimd_rate_control_reset(struct aimd_rate_control *rc)
{
    memset(rc, 0, sizeof(*rc));
    rc->var_max_bitrate_kbps = 0.4;
    rc->current_bitrate = BITRATE_INIT_BPS;
    rc->latest_estimated_throughput = BITRATE_INIT_BPS;
    rc->rtt_ms = 200u;
    rc->state = RATE_HOLD;
}

static uint64_t aimd_feedback_interval_ms(void)
{
    return 500u;
}

static void aimd_update_max_throughput_estimate(struct aimd_rate_control *rc,
                                                double estimated_throughput_kbps)
{
    const double alpha = 0.05;
    if (!rc->has_avg_max_bitrate_kbps) {
        rc->avg_max_bitrate_kbps = estimated_throughput_kbps;
        rc->has_avg_max_bitrate_kbps = true;
    } else {
        rc->avg_max_bitrate_kbps =
            (1.0 - alpha) * rc->avg_max_bitrate_kbps + alpha * estimated_throughput_kbps;
    }
    double norm = rc->avg_max_bitrate_kbps > 1.0 ? rc->avg_max_bitrate_kbps : 1.0;
    rc->var_max_bitrate_kbps =
        (1.0 - alpha) * rc->var_max_bitrate_kbps +
        alpha * ((rc->avg_max_bitrate_kbps - estimated_throughput_kbps) *
                 (rc->avg_max_bitrate_kbps - estimated_throughput_kbps)) /
            norm;
    if (rc->var_max_bitrate_kbps < 0.4) {
        rc->var_max_bitrate_kbps = 0.4;
    } else if (rc->var_max_bitrate_kbps > 2.5) {
        rc->var_max_bitrate_kbps = 2.5;
    }
}

static uint64_t aimd_near_max_rate_increase(const struct aimd_rate_control *rc)
{
    double bits_per_frame = (double)rc->current_bitrate / 30.0;
    double packets_per_frame = ceil(bits_per_frame / (8.0 * 1200.0));
    if (packets_per_frame < 1.0) {
        packets_per_frame = 1.0;
    }
    double avg_packet_size_bits = bits_per_frame / packets_per_frame;
    uint32_t response_time = rc->rtt_ms + 100u;
    uint64_t inc = (uint64_t)((avg_packet_size_bits * 1000.0) / (double)response_time);
    return inc < 4000u ? 4000u : inc;
}

static uint64_t aimd_additive_rate_increase(const struct aimd_rate_control *rc,
                                            uint64_t last_ms,
                                            uint64_t now_ms)
{
    return ((now_ms - last_ms) * aimd_near_max_rate_increase(rc)) / 1000u;
}

static uint64_t aimd_multiplicative_rate_increase(uint64_t current_bitrate,
                                                  uint64_t last_ms,
                                                  uint64_t now_ms,
                                                  bool has_last_ms)
{
    double alpha = 1.08;
    if (has_last_ms && now_ms >= last_ms) {
        uint64_t elapsed_ms = now_ms - last_ms;
        if (elapsed_ms > 1000u) {
            elapsed_ms = 1000u;
        }
        alpha = pow(alpha, (double)elapsed_ms / 1000.0);
    }
    double inc = (alpha - 1.0) * (double)current_bitrate;
    if (inc < 1000.0) {
        inc = 1000.0;
    }
    return (uint64_t)inc;
}

static uint64_t aimd_clamp_bitrate(const struct aimd_rate_control *rc,
                                   uint64_t new_bitrate,
                                   uint64_t estimated_throughput)
{
    uint64_t max_bitrate = (uint64_t)(1.5 * (double)estimated_throughput) + 10000u;
    if (max_bitrate < rc->current_bitrate) {
        max_bitrate = rc->current_bitrate;
    }
    return new_bitrate < max_bitrate ? new_bitrate : max_bitrate;
}

static bool aimd_rate_control_update(struct aimd_rate_control *rc,
                                     enum bandwidth_usage usage,
                                     bool has_estimated_throughput,
                                     uint64_t estimated_throughput,
                                     uint64_t now_ms,
                                     uint64_t *out_bitrate)
{
    if (!rc->current_bitrate_initialized && has_estimated_throughput) {
        if (!rc->has_first_estimated_throughput_time) {
            rc->first_estimated_throughput_time = now_ms;
            rc->has_first_estimated_throughput_time = true;
        } else if (now_ms > rc->first_estimated_throughput_time + 3000u) {
            rc->current_bitrate = estimated_throughput;
            rc->current_bitrate_initialized = true;
        }
    }

    if (!rc->current_bitrate_initialized && usage != BW_OVERUSING) {
        return false;
    }

    if (usage == BW_NORMAL && rc->state == RATE_HOLD) {
        rc->last_change_ms = now_ms;
        rc->has_last_change_ms = true;
        rc->state = RATE_INCREASE;
    } else if (usage == BW_OVERUSING) {
        rc->state = RATE_DECREASE;
    } else if (usage == BW_UNDERUSING) {
        rc->state = RATE_HOLD;
    }

    uint64_t new_bitrate = rc->current_bitrate;
    uint64_t throughput = has_estimated_throughput ? estimated_throughput : rc->latest_estimated_throughput;
    if (has_estimated_throughput) {
        rc->latest_estimated_throughput = estimated_throughput;
    }
    double throughput_kbps = (double)throughput / 1000.0;

    if (rc->state == RATE_INCREASE) {
        if (rc->has_avg_max_bitrate_kbps) {
            double sigma_kbps = sqrt(rc->var_max_bitrate_kbps * rc->avg_max_bitrate_kbps);
            if (throughput_kbps >= rc->avg_max_bitrate_kbps + 3.0 * sigma_kbps) {
                rc->near_max = false;
                rc->has_avg_max_bitrate_kbps = false;
            }
        }
        if (rc->near_max) {
            if (!rc->has_last_change_ms) {
                rc->last_change_ms = now_ms;
                rc->has_last_change_ms = true;
            }
            new_bitrate += aimd_additive_rate_increase(rc, rc->last_change_ms, now_ms);
        } else {
            new_bitrate += aimd_multiplicative_rate_increase(new_bitrate,
                                                             rc->last_change_ms,
                                                             now_ms,
                                                             rc->has_last_change_ms);
        }
        rc->last_change_ms = now_ms;
        rc->has_last_change_ms = true;
    } else if (rc->state == RATE_DECREASE) {
        if (rc->has_avg_max_bitrate_kbps) {
            double sigma_kbps = sqrt(rc->var_max_bitrate_kbps * rc->avg_max_bitrate_kbps);
            if (throughput_kbps < rc->avg_max_bitrate_kbps - 3.0 * sigma_kbps) {
                rc->has_avg_max_bitrate_kbps = false;
            }
        }
        aimd_update_max_throughput_estimate(rc, throughput_kbps);
        rc->near_max = true;
        new_bitrate = (uint64_t)llround(0.85 * (double)throughput);
        rc->last_change_ms = now_ms;
        rc->has_last_change_ms = true;
        rc->state = RATE_HOLD;
    }

    rc->current_bitrate = aimd_clamp_bitrate(rc, new_bitrate, throughput);
    if (out_bitrate) {
        *out_bitrate = rc->current_bitrate;
    }
    return true;
}

static void remote_bitrate_estimator_reset(struct remote_bitrate_estimator *rbe)
{
    memset(rbe, 0, sizeof(*rbe));
    rate_counter_reset(&rbe->incoming_bitrate);
    rbe->incoming_bitrate_initialized = true;
    inter_arrival_reset(&rbe->inter_arrival);
    overuse_estimator_reset(&rbe->estimator);
    overuse_detector_reset(&rbe->detector);
    aimd_rate_control_reset(&rbe->rate_control);
}

static bool remote_bitrate_estimator_add(struct remote_bitrate_estimator *rbe,
                                         uint64_t arrival_time_ms,
                                         uint64_t send_time_ms,
                                         uint32_t payload_size,
                                         uint64_t *target_bitrate_bps_out,
                                         uint64_t *acked_bitrate_bps_out)
{
    struct inter_arrival_delta deltas;
    bool update_estimate = false;
    uint64_t acked_bitrate = 0;
    bool has_acked_bitrate = rate_counter_rate(&rbe->incoming_bitrate, arrival_time_ms, &acked_bitrate);
    if (has_acked_bitrate) {
        rbe->incoming_bitrate_initialized = true;
    } else if (rbe->incoming_bitrate_initialized) {
        rate_counter_reset(&rbe->incoming_bitrate);
        rbe->incoming_bitrate_initialized = false;
    }
    rate_counter_add(&rbe->incoming_bitrate, payload_size, arrival_time_ms);
    has_acked_bitrate = rate_counter_rate(&rbe->incoming_bitrate, arrival_time_ms, &acked_bitrate);
    if (acked_bitrate_bps_out) {
        *acked_bitrate_bps_out = has_acked_bitrate ? acked_bitrate : 0u;
    }

    if (inter_arrival_compute_deltas(&rbe->inter_arrival,
                                     send_time_ms,
                                     arrival_time_ms,
                                     payload_size,
                                     &deltas)) {
        overuse_estimator_update(&rbe->estimator,
                                 deltas.arrival_time_delta_ms,
                                 (double)deltas.timestamp_delta_ms,
                                 deltas.size_delta,
                                 rbe->detector.hypothesis);
        (void)overuse_detector_detect(&rbe->detector,
                                      rbe->estimator.offset,
                                      (double)deltas.timestamp_delta_ms,
                                      rbe->estimator.num_of_deltas,
                                      arrival_time_ms);
    }

    if (!rbe->has_last_update_ms ||
        (arrival_time_ms - rbe->last_update_ms) > aimd_feedback_interval_ms()) {
        update_estimate = true;
    } else if (rbe->detector.hypothesis == BW_OVERUSING) {
        update_estimate = true;
    }

    if (!update_estimate) {
        return false;
    }

    uint64_t target_bps = 0;
    if (aimd_rate_control_update(&rbe->rate_control,
                                 rbe->detector.hypothesis,
                                 has_acked_bitrate,
                                 acked_bitrate,
                                 arrival_time_ms,
                                 &target_bps)) {
        rbe->last_update_ms = arrival_time_ms;
        rbe->has_last_update_ms = true;
        if (target_bitrate_bps_out) {
            *target_bitrate_bps_out = target_bps;
        }
        return true;
    }
    return false;
}

void estimator_gcc_REMB_set_log_path(const char *path)
{
    if (!path) {
        g_state.log_path[0] = '\0';
        return;
    }
    size_t n = sizeof(g_state.log_path) - 1u;
    strncpy(g_state.log_path, path, n);
    g_state.log_path[n] = '\0';
}

int estimator_gcc_REMB_start(void)
{
    remote_bitrate_estimator_reset(&g_state.rbe);
    g_state.target_bitrate_bps = BITRATE_INIT_BPS;
    g_state.last_acked_bitrate_bps = 0u;
    g_state.started = true;
    if (g_state.log_file) {
        fclose(g_state.log_file);
        g_state.log_file = NULL;
    }
    if (g_state.log_path[0] != '\0') {
        g_state.log_file = fopen(g_state.log_path, "w");
        if (g_state.log_file) {
            fprintf(g_state.log_file,
                    "frame_id,sample_arrival_ms,sample_send_ms,acked_bitrate_bps,offset,threshold,usage,target_bitrate_bps\n");
            fflush(g_state.log_file);
        }
    }
    return 0;
}

void estimator_gcc_REMB_stop(void)
{
    if (g_state.log_file) {
        fclose(g_state.log_file);
        g_state.log_file = NULL;
    }
    g_state.started = false;
}

int estimator_gcc_REMB_estimate(const struct rate_estimator_input *in,
                                struct rate_estimator_output *out)
{
    if (!in || !out) {
        return -1;
    }
    if (!g_state.started) {
        return -1;
    }

    uint64_t sample_arrival_ms = 0u;
    uint64_t sample_send_ms = 0u;
    for (size_t i = 0; i < in->feedback_count; ++i) {
        const struct rate_estimator_packet_feedback *f = &in->feedbacks[i];
        if (f->pkt_len == 0u || f->recv_ts_us == 0u || f->send_time_us == 0u) {
            continue;
        }
        sample_arrival_ms = f->recv_ts_us / 1000u;
        sample_send_ms = f->send_time_us / 1000u;
        uint64_t maybe_target_bps = 0u;
        uint64_t acked_bitrate = 0u;
        if (remote_bitrate_estimator_add(&g_state.rbe,
                                         sample_arrival_ms,
                                         sample_send_ms,
                                         f->pkt_len,
                                         &maybe_target_bps,
                                         &acked_bitrate)) {
            g_state.target_bitrate_bps = maybe_target_bps;
        }
        if (acked_bitrate > 0u) {
            g_state.last_acked_bitrate_bps = acked_bitrate;
        }
    }

    if (g_state.target_bitrate_bps == 0u) {
        g_state.target_bitrate_bps = in->init_bitrate_bps > 0u ? in->init_bitrate_bps : BITRATE_INIT_BPS;
    }

    out->target_bitrate_bps = g_state.target_bitrate_bps;
    out->send_this_frame = true;
    out->force_probe_packet = false;
    out->burst_budget_packets = 0u;

    if (g_state.log_file) {
        fprintf(g_state.log_file,
                "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f,%.6f,%s,%" PRIu64 "\n",
                in->frame_id,
                sample_arrival_ms,
                sample_send_ms,
                g_state.last_acked_bitrate_bps,
                g_state.rbe.estimator.offset,
                g_state.rbe.detector.threshold,
                usage_to_str(g_state.rbe.detector.hypothesis),
                out->target_bitrate_bps);
        fflush(g_state.log_file);
    }

    if (rate_estimator_debug_enabled() && ((in->frame_id % 30u) == 0u)) {
        fprintf(stderr,
                "[GCC_REMB] frame=%" PRIu64 " target=%" PRIu64 " acked=%" PRIu64
                " usage=%s offset=%.4f threshold=%.4f feedback=%zu\n",
                in->frame_id,
                out->target_bitrate_bps,
                g_state.last_acked_bitrate_bps,
                usage_to_str(g_state.rbe.detector.hypothesis),
                g_state.rbe.estimator.offset,
                g_state.rbe.detector.threshold,
                in->feedback_count);
    }
    return 0;
}
