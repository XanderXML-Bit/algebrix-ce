/* Exercise actual state readers after failed replacement, not just open faults. */
#include "split/ipc.h"
#include <fileioc.h>
#include <stdio.h>
#include <string.h>

#define BACKUP "CEOLD"
static ce_shared_state_t old, next, loaded;
static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void fresh(void)
{
    fileioc_mock_reset();
    fileioc_mock_enable();
    ce_state_defaults(&old);
    old.user_vars.mask[0] = 1;
    old.user_vars.mask[2] = 128;
    old.user_vars.value[0] = -2.5f;
    old.user_vars.value[23] = 6;
    old.last_answer = 6;
    strcpy(old.last_result, "old answer");
    next = old;
    next.user_vars.value[23] = 8;
    next.last_answer = 8;
    strcpy(next.last_result, "new answer");
}

static void preserved(void)
{
    memset(&loaded, 0, sizeof(loaded));
    CHECK(ce_state_load(&loaded));
    CHECK(loaded.user_vars.value[23] == 6);
    CHECK(loaded.user_vars.value[0] == -2.5f);
    CHECK(memcmp(&loaded, &old, sizeof(old)) == 0);
}

int main(void)
{
    size_t n;
    int op;
    fresh();
    CHECK(!ce_state_load(&loaded));
    CHECK(loaded.magic == CE_IPC_MAGIC);
    CHECK(ce_state_save(&old));
    preserved();
    CHECK(ce_state_save(&next));
    CHECK(ce_state_load(&loaded) && memcmp(&next, &loaded, sizeof(next)) == 0);
    CHECK(ce_state_save(&old));
    preserved();

    /* Every short prefix, including a complete header and all but one byte. */
    for (n = 0; n < sizeof(old); n++) {
        fresh();
        CHECK(ce_state_save(&old));
        fileioc_mock_fail(MOCK_WRITE, CE_APPVAR_STATE, 0, 1, n);
        CHECK(!ce_state_save(&next));
        preserved();
        /* A second failed write must not replace the authoritative backup. */
        fileioc_mock_fail(MOCK_WRITE, CE_APPVAR_STATE, 0, 1, n);
        CHECK(!ce_state_save(&next));
        preserved();
        CHECK(ce_state_save(&next));
        CHECK(ce_state_load(&loaded) && memcmp(&next, &loaded, sizeof(next)) == 0);
        CHECK(!fileioc_mock_var_exists(BACKUP, OS_TYPE_APPVAR));
    }

    for (op = 0; op < 3; op++) {
        fresh();
        CHECK(ce_state_save(&old));
        if (op == 0) fileioc_mock_fail_writes(CE_APPVAR_STATE, 0, 1);
        if (op == 1) fileioc_mock_fail(MOCK_RENAME, CE_APPVAR_STATE, 0, 1, 0);
        if (op == 2) fileioc_mock_fail(MOCK_DELETE, BACKUP, 0, 1, 0);
        CHECK(!ce_state_save(&next));
        preserved();
        CHECK(ce_state_save(&next));
        CHECK(ce_state_load(&loaded) && memcmp(&next, &loaded, sizeof(next)) == 0);
    }

    /* A present but unreadable predecessor never selects speculative primary. */
    for (op = MOCK_READ; op <= MOCK_READ_OPEN; op++) {
        fresh();
        CHECK(ce_state_save(&old));
        fileioc_mock_fail(MOCK_DELETE, BACKUP, 0, 1, 0);
        CHECK(!ce_state_save(&next));
        fileioc_mock_fail(op, BACKUP, 0, 1, 0);
        CHECK(!ce_state_load(&loaded));
        CHECK(loaded.user_vars.mask[0] == 0);
        preserved();
    }

    /* No valid predecessor: failed initial create and partial initial write. */
    fresh();
    fileioc_mock_fail_writes(CE_APPVAR_STATE, 0, 1);
    CHECK(!ce_state_save(&old));
    CHECK(!ce_state_load(&loaded));
    CHECK(ce_state_save(&old));
    preserved();
    fresh();
    fileioc_mock_fail(MOCK_WRITE, CE_APPVAR_STATE, 0, 1, sizeof(old) / 2);
    CHECK(!ce_state_save(&old));
    CHECK(!ce_state_load(&loaded));
    fileioc_mock_fail(MOCK_DELETE, BACKUP, 0, 1, 0);
    CHECK(!ce_state_save(&next));
    CHECK(!ce_state_load(&loaded));
    CHECK(ce_state_save(&old));
    preserved();

    /* Allocation failure with a full variable table keeps the old bytes. */
    fresh();
    CHECK(ce_state_save(&old));
    for (op = 0; op < FILEIOC_MOCK_MAX_VARS - 1; op++) {
        char name[8];
        sprintf(name, "FILL%d", op);
        fileioc_mock_add_program(name, 0);
    }
    CHECK(!ce_state_save(&next));
    preserved();

    printf("state_save_test: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
