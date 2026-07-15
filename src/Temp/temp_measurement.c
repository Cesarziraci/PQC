#include "temp_measurement.h"
#include "si7021.h"

void print_measurement(int rh_milli, int temp_milli)
{
  long rh_int = (long)(rh_milli / 1000);
  long rh_frac = (long)((rh_milli >= 0 ? rh_milli : -rh_milli) % 1000);

  long t_int = (long)(temp_milli / 1000);
  long t_frac = (long)((temp_milli >= 0 ? temp_milli : -temp_milli) % 1000);

  printf("RH:%ld.%03ld %%,  T:%ld.%03ld C\r\n", rh_int, rh_frac, t_int, t_frac);
}

void temp_measurement_read(pqc_temp_payload_t *out)
{
  uint32_t hume = 0;
  int32_t temper = 0;

  if(out == NULL) {
    return;
  }

  /*
   * SI7021_measure() returns humidity and temperature already scaled by 1000.
   * Keep the milli-units here because pqc_temp_payload_t and print_measurement()
   * both expect RH and temperature in milli-units.
   */
  SI7021_measure(&hume, &temper);

  out->rh_milli = (int32_t)hume;
  out->temp_milli = temper;
}
