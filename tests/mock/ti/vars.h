#ifndef TI_VARS_H
#define TI_VARS_H

#include <stdint.h>
#include <stddef.h>

#define OS_TYPE_APPVAR 0x15u
#define OS_TYPE_PRGM 0x05u
#define OS_TYPE_PROT_PRGM 0x06u

void *os_GetAppVarData(const char *name, int *archived);

/**
 * @brief Callback function pointer type for os_RunPrgm
 * @see os_RunPrgm
 */
typedef int (*os_runprgm_callback_t)(void *data, int retval);

/**
 * Runs a program that exists on the calculator.
 *
 * On real hardware this destroys the currently running program. The callback
 * is used as the re-entry point when the called program finishes. If callback
 * is NULL, execution does not return to the calling program.
 *
 * In the desktop mock this returns immediately with 0 (success).
 */
int os_RunPrgm(const char *prgm, void *data, size_t size, os_runprgm_callback_t callback);

#endif /* TI_VARS_H */
