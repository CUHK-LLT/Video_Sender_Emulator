#include "estimator_fix.h"

#include "proto.h"

int estimator_fix_estimate(const struct rate_estimator_input *in,
                           struct rate_estimator_output *out)
{
    if (!in || !out) {
        return -1;
    }

    out->target_bitrate_bps = in->init_bitrate_bps > 0 ? in->init_bitrate_bps : BITRATE_INIT_BPS;
    out->send_this_frame = true;
    out->force_probe_packet = false;
    out->burst_budget_packets = 0;
    return 0;
}
