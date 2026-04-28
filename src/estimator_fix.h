#ifndef ESTIMATOR_FIX_H
#define ESTIMATOR_FIX_H

#include "rate_estimator.h"

int estimator_fix_estimate(const struct rate_estimator_input *in,
                           struct rate_estimator_output *out);

#endif
