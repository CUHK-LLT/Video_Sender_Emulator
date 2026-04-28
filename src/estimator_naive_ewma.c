#define _POSIX_C_SOURCE 200809L

#include "estimator_naive_ewma.h"

#include "proto.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define EWMA_ALPHA_DEN 8
#define RTCP_DEDUP_SLOTS 16384u
#define US_PER_SEC 1000000ull
#define RTCP_MIN_INTERVAL_US 1000ull

static struct {
    _Atomic uint64_t target_bitrate_bps;
    _Atomic bool started;
    FILE *app_log_file;
    uint64_t rtcp_rate_bps;
    uint64_t rtcp_target_bps;
    uint64_t final_target_bps;
    bool rtcp_window_closed_last;
    uint64_t rtcp_last_batch_id;
    uint64_t rtcp_last_batch_bytes;
    uint64_t rtcp_last_batch_first_seq;
    uint64_t rtcp_last_batch_last_seq;
    uint64_t rtcp_last_batch_first_ts_us;
    uint64_t rtcp_last_batch_last_ts_us;
    bool rtcp_last_batch_skipped;
    bool has_latest_sent_seq;
    uint64_t latest_sent_seq;
    uint64_t rtcp_dedup_seq[RTCP_DEDUP_SLOTS];
    bool rtcp_dedup_valid[RTCP_DEDUP_SLOTS];
    uint32_t last_feedback_count;
} g_state;

static uint64_t realtime_ns_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * NS_PER_SEC + (uint64_t)now.tv_nsec;
}

static char g_app_log_path[512] = {0};

void estimator_naive_ewma_set_log_path(const char *path)
{
    if (path) {
        size_t n = sizeof(g_app_log_path) - 1;
        strncpy(g_app_log_path, path, n);
        g_app_log_path[n] = '\0';
    } else {
        g_app_log_path[0] = '\0';
    }
}

int estimator_naive_ewma_start(void)
{
    if (atomic_load(&g_state.started)) {
        return 0;
    }

    memset(&g_state, 0, sizeof(g_state));
    atomic_store(&g_state.target_bitrate_bps, BITRATE_INIT_BPS);
    if (g_app_log_path[0]) {
        g_state.app_log_file = fopen(g_app_log_path, "w");
        if (g_state.app_log_file) {
            fprintf(g_state.app_log_file,
                    "timestamp_ns,frame_id,rtcp_rate_bps,target_bps,feedback_count,rtcp_last_batch_id,rtcp_last_batch_bytes,rtcp_last_batch_first_seq,rtcp_last_batch_last_seq,rtcp_last_batch_first_ts_us,rtcp_last_batch_last_ts_us,rtcp_last_batch_skipped,latest_sent_seq,rtcp_window_closed\n");
            fflush(g_state.app_log_file);
        }
    }
    atomic_store(&g_state.started, true);
    return 0;
}

void estimator_naive_ewma_stop(void)
{
    if (!atomic_load(&g_state.started)) {
        return;
    }

    if (g_state.app_log_file) {
        fclose(g_state.app_log_file);
        g_state.app_log_file = NULL;
    }
    atomic_store(&g_state.started, false);
}

bool estimator_naive_ewma_is_running(void)
{
    return atomic_load(&g_state.started);
}

uint64_t estimator_naive_ewma_get_target_bitrate(void)
{
    if (!atomic_load(&g_state.started)) {
        return BITRATE_INIT_BPS;
    }
    return atomic_load(&g_state.target_bitrate_bps);
}

int estimator_naive_ewma_estimate(const struct rate_estimator_input *in,
                                  struct rate_estimator_output *out)
{
    if (!in || !out) {
        return -1;
    }

    bool recomputed = false;
    g_state.has_latest_sent_seq = in->has_latest_sent_seq;
    g_state.latest_sent_seq = in->latest_sent_seq;

