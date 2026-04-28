#include "rate_estimator.h"

#include "estimator_camel.h"
#include "estimator_fix.h"
#include "estimator_gcc_REMB.h"
#include "estimator_naive_ewma.h"
#include "proto.h"

#include <inttypes.h>
#include <stdio.h>

static bool g_debug_estimator = false;
static bool g_running = false;
static struct rate_estimator_config g_cfg = {
    .algo = RATE_ESTIMATOR_NAIVE_EWMA,
    .debug_enabled = false,
    .debug_naive_ewma_monitor = false,
    .log_dir = ".",
};
static bool g_inited = false;

static void configure_naive_ewma_log_path(void)
{
    if (!g_cfg.debug_naive_ewma_monitor || !g_cfg.log_dir) {
        estimator_naive_ewma_set_log_path(NULL);
        return;
    }

    char app_log_path[512];
    int n = snprintf(app_log_path,
                     sizeof(app_log_path),
                     "%s/estimator_naive_ewma.csv",
                     g_cfg.log_dir);
    if (n <= 0 || (size_t)n >= sizeof(app_log_path)) {
        estimator_naive_ewma_set_log_path(NULL);
        fprintf(stderr, "naive-ewma monitor log path is too long, monitor logging disabled\n");
        return;
    }
    estimator_naive_ewma_set_log_path(app_log_path);
    fprintf(stderr, "naive-ewma app log: %s\n", app_log_path);
}

static void configure_camel_log_path(void)
{
    if (!g_cfg.log_dir) {
        estimator_camel_set_log_path(NULL);
        return;
    }
    char camel_log_path[512];
    int n = snprintf(camel_log_path,
                     sizeof(camel_log_path),
                     "%s/estimator_camel.csv",
                     g_cfg.log_dir);
    if (n <= 0 || (size_t)n >= sizeof(camel_log_path)) {
        estimator_camel_set_log_path(NULL);
        fprintf(stderr, "camel log path is too long, logging disabled\n");
        return;
    }
    estimator_camel_set_log_path(camel_log_path);
}

static void configure_gcc_remb_log_path(void)
{
    if (!g_cfg.log_dir) {
        estimator_gcc_REMB_set_log_path(NULL);
        return;
    }
    char gcc_log_path[512];
    int n = snprintf(gcc_log_path,
                     sizeof(gcc_log_path),
                     "%s/estimator_gcc_REMB.csv",
                     g_cfg.log_dir);
    if (n <= 0 || (size_t)n >= sizeof(gcc_log_path)) {
        estimator_gcc_REMB_set_log_path(NULL);
        fprintf(stderr, "gcc-remb log path is too long, logging disabled\n");
        return;
    }
    estimator_gcc_REMB_set_log_path(gcc_log_path);
}

int rate_estimator_init(const struct rate_estimator_config *cfg)
{
    if (!cfg) {
        return -1;
    }
    g_cfg = *cfg;
    if (!g_cfg.log_dir) {
        g_cfg.log_dir = ".";
    }
    g_running = false;
    g_inited = true;
    rate_estimator_set_debug(g_cfg.debug_enabled);
    return 0;
}

int rate_estimator_start(void)
{
    if (!g_inited) {
        return -1;
    }
    if (g_running) {
        return 0;
    }

    if (g_cfg.algo == RATE_ESTIMATOR_NAIVE_EWMA) {
        configure_naive_ewma_log_path();
        if (estimator_naive_ewma_start() != 0) {
            fprintf(stderr, "naive-ewma monitor start failed (map not pinned?), using fallback bitrate\n");
        }
    } else if (g_cfg.algo == RATE_ESTIMATOR_CAMEL) {
        configure_camel_log_path();
        if (estimator_camel_start() != 0) {
            fprintf(stderr, "camel estimator start failed, using fallback bitrate\n");
        }
    } else if (g_cfg.algo == RATE_ESTIMATOR_GCC_REMB) {
        configure_gcc_remb_log_path();
        if (estimator_gcc_REMB_start() != 0) {
            fprintf(stderr, "gcc-remb estimator start failed, using fallback bitrate\n");
        }
    }

    g_running = true;
    return 0;
}

void rate_estimator_set_debug(bool enabled)
{
    g_debug_estimator = enabled;
}

bool rate_estimator_debug_enabled(void)
{
    return g_debug_estimator;
}

int rate_estimator_get_target(const struct rate_estimator_input *in,
                              struct rate_estimator_output *out)
{
    if (!g_inited || !in || !out) {
        return -1;
    }

    out->force_probe_packet = false;
    if (g_cfg.algo == RATE_ESTIMATOR_NAIVE_EWMA) {
        if (estimator_naive_ewma_estimate(in, out) != 0) {
            return -1;
        }
    } else if (g_cfg.algo == RATE_ESTIMATOR_CAMEL) {
        if (estimator_camel_estimate(in, out) != 0) {
            return -1;
        }
    } else if (g_cfg.algo == RATE_ESTIMATOR_GCC_REMB) {
        if (estimator_gcc_REMB_estimate(in, out) != 0) {
            return -1;
        }
    } else {
        if (estimator_fix_estimate(in, out) != 0) {
            return -1;
        }
    }

    if (out->target_bitrate_bps < BITRATE_MIN_BPS) {
        out->target_bitrate_bps = BITRATE_MIN_BPS;
    } else if (out->target_bitrate_bps > BITRATE_MAX_BPS) {
        out->target_bitrate_bps = BITRATE_MAX_BPS;
    }
    if (g_debug_estimator) {
        fprintf(stderr,
                "[EST] frame=%" PRIu64 " algo=%s bitrate=%" PRIu64 " send=%u probe=%u\n",
                in->frame_id,
                g_cfg.algo == RATE_ESTIMATOR_NAIVE_EWMA ? "naive_ewma"
                : (g_cfg.algo == RATE_ESTIMATOR_CAMEL
                       ? "camel"
                       : (g_cfg.algo == RATE_ESTIMATOR_GCC_REMB ? "gcc_remb" : "fix")),
                out->target_bitrate_bps,
                out->send_this_frame ? 1u : 0u,
                out->force_probe_packet ? 1u : 0u);
    }
    return 0;
}

void rate_estimator_stop(void)
{
    if (!g_running) {
        return;
    }
    if (g_cfg.algo == RATE_ESTIMATOR_NAIVE_EWMA) {
        estimator_naive_ewma_stop();
    } else if (g_cfg.algo == RATE_ESTIMATOR_CAMEL) {
        estimator_camel_stop();
    } else if (g_cfg.algo == RATE_ESTIMATOR_GCC_REMB) {
        estimator_gcc_REMB_stop();
    }
    g_running = false;
}

void rate_estimator_shutdown(void)
{
    rate_estimator_stop();
    g_inited = false;
    g_cfg.algo = RATE_ESTIMATOR_NAIVE_EWMA;
    g_cfg.debug_enabled = false;
    g_cfg.debug_naive_ewma_monitor = false;
    g_cfg.log_dir = ".";
    g_debug_estimator = false;
}

bool rate_estimator_is_running(void)
{
    return g_running;
}
