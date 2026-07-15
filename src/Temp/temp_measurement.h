#ifndef TEMP_MEASUREMENT_H
#define TEMP_MEASUREMENT_H
#include "protocol.h"
#include "lib/sensors.h"

void print_measurement(int rh_milli, int temp_milli);
void temp_measurement_read(pqc_temp_payload_t *out);

#endif /* TEMP_MEASUREMENT_H */