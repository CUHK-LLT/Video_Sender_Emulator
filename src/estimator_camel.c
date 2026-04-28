#include "estimator_camel.h"

#include "proto.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define EWMA_ALPHA_DEN 8u
#define GAMMA_MIN 0.20
#define GAMMA_DECAY 0.95
#define SLOPE_THRESH_US_PER_BYTE 0.0015
#define MIN_FRAME_SPAN_US 500ull
#define MIN_VALID_DELAY_US 100ull
#define MIN_BURST_PACKETS 2u

struct frame_accum {
    bool active;
    uint64_t frame_id;
    uint32_t frame_packet_count;
    uint32_t packet_samples;
    uint64_t bytes_total;
    uint32_t first_pkt_len;
    uint64_t first_recv_ts_us;
    uint64_t last_recv_ts_us;
    uint64_t first_send_ts_us;
    uint64_t first_local_feedback_ts_us;
};

static struct {
    bool started;
    FILE *log_file;
    uint64_t avg_bandwidth_bps;
    uint64_t min_delay_us;
    uint64_t bdp_bytes;
    uint64_t target_bitrate_bps;
    double gamma;
    bool have_last_gradient_sample;
    uint64_t last_delay_us;
    uint64_t last_inflight_bytes;
    struct frame_accum frame;
} g_state;

static char g_log_path[512];

static void finalize_frame_sample(const struct rate_estimator_input *in,
                                  bool *updated,
                                  bool *congested,
                                  uint32_t *burst_budget_packets)
{
    if (!g_state.frame.active || g_state.frame.packet_samples < 2) {
        return;
    }
    if (g_state.frame.last_recv_ts_us <= g_state.frame.first_recv_ts_us) {
        return;
    }

    uint64_t span_us = g_state.frame.last_recv_ts_us - g_state.frame.first_recv_ts_us;
    if (span_us < MIN_FRAME_SPAN_US) {
        return;
    }
    if (g_state.frame.bytes_total <= g_state.frame.first_pkt_len) {
        return;
    }

    uint64_t payload_wo_first = g_state.frame.bytes_total - g_state.frame.first_pkt_len;
    uint64_t frame_bw_bps = (payload_wo_first * 8ull * 1000000ull) / span_us;
    if (frame_bw_bps == 0) {
        return;
    }

    if (g_state.avg_bandwidth_bps == 0) {
        g_state.avg_bandwidth_bps = frame_bw_bps;
    } else {
        g_state.avg_bandwidth_bps =
            ((EWMA_ALPHA_DEN - 1u) * g_state.avg_bandwidth_bps + frame_bw_bps) / EWMA_ALPHA_DEN;
    }

    uint64_t delay_us = 0;
    if (g_state.frame.first_local_feedback_ts_us > g_state.frame.first_send_ts_us) {
        delay_us = g_state.frame.first_local_feedback_ts_us - g_state.frame.first_send_ts_us;
    }

    if (delay_us >= MIN_VALID_DELAY_US) {
        if (g_state.min_delay_us == 0 || delay_us < g_state.min_delay_us) {
            g_state.min_delay_us = delay_us;
        }

        bool over_thresh = false;
        if (g_state.have_last_gradient_sample && in->inflight_bytes > g_state.last_inflight_bytes) {
            uint64_t inflight_delta = in->inflight_bytes - g_state.last_inflight_bytes;
            uint64_t delay_delta = delay_us > g_state.last_delay_us ? (delay_us - g_state.last_delay_us) : 0ull;
            double slope = (double)delay_delta / (double)inflight_delta;
            over_thresh = slope > SLOPE_THRESH_US_PER_BYTE;
        }
        g_state.last_delay_us = delay_us;
        g_state.last_inflight_bytes = in->inflight_bytes;
        g_state.have_last_gradient_sample = true;

        if (over_thresh || in->loss_events_delta > 0u) {
            g_state.gamma *= GAMMA_DECAY;
            if (g_state.gamma < GAMMA_MIN) {
                g_state.gamma = GAMMA_MIN;
            }
            *congested = true;
        } else {
            g_state.gamma = 1.0;
        }
    }

    if (g_state.min_delay_us > 0 && g_state.avg_bandwidth_bps > 0) {
        g_state.bdp_bytes = (g_state.avg_bandwidth_bps * g_state.min_delay_us) / (8ull * 1000000ull);
    }

    if (g_state.avg_bandwidth_bps > 0) {
        double target = (double)g_state.avg_bandwidth_bps * g_state.gamma;
        if (target < (double)BITRATE_MIN_BPS) {
            target = (double)BITRATE_MIN_BPS;
        }
        g_state.target_bitrate_bps = (uint64_t)target;
    }

    if (g_state.bdp_bytes > 0 && in->frame_interval_ns > 0 && in->init_bitrate_bps > 0) {
        uint64_t bits_per_frame = (in->init_bitrate_bps * in->frame_interval_ns) / NS_PER_SEC;
        uint64_t bytes_per_frame = bits_per_frame / 8ull;
        if (bytes_per_frame > 0) {
            uint64_t est_pkts = g_state.bdp_bytes / (uint64_t)g_state.frame.first_pkt_len;
            uint64_t frame_pkts = bytes_per_frame / (uint64_t)g_state.frame.first_pkt_len;
            uint64_t budget = est_pkts < frame_pkts ? est_pkts : frame_pkts;
            if (budget >= MIN_BURST_PACKETS) {
                if (budget > UINT32_MAX) {
                    budget = UINT32_MAX;
                }
                *burst_budget_packets = (uint32_t)budget;
            }
        }
    }

    *updated = true;
}

