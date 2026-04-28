#ifndef ESTIMATOR_CAMEL_H
#define ESTIMATOR_CAMEL_H

#include "rate_estimator.h"

#include <stdint.h>

int estimator_camel_start(void);
void estimator_camel_stop(void);
void estimator_camel_set_log_path(const char *path);
int estimator_camel_estimate(const struct rate_estimator_input *in,
                             struct rate_estimator_output *out);

#endif
