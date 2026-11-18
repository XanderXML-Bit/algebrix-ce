#include "calcengine.h"

#ifndef DESKTOP_BUILD
#include <keypadc.h>
#endif

settings_t g_settings = { ANGLE_RADIANS, true, 6, COMPLEX_OFF, false, STEP_ALL,
                          SOLVE_FULL_AUTO, METHOD_AUTO };
ce_float_t g_last_answer = 0;

bool check_cancel(void)
{
#ifndef DESKTOP_BUILD
    /* User-abort from long computations.  CLEAR is the in-app cancel key;
     * ON is the hardware break key that TI-84 CE users expect to halt any
     * running program.  On real hardware, pressing ON during an eZ80 ASM
     * program sets kb_Data[1] & kb_On (group 1, bit 1); honoring it here
     * lets a user recover from a non-convergent integral, a long-running
     * series expansion, or any other slow computation without pulling
     * batteries or waiting for the OS watchdog. */
    kb_Scan();
    if ((kb_Data[6] & kb_Clear) != 0) return true;
    if ((kb_Data[1] & kb_On)    != 0) return true;
    return false;
#else
    return false;
#endif
}

void progress_start(void)
{
    /* No-op in split runtime */
}

void progress_check(void)
{
    /* No-op in split runtime */
}
