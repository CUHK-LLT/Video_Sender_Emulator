#ifndef ESTIMATOR_GCC_REMB_H
#define ESTIMATOR_GCC_REMB_H

#include "rate_estimator.h"

int estimator_gcc_REMB_start(void);
void estimator_gcc_REMB_stop(void);
void estimator_gcc_REMB_set_log_path(const char *path);
int estimator_gcc_REMB_estimate(const struct rate_estimator_input *in,
                                struct rate_estimator_output *out);

#endif
