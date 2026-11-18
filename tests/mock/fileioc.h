/*
 * fileioc.h — Mock TI CE file I/O header for desktop compilation
 * Provides in-memory AppVar storage for IPC round-trip testing.
 */
#ifndef FILEIOC_H
#define FILEIOC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* TI file handle type (0 = invalid) */
typedef uint8_t ti_var_t;

/* CEISTR uses eight live variables plus a transient state predecessor. */
#define FILEIOC_MOCK_MAX_VARS 16

/* Variable types */
#define TI_APPVAR_TYPE  0x15
#ifndef OS_TYPE_APPVAR
#define OS_TYPE_APPVAR  0x15u
#endif
#ifndef OS_TYPE_PRGM
#define OS_TYPE_PRGM    0x05u
#endif
#ifndef OS_TYPE_PROT_PRGM
#define OS_TYPE_PROT_PRGM 0x06u
#endif

/* TI file I/O API */
ti_var_t ti_Open(const char *name, const char *mode);
ti_var_t ti_OpenVar(const char *name, const char *mode, uint8_t type);
int ti_Close(ti_var_t slot);
size_t ti_Write(const void *data, size_t size, size_t count, ti_var_t slot);
size_t ti_Read(void *data, size_t size, size_t count, ti_var_t slot);
size_t ti_GetSize(ti_var_t slot);
int ti_SetVar(uint8_t type, const char *name, const void *data);
int ti_SetArchiveStatus(int archived, ti_var_t slot);
int ti_IsArchived(ti_var_t slot);
int ti_Delete(const char *name);
uint8_t ti_Rename(const char *old_name, const char *new_name);


/* Each rule affects matching operations after skip calls. WRITE copies at
 * most bytes before returning a short element count. Other failures leave
 * the variable unchanged. Rules can combine independent failure points. */
enum { MOCK_WRITE, MOCK_RENAME, MOCK_DELETE, MOCK_READ, MOCK_READ_OPEN };
void fileioc_mock_fail(int op, const char *name, int skip, int count, size_t bytes);
void fileioc_mock_clear_faults(void);

/* Mock control functions for tests */
void fileioc_mock_enable(void);
void fileioc_mock_disable(void);
void fileioc_mock_reset(void);
void fileioc_mock_add_program(const char *name, int archived);
void fileioc_mock_fail_writes(const char *name, int skip, int count);
int fileioc_mock_var_exists(const char *name, uint8_t type);
int fileioc_mock_is_archived(const char *name, uint8_t type);

#endif /* FILEIOC_H */