int estimator_camel_start(void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.gamma = 1.0;
    g_state.target_bitrate_bps = BITRATE_INIT_BPS;

    if (g_log_path[0] != '\0') {
        g_state.log_file = fopen(g_log_path, "w");
        if (g_state.log_file) {
            fprintf(g_state.log_file,
                    "frame_id,avg_bw_bps,delay_us,min_delay_us,gamma,bdp_bytes,target_bps,inflight_bytes,loss_delta,congested,feedback_count,burst_budget_packets\n");
            fflush(g_state.log_file);
        }
    }
    g_state.started = true;
    return 0;
}

void estimator_camel_stop(void)
{
    if (g_state.log_file) {
        fclose(g_state.log_file);
        g_state.log_file = NULL;
    }
    g_state.started = false;
}

void estimator_camel_set_log_path(const char *path)
{
    if (!path) {
        g_log_path[0] = '\0';
        return;
    }
    size_t n = sizeof(g_log_path) - 1;
    strncpy(g_log_path, path, n);
    g_log_path[n] = '\0';
}

int estimator_camel_estimate(const struct rate_estimator_input *in,
                             struct rate_estimator_output *out)
{
    if (!in || !out) {
        return -1;
    }
    if (!g_state.started) {
        return -1;
    }

    bool updated = false;
    bool congested = false;
    uint32_t burst_budget_packets = 0;
    uint64_t last_delay_for_log = g_state.last_delay_us;

    for (size_t i = 0; i < in->feedback_count; ++i) {
        const struct rate_estimator_packet_feedback *f = &in->feedbacks[i];
        if (f->pkt_len == 0 || f->frame_packet_count == 0) {
            continue;
        }
        if (!g_state.frame.active || g_state.frame.frame_id != f->frame_id) {
            finalize_frame_sample(in, &updated, &congested, &burst_budget_packets);
            memset(&g_state.frame, 0, sizeof(g_state.frame));
            g_state.frame.active = true;
            g_state.frame.frame_id = f->frame_id;
            g_state.frame.frame_packet_count = f->frame_packet_count;
        }

        if (g_state.frame.packet_samples == 0) {
            g_state.frame.first_pkt_len = f->pkt_len;
            g_state.frame.first_recv_ts_us = f->recv_ts_us;
            g_state.frame.first_send_ts_us = f->send_time_us;
            g_state.frame.first_local_feedback_ts_us = f->local_record_ts_us;
        }
        g_state.frame.packet_samples++;
        g_state.frame.bytes_total += (uint64_t)f->pkt_len;
        if (f->recv_ts_us > g_state.frame.last_recv_ts_us) {
            g_state.frame.last_recv_ts_us = f->recv_ts_us;
        }
        if (f->packet_id == 0u) {
            g_state.frame.first_send_ts_us = f->send_time_us;
            g_state.frame.first_local_feedback_ts_us = f->local_record_ts_us;
            g_state.frame.first_recv_ts_us = f->recv_ts_us;
        }

        if (g_state.frame.packet_samples >= g_state.frame.frame_packet_count) {
            finalize_frame_sample(in, &updated, &congested, &burst_budget_packets);
            last_delay_for_log = g_state.last_delay_us;
            memset(&g_state.frame, 0, sizeof(g_state.frame));
        }
    }

    if (g_state.target_bitrate_bps == 0) {
        g_state.target_bitrate_bps = in->init_bitrate_bps > 0 ? in->init_bitrate_bps : BITRATE_INIT_BPS;
    }

    out->target_bitrate_bps = g_state.target_bitrate_bps;
    out->send_this_frame = true;
    out->force_probe_packet = false;
    out->burst_budget_packets = burst_budget_packets;

    if (g_state.log_file) {
        fprintf(g_state.log_file,
                "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu32 ",%u,%zu,%" PRIu32 "\n",
                in->frame_id,
                g_state.avg_bandwidth_bps,
                last_delay_for_log,
                g_state.min_delay_us,
                g_state.gamma,
                g_state.bdp_bytes,
                g_state.target_bitrate_bps,
                in->inflight_bytes,
                in->loss_events_delta,
                congested ? 1u : 0u,
                in->feedback_count,
                out->burst_budget_packets);
        fflush(g_state.log_file);
    }
    return 0;
}