    g_state.last_feedback_count = (uint32_t)in->feedback_count;
    g_state.rtcp_last_batch_skipped = false;
    for (size_t i = 0; i < in->feedback_count;) {
        uint64_t batch_id = in->feedbacks[i].rtcp_batch_id;
        uint64_t batch_bytes = 0;
        uint64_t first_ts_us = 0;
        uint64_t last_ts_us = 0;
        uint64_t first_seq = 0;
        uint64_t last_seq = 0;
        bool have_sample = false;
        bool batch_skipped = false;

        while (i < in->feedback_count && in->feedbacks[i].rtcp_batch_id == batch_id) {
            const struct rate_estimator_packet_feedback *f = &in->feedbacks[i++];
            uint32_t dedup_idx = (uint32_t)(f->send_seq % RTCP_DEDUP_SLOTS);
            if (g_state.rtcp_dedup_valid[dedup_idx] && g_state.rtcp_dedup_seq[dedup_idx] == f->send_seq) {
                continue;
            }
            g_state.rtcp_dedup_valid[dedup_idx] = true;
            g_state.rtcp_dedup_seq[dedup_idx] = f->send_seq;

            if (f->pkt_len == 0 || f->recv_ts_us == 0) {
                continue;
            }

            if (!have_sample) {
                have_sample = true;
                first_ts_us = f->recv_ts_us;
                last_ts_us = f->recv_ts_us;
                first_seq = f->send_seq;
                last_seq = f->send_seq;
            } else {
                if (f->recv_ts_us < last_ts_us) {
                    batch_skipped = true;
                }
                last_ts_us = f->recv_ts_us;
                last_seq = f->send_seq;
            }
            batch_bytes += f->pkt_len;
        }

        g_state.rtcp_last_batch_id = batch_id;
        g_state.rtcp_last_batch_bytes = batch_bytes;
        g_state.rtcp_last_batch_first_seq = first_seq;
        g_state.rtcp_last_batch_last_seq = last_seq;
        g_state.rtcp_last_batch_first_ts_us = first_ts_us;
        g_state.rtcp_last_batch_last_ts_us = last_ts_us;
        g_state.rtcp_last_batch_skipped = batch_skipped;
        g_state.rtcp_window_closed_last = g_state.has_latest_sent_seq && last_seq > 0 &&
                                          g_state.latest_sent_seq == last_seq;

        if (!have_sample || batch_skipped || last_ts_us <= first_ts_us) {
            g_state.rtcp_last_batch_skipped = true;
            continue;
        }

        uint64_t interval_us = last_ts_us - first_ts_us;
        if (interval_us < RTCP_MIN_INTERVAL_US) {
            g_state.rtcp_last_batch_skipped = true;
            continue;
        }

        uint64_t sample_rate = (batch_bytes * 8ull * US_PER_SEC) / interval_us;
        if (g_state.rtcp_rate_bps == 0) {
            g_state.rtcp_rate_bps = sample_rate;
        } else {
            g_state.rtcp_rate_bps =
                ((EWMA_ALPHA_DEN - 1) * g_state.rtcp_rate_bps + sample_rate) / EWMA_ALPHA_DEN;
        }
        recomputed = true;
    }

    uint64_t bitrate = BITRATE_INIT_BPS;
    g_state.rtcp_target_bps = g_state.rtcp_rate_bps > 0 ? g_state.rtcp_rate_bps / 2 : 0;
    if (g_state.rtcp_rate_bps > 0) {
        bitrate = g_state.rtcp_rate_bps / 2;
    } else {
        bitrate = BITRATE_INIT_BPS;
    }
    g_state.final_target_bps = bitrate;

    if (recomputed || atomic_load(&g_state.target_bitrate_bps) == 0) {
        atomic_store(&g_state.target_bitrate_bps, bitrate);
    } else {
        bitrate = atomic_load(&g_state.target_bitrate_bps);
    }

    if (g_state.app_log_file) {
        fprintf(g_state.app_log_file,
                "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%" PRIu64 ",%u\n",
                realtime_ns_now(),
                in->frame_id,
                g_state.rtcp_rate_bps,
                bitrate,
                in->feedback_count,
                g_state.rtcp_last_batch_id,
                g_state.rtcp_last_batch_bytes,
                g_state.rtcp_last_batch_first_seq,
                g_state.rtcp_last_batch_last_seq,
                g_state.rtcp_last_batch_first_ts_us,
                g_state.rtcp_last_batch_last_ts_us,
                g_state.rtcp_last_batch_skipped ? 1u : 0u,
                g_state.has_latest_sent_seq ? g_state.latest_sent_seq : 0,
                g_state.rtcp_window_closed_last ? 1u : 0u);
        fflush(g_state.app_log_file);
    }

    out->target_bitrate_bps = bitrate;
    out->send_this_frame = true;
    out->force_probe_packet = false;
    out->burst_budget_packets = 0;

    if (rate_estimator_debug_enabled() && ((in->frame_id % 30u) == 0u)) {
        fprintf(stderr,
                "[NAIVE_EWMA] frame=%" PRIu64 " target=%" PRIu64 " rtcp_rate=%" PRIu64 " recomputed=%u feedback=%zu rtcp_closed=%u\n",
                in->frame_id,
                bitrate,
                g_state.rtcp_rate_bps,
                recomputed ? 1u : 0u,
                in->feedback_count,
                g_state.rtcp_window_closed_last ? 1u : 0u);
    }
    return 0;
}
