/*
 * fileioc_mock.c — Mock file I/O for desktop tests
 * Provides in-memory AppVar storage so IPC round-trip tests work.
 * Call fileioc_mock_reset() between test groups to clear all stored data.
 */
#include "fileioc.h"
#include <string.h>
#include <stdlib.h>

/* ── In-memory AppVar store ───────────────────────────────── */
#define MOCK_MAX_VARS   FILEIOC_MOCK_MAX_VARS
#define MOCK_MAX_DATA   8192   /* 8KB max per variable — enough for IPC structs */
#define MOCK_MAX_NAME   16

typedef struct {
    char name[MOCK_MAX_NAME];
    uint8_t type;       /* OS_TYPE_APPVAR, OS_TYPE_PRGM, etc. */
    uint8_t data[MOCK_MAX_DATA];
    size_t size;
    int archived;       /* 0 = RAM, 1 = archived */
    int in_use;
} mock_var_t;

static mock_var_t g_vars[MOCK_MAX_VARS];

/* ── Open slot tracking ───────────────────────────────────── */
#define MOCK_MAX_SLOTS  8

typedef struct {
    int var_idx;    /* index into g_vars, -1 = unused */
    size_t cursor;  /* read/write cursor position */
    char mode;      /* 'r' or 'w' */
} mock_slot_t;

static mock_slot_t g_slots[MOCK_MAX_SLOTS];

/* ── Enable/disable flag ──────────────────────────────────── */
static int g_mock_enabled = 0;

/* ── Write-failure injection ──────────────────────────────── */
/* fileioc_mock_fail_writes(name, skip, count): let `skip` more write-opens
 * of `name` succeed, then fail the next `count` (ti_Open returns 0 and the
 * variable keeps its old contents, as when the archive is full). */
static char g_fail_name[MOCK_MAX_NAME];
static int  g_fail_skip;
static int  g_fail_count;

typedef struct {
    int op, skip, count;
    size_t bytes;
    char name[MOCK_MAX_NAME];
} mock_fault_t;
static mock_fault_t g_faults[8];

void fileioc_mock_clear_faults(void)
{
    memset(g_faults, 0, sizeof(g_faults));
}

void fileioc_mock_fail(int op, const char *name, int skip, int count, size_t bytes)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (!g_faults[i].count) {
            g_faults[i].op = op;
            g_faults[i].skip = skip;
            g_faults[i].count = count;
            g_faults[i].bytes = bytes;
            strncpy(g_faults[i].name, name, MOCK_MAX_NAME - 1);
            g_faults[i].name[MOCK_MAX_NAME - 1] = '\0';
            return;
        }
    }
    abort();
}

static mock_fault_t *fault(int op, const char *name)
{
    int i;
    for (i = 0; i < 8; i++) {
        mock_fault_t *f = &g_faults[i];
        if (f->count <= 0 || f->op != op || strcmp(f->name, name)) continue;
        if (f->skip > 0) { f->skip--; continue; }
        f->count--;
        return f;
    }
    return NULL;
}

void fileioc_mock_fail_writes(const char *name, int skip, int count)
{
    g_fail_name[0] = '\0';
    g_fail_skip = skip;
    g_fail_count = count;
    if (name) {
        strncpy(g_fail_name, name, MOCK_MAX_NAME - 1);
        g_fail_name[MOCK_MAX_NAME - 1] = '\0';
    }
}

static int write_open_fails(const char *name)
{
    if (g_fail_count <= 0 || !g_fail_name[0] || strcmp(g_fail_name, name) != 0) return 0;
    if (g_fail_skip > 0) { g_fail_skip--; return 0; }
    g_fail_count--;
    return 1;
}

void fileioc_mock_enable(void)
{
    g_mock_enabled = 1;
}

void fileioc_mock_disable(void)
{
    g_mock_enabled = 0;
}

void fileioc_mock_reset(void)
{
    int i;
    for (i = 0; i < MOCK_MAX_VARS; i++) {
        g_vars[i].in_use = 0;
        g_vars[i].name[0] = '\0';
        g_vars[i].size = 0;
        g_vars[i].archived = 0;
    }
    for (i = 0; i < MOCK_MAX_SLOTS; i++) {
        g_slots[i].var_idx = -1;
    }
    fileioc_mock_fail_writes(NULL, 0, 0);
    memset(g_faults, 0, sizeof(g_faults));
}

