#ifndef RATE_ESTIMATOR_H
#define RATE_ESTIMATOR_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

enum rate_estimator_algo {
    RATE_ESTIMATOR_FIX = 0,
    RATE_ESTIMATOR_NAIVE_EWMA = 1,
    RATE_ESTIMATOR_CAMEL = 2,
    RATE_ESTIMATOR_GCC_REMB = 3,
    RATE_ESTIMATOR_MORTISE = RATE_ESTIMATOR_NAIVE_EWMA, /* backward compatible alias */
};

struct rate_estimator_config {
    enum rate_estimator_algo algo;
    bool debug_enabled;
    bool debug_naive_ewma_monitor;
    const char *log_dir;
};

struct rate_estimator_packet_feedback {
    uint64_t send_seq;
    uint64_t rtcp_batch_id;
    uint64_t frame_id;
    uint32_t packet_id;
    uint32_t frame_packet_count;
    uint32_t pkt_len;
    uint64_t send_time_us;
    uint64_t recv_ts_us;
    uint64_t local_record_ts_us;
};

struct rate_estimator_input {
    uint64_t frame_id;
    uint64_t now_us;
    uint64_t frame_interval_ns;
    uint64_t init_bitrate_bps;
    const struct rate_estimator_packet_feedback *feedbacks;
    size_t feedback_count;
    bool has_latest_sent_seq;
    uint64_t latest_sent_seq;
    uint64_t inflight_bytes;
    uint32_t loss_events_delta;
};

struct rate_estimator_output {
    uint64_t target_bitrate_bps;
    bool send_this_frame;
    bool force_probe_packet;
    uint32_t burst_budget_packets;
};

int rate_estimator_init(const struct rate_estimator_config *cfg);
int rate_estimator_start(void);
int rate_estimator_get_target(const struct rate_estimator_input *in,
                              struct rate_estimator_output *out);
void rate_estimator_stop(void);
void rate_estimator_shutdown(void);
bool rate_estimator_is_running(void);
void rate_estimator_set_debug(bool enabled);
bool rate_estimator_debug_enabled(void);

#endif
