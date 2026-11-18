/* Non-evaluating worker and synchronous forwarding failure controls. */
#include "split/module_runtime.h"
#include <fileioc.h>
#include <stdio.h>
#include <string.h>

settings_t g_settings;
ce_float_t g_last_answer;
static ce_shared_state_t seed, state, child;
static ce_request_t request;
static ce_response_t response, answer;
static int checks, failures, launches, child_fault = -1;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void arm(int k)
{
    if (k == 0) fileioc_mock_fail_writes(CE_APPVAR_STATE, 0, 1);
    else fileioc_mock_fail(k < 3 ? MOCK_WRITE : k == 3 ? MOCK_RENAME : MOCK_DELETE,
                          k == 4 ? "CEOLD" : CE_APPVAR_STATE, 0, 1, k == 1 ? 0 : 1706);
}

int os_RunPrgm(const char *name, void *data, size_t size, os_runprgm_callback_t callback)
{
    (void)name; (void)data; (void)size; (void)callback;
    launches++;
    CHECK(ce_state_load(&child));
    memset(&answer, 0, sizeof(answer));
    answer.magic = CE_IPC_MAGIC;
    answer.version = CE_IPC_VERSION;
    answer.ok = 1;
    strcpy(answer.result, "= 9");
    if (child_fault >= 0) { arm(child_fault); child_fault = -1; }
    ce_module_runtime_finish(&request, &answer, &child);
    return 0;
}

static void preserved(void)
{
    CHECK(ce_state_load(&state));
    CHECK(state.user_vars.value[23] == 6 && state.user_vars.value[0] == -2.5f);
}

static void fresh(void)
{
    fileioc_mock_reset();
    fileioc_mock_enable();
    ce_state_defaults(&seed);
    seed.user_vars.mask[0] = 1;
    seed.user_vars.mask[2] = 128;
    seed.user_vars.value[0] = -2.5f;
    seed.user_vars.value[23] = 6;
    strcpy(seed.pending_module, "ROUTER");
    CHECK(ce_state_save(&seed));
    fileioc_mock_add_program("WORKER", 1);
    state = seed;
    g_settings = seed.settings;
    g_last_answer = seed.last_answer;
    memset(&request, 0, sizeof(request));
    strcpy(request.expression, "9");
    launches = 0;
}

int main(void)
{
    int k;
    char error[128];
    for (k = 0; k < 5; k++) {
        fresh();
        memset(&answer, 0, sizeof(answer));
        answer.magic = CE_IPC_MAGIC;
        answer.version = CE_IPC_VERSION;
        answer.ok = 1;
        CHECK(ce_response_write(&answer));
        arm(k);
        CHECK(!ce_module_runtime_forward("ROUTER", "WORKER", &request, &state,
                                         &response, error, sizeof(error)));
        CHECK(strcmp(error, "IPC state save failed") == 0);
        CHECK(launches == 0);
        CHECK(!ce_response_read(&response));
        preserved();
        CHECK(ce_module_runtime_forward("ROUTER", "WORKER", &request, &state,
                                        &response, error, sizeof(error)));
        CHECK(response.ok && strcmp(response.result, "= 9") == 0);
        preserved();

        fresh();
        child_fault = k;
        CHECK(!ce_module_runtime_forward("ROUTER", "WORKER", &request, &state,
                                         &response, error, sizeof(error)));
        CHECK(strcmp(error, "No module response") == 0);
        CHECK(launches == 1);
        CHECK(!ce_response_read(&response));
        preserved();
        CHECK(ce_module_runtime_forward("ROUTER", "WORKER", &request, &state,
                                        &response, error, sizeof(error)));
        CHECK(response.ok);
        preserved();
    }
    printf("state_runtime_test: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