/* ── Internal helpers ─────────────────────────────────────── */
static int find_var(const char *name, uint8_t type)
{
    int i;
    for (i = 0; i < MOCK_MAX_VARS; i++) {
        if (g_vars[i].in_use &&
            g_vars[i].type == type &&
            strcmp(g_vars[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int find_or_create_var(const char *name, uint8_t type)
{
    int idx = find_var(name, type);
    int i;
    if (idx >= 0) return idx;
    for (i = 0; i < MOCK_MAX_VARS; i++) {
        if (!g_vars[i].in_use) {
            g_vars[i].in_use = 1;
            strncpy(g_vars[i].name, name, MOCK_MAX_NAME - 1);
            g_vars[i].name[MOCK_MAX_NAME - 1] = '\0';
            g_vars[i].type = type;
            g_vars[i].size = 0;
            g_vars[i].archived = 0;
            return i;
        }
    }
    return -1;
}

static ti_var_t alloc_slot(int var_idx, char mode)
{
    int i;
    for (i = 0; i < MOCK_MAX_SLOTS; i++) {
        if (g_slots[i].var_idx < 0) {
            g_slots[i].var_idx = var_idx;
            g_slots[i].cursor = 0;
            g_slots[i].mode = mode;
            return (ti_var_t)(i + 1); /* slots are 1-based */
        }
    }
    return 0;
}

/* ── Public API ───────────────────────────────────────────── */

ti_var_t ti_Open(const char *name, const char *mode)
{
    int idx;
    if (!g_mock_enabled || !name || !mode) return 0;

    if (mode[0] == 'w') {
        if (write_open_fails(name)) return 0;
        idx = find_or_create_var(name, OS_TYPE_APPVAR);
        if (idx < 0) return 0;
        g_vars[idx].size = 0; /* truncate on write-open */
        return alloc_slot(idx, 'w');
    } else {
        if (fault(MOCK_READ_OPEN, name)) return 0;
        idx = find_var(name, OS_TYPE_APPVAR);
        if (idx < 0) return 0;
        return alloc_slot(idx, 'r');
    }
}

ti_var_t ti_OpenVar(const char *name, const char *mode, uint8_t type)
{
    int idx;
    if (!g_mock_enabled || !name || !mode) return 0;

    if (mode[0] == 'w') {
        if (write_open_fails(name)) return 0;
        idx = find_or_create_var(name, type);
        if (idx < 0) return 0;
        g_vars[idx].size = 0;
        return alloc_slot(idx, 'w');
    } else {
        idx = find_var(name, type);
        if (idx < 0) return 0;
        return alloc_slot(idx, 'r');
    }
}

int ti_Close(ti_var_t slot)
{
    if (!g_mock_enabled) return 0;
    if (slot == 0 || slot > MOCK_MAX_SLOTS) return 0;
    g_slots[slot - 1].var_idx = -1;
    return 1;
}

size_t ti_Write(const void *data, size_t size, size_t count, ti_var_t slot)
{
    size_t total;
    mock_fault_t *f;
    int sidx, vidx;
    if (!g_mock_enabled || !data || slot == 0 || slot > MOCK_MAX_SLOTS) return 0;
    sidx = (int)(slot - 1);
    vidx = g_slots[sidx].var_idx;
    if (vidx < 0 || g_slots[sidx].mode != 'w') return 0;

    total = size * count;
    f = fault(MOCK_WRITE, g_vars[vidx].name);
    if (f && total > f->bytes) total = f->bytes;
    if (g_slots[sidx].cursor + total > MOCK_MAX_DATA) return 0;

    memcpy(g_vars[vidx].data + g_slots[sidx].cursor, data, total);
    g_slots[sidx].cursor += total;
    if (g_slots[sidx].cursor > g_vars[vidx].size)
        g_vars[vidx].size = g_slots[sidx].cursor;
    return size ? total / size : 0;
}

size_t ti_Read(void *data, size_t size, size_t count, ti_var_t slot)
{
    size_t total;
    int sidx, vidx;
    if (!g_mock_enabled || !data || slot == 0 || slot > MOCK_MAX_SLOTS) return 0;
    sidx = (int)(slot - 1);
    vidx = g_slots[sidx].var_idx;
    if (vidx < 0 || g_slots[sidx].mode != 'r') return 0;
    if (fault(MOCK_READ, g_vars[vidx].name)) return 0;

    total = size * count;
    if (g_slots[sidx].cursor + total > g_vars[vidx].size) return 0;

    memcpy(data, g_vars[vidx].data + g_slots[sidx].cursor, total);
    g_slots[sidx].cursor += total;
    return count;
}

size_t ti_GetSize(ti_var_t slot)
{
    int sidx, vidx;
    if (!g_mock_enabled || slot == 0 || slot > MOCK_MAX_SLOTS) return 0;
    sidx = (int)(slot - 1);
    vidx = g_slots[sidx].var_idx;
    if (vidx < 0) return 0;
    return g_vars[vidx].size;
}

int ti_SetVar(uint8_t type, const char *name, const void *data)
{
    int idx;
    if (!g_mock_enabled || !name || !data) return 0;

    idx = find_or_create_var(name, type);
    if (idx < 0) return 0;

    /* ti_SetVar on real hardware writes the raw data pointer contents.
     * For our mock, we write by opening a slot and writing sizeof the struct.
     * But since we don't know the struct size from here, we use a different
     * approach: just store a pointer-sized marker. The real TI implementation
     * serializes the pointed-to data as a variable. For testing purposes,
     * we'll simulate write by opening w slot and writing. But ti_SetVar
     * actually takes a void* to the raw data and writes the entire variable.
     *
     * For simplicity, we open a write slot, write enough data to fill the
     * variable, then close. The caller (ipc.c write_struct_appvar) passes
     * the struct pointer directly. We need the struct size... which we don't
     * have here.
     *
     * Real TI ti_SetVar writes the entire variable from the data pointer.
     * For our mock, we'll just store a reasonable amount (up to MOCK_MAX_DATA).
     * Since the ipc.c code uses ti_SetVar with struct pointers, we need to
     * know the size. We'll store a fixed 16KB which is enough for any IPC struct.
     */

    /* ti_SetVar on real hardware writes the data pointer contents.
     * In our mock, this is no longer used by ipc.c (which now uses
     * ti_Open/ti_Write/ti_Close). Keep as a simple stub that marks
     * the variable as non-empty. */
    g_vars[idx].size = 1;
    g_vars[idx].data[0] = 0;

    (void)data;
    return 1;
}

int ti_SetArchiveStatus(int archived, ti_var_t slot)
{
    int sidx, vidx;
    if (!g_mock_enabled || slot == 0 || slot > MOCK_MAX_SLOTS) return 0;
    sidx = (int)(slot - 1);
    vidx = g_slots[sidx].var_idx;
    if (vidx < 0) return 0;
    g_vars[vidx].archived = archived ? 1 : 0;
    return 1;
}

int ti_IsArchived(ti_var_t slot)
{
    int sidx, vidx;
    if (!g_mock_enabled || slot == 0 || slot > MOCK_MAX_SLOTS) return 0;
    sidx = (int)(slot - 1);
    vidx = g_slots[sidx].var_idx;
    if (vidx < 0) return 0;
    return g_vars[vidx].archived;
}

int ti_Delete(const char *name)
{
    int i;
    if (!g_mock_enabled || !name) return 0;
    if (fault(MOCK_DELETE, name)) return 0;
    /* Delete all vars with this name regardless of type */
    for (i = 0; i < MOCK_MAX_VARS; i++) {
        if (g_vars[i].in_use && strcmp(g_vars[i].name, name) == 0) {
            g_vars[i].in_use = 0;
            g_vars[i].name[0] = '\0';
            g_vars[i].size = 0;
            return 1;
        }
    }
    return 0;
}

/* CEdev v11.2 checks destination before source. Rename failures preserve data. */
uint8_t ti_Rename(const char *old_name, const char *new_name)
{
    int idx;
    if (fault(MOCK_RENAME, old_name)) return 3;
    if (find_var(new_name, OS_TYPE_APPVAR) >= 0) return 1;
    idx = find_var(old_name, OS_TYPE_APPVAR);
    if (idx < 0) return 2;
    strncpy(g_vars[idx].name, new_name, MOCK_MAX_NAME - 1);
    g_vars[idx].name[MOCK_MAX_NAME - 1] = '\0';
    return 0;
}

void *os_GetAppVarData(const char *name, int *archived)
{
    int idx = find_var(name, OS_TYPE_APPVAR);
    if (idx < 0) return NULL;
    if (archived) *archived = g_vars[idx].archived;
    return g_vars[idx].data;
}

/* ── Mock helper: pre-populate a program variable ─────────── */
void fileioc_mock_add_program(const char *name, int archived)
{
    int idx;
    if (!name) return;
    idx = find_or_create_var(name, OS_TYPE_PRGM);
    if (idx < 0) return;
    g_vars[idx].size = 1; /* non-empty */
    g_vars[idx].archived = archived ? 1 : 0;
}

int fileioc_mock_var_exists(const char *name, uint8_t type)
{
    return find_var(name, type) >= 0;
}

int fileioc_mock_is_archived(const char *name, uint8_t type)
{
    int idx = find_var(name, type);
    if (idx < 0) return -1;
    return g_vars[idx].archived;
}
