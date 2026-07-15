#ifndef RESET_DIAGNOSTICS_H
#define RESET_DIAGNOSTICS_H

#include <stdint.h>
#include <stdio.h>
#include "em_rmu.h"

static void
print_reset_cause(void)
{
  uint32_t cause;

  cause = RMU_ResetCauseGet();

  printf("\n[RESET] Cause raw: 0x%08lx\n", (unsigned long)cause);

#ifdef RMU_RSTCAUSE_PORST
  if(cause & RMU_RSTCAUSE_PORST) {
    printf("[RESET] Power-on reset\n");
  }
#endif

#ifdef RMU_RSTCAUSE_EXTRST
  if(cause & RMU_RSTCAUSE_EXTRST) {
    printf("[RESET] External pin reset\n");
  }
#endif

#ifdef RMU_RSTCAUSE_SYSREQRST
  if(cause & RMU_RSTCAUSE_SYSREQRST) {
    printf("[RESET] Software system reset\n");
  }
#endif

#ifdef RMU_RSTCAUSE_WDOGRST
  if(cause & RMU_RSTCAUSE_WDOGRST) {
    printf("[RESET] Watchdog reset\n");
  }
#endif

#ifdef RMU_RSTCAUSE_LOCKUPRST
  if(cause & RMU_RSTCAUSE_LOCKUPRST) {
    printf("[RESET] CPU lockup reset / HardFault-type reset\n");
  }
#endif

#ifdef RMU_RSTCAUSE_BODREGRST
  if(cause & RMU_RSTCAUSE_BODREGRST) {
    printf("[RESET] Brown-out regulated supply reset\n");
  }
#endif

#ifdef RMU_RSTCAUSE_BODUNREGRST
  if(cause & RMU_RSTCAUSE_BODUNREGRST) {
    printf("[RESET] Brown-out unregulated supply reset\n");
  }
#endif

#ifdef RMU_RSTCAUSE_EM4RST
  if(cause & RMU_RSTCAUSE_EM4RST) {
    printf("[RESET] EM4 wakeup reset\n");
  }
#endif

  RMU_ResetCauseClear();
}

#endif /* RESET_DIAGNOSTICS_H */
