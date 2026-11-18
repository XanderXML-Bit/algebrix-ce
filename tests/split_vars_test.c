/*
 * split_vars_test.c — desktop check of stored variables (`5->X`, then
 * `X^2`) through the real worker entry points, an internal review.
 *
 * Built twice by tests/Makefile, each time with the PROGRAM_CFLAGS read
 * from the module's own makefile:
 *   split_vars_cebase  split/Makefile.base          + src/split/module_main.c
 *   split_vars_celscx  split/Makefile.lists_complex + src/modules/common/module_entry.c
 * The module's main() is renamed with -Dmain=module_program_main, so this
 * file #undefs main for its own entry point.
 *
 * Each run plays the launcher the way the CE does it: parse the input,
 * write CE_REQ with the AST over astwire, zero the module's globals (a
 * program load zeroes .bss), call the module's main() and read CE_RESP and
 * CE_STATE back through the real ipc.c readers.  CE_STATE stays in the
 * fileioc mock between runs, as it stays in the calculator's RAM.
 *
 *   --dump FILE  write CE_STATE's bytes to FILE at the end
 *   --load FILE  start with CE_STATE read from FILE (a state another
 *                module wrote, so the cross-module hand-off is real)
 *
 * Before an internal review, `5->X` showed "= 5" (CELSCX bound X only inside its
 * own eval context) and the next entry `X` showed "X has no value": no
 * module kept scalar values.
 */
#undef main
#include "calcengine.h"
#include "memory.h"
#include "token.h"
#include "parser.h"
#include "ast.h"
#include "eval.h"
#include "split/astwire.h"
#include "split/ipc.h"
#include "split/module_runtime.h"

#include <fileioc.h>
#include <tice.h>
#include <stdio.h>
#include <string.h>

#if !defined(HW_BUILD_SPLIT) || !CE_EVAL_USER_VARS || !CE_PROC_GENERIC || CE_PROC_PARSE_INPUT \
    || CE_PROC_ALG || CE_PROC_CALC || CE_PROC_STAT || CE_PROC_MATRIX || CE_PROC_USE_CLASSIFY
#error "split_vars_test.c must be built with split/Makefile.base or Makefile.lists_complex PROGRAM_CFLAGS"
#endif

#if CE_PROC_CMPLX
#define MODULE_NAME "CELSCX"
#else
#define MODULE_NAME "CEBASE"
#endif

int module_program_main(void);   /* the module's main(), renamed */

void fileioc_mock_reset(void);
void fileioc_mock_enable(void);
void fileioc_mock_fail_writes(const char *name, int skip, int count);

/* ipc.c links os_RunPrgm; nothing here forwards. */
int os_RunPrgm(const char *prgm, void *data, size_t size, os_runprgm_callback_t callback)
{
    (void)prgm; (void)data; (void)size; (void)callback;
    return -1;
}

static int g_run = 0;
static int g_fail = 0;
static bool fail_next_response = false;
static bool fail_next_state = false;
static int fail_next_state_op = -1;
static size_t fail_next_state_bytes;

static void check(int cond, const char *what, const char *in, const char *got)
{
    g_run++;
    if (!cond) {
        g_fail++;
        printf("FAIL [%s]: %s [%s] got \"%s\"\n", MODULE_NAME, what, in, got ? got : "(null)");
    }
}

static int first_line_is(const char *text, const char *want)
{
    size_t n = strlen(want);
    return strncmp(text, want, n) == 0 && (text[n] == '\n' || text[n] == '\0');
}

static int slot_set(const ce_user_vars_t *v, char letter)
{
    int i = letter - 'A';
    return (v->mask[i >> 3] >> (i & 7)) & 1;
}

/* One launcher -> module -> launcher round trip.  Returns false when the
 * module did not leave a CE_RESP that ce_response_read() accepts. */
static bool run_entry(const char *input, ce_response_t *resp)
{
    static tokenizer_t tok;
    static parser_t par;
    static ce_request_t req;
    static ce_shared_state_t state;
    ast_node_t *parsed;
    uint16_t wire_size = 0;

    memset(resp, 0, sizeof(*resp));
    memset(&req, 0, sizeof(req));
    mem_free_all();
    tok_init(&tok, input);
    if (tok_tokenize(&tok) != CE_OK) return false;
    parser_init(&par, &tok);
    parsed = parser_parse(&par);
    if (!parsed) return false;
    req.magic = CE_IPC_MAGIC;
    req.version = CE_IPC_VERSION;
    req.flags = CE_REQ_FLAG_HAS_AST;
    ce_state_load(&state);
    req.settings = state.settings;
    req.last_answer = state.last_answer;
    strcpy(state.pending_module, MODULE_NAME);
    state.has_pending_response = 0;
    if (!ce_state_save(&state)) return false;
    strncpy(req.expression, input, sizeof(req.expression) - 1);
    strncpy(req.caller, "CALCENG", sizeof(req.caller) - 1);
    strncpy(req.return_to, "CALCENG", sizeof(req.return_to) - 1);
    if (!ce_ast_serialize(parsed, req.ast_payload, sizeof(req.ast_payload), &wire_size))
        return false;
    req.ast_size = wire_size;
    mem_free_all();
    if (!ce_request_write(&req)) return false;
    ce_response_delete();

    /* A program load zeroes .bss: nothing survives in RAM globals. */
    memset(&g_user_vars, 0, sizeof(g_user_vars));
    g_last_answer = 0;

    if (fail_next_response) {
        fileioc_mock_fail_writes(CE_APPVAR_RESP, 0, 1);
        fail_next_response = false;
    }
    if (fail_next_state) {
        fileioc_mock_fail_writes(CE_APPVAR_STATE, 0, 1);
        fail_next_state = false;
    }
    if (fail_next_state_op >= 0) {
        fileioc_mock_fail(fail_next_state_op,
                         fail_next_state_op == MOCK_DELETE ? "CEOLD" : CE_APPVAR_STATE,
                         0, 1, fail_next_state_bytes);
        fail_next_state_op = -1;
    }
    module_program_main();
    return ce_response_read(resp);
}

static bool load_state(ce_shared_state_t *st)
{
    memset(st, 0, sizeof(*st));
    return ce_state_load(st);
}

static bool file_to_state(const char *path)
{
    static ce_shared_state_t st;
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return false;
    n = fread(&st, 1, sizeof(st), f);
    fclose(f);
    return n == sizeof(st) && ce_state_save(&st);
}

static bool state_to_file(const char *path)
{
    static ce_shared_state_t st;
    FILE *f;
    size_t n;
    uint8_t slot = ti_Open(CE_APPVAR_STATE, "r");
    if (!slot) return false;
    n = ti_Read(&st, sizeof(st), 1, slot);
    ti_Close(slot);
    if (n != 1) return false;
    f = fopen(path, "wb");
    if (!f) return false;
    n = fwrite(&st, 1, sizeof(st), f);
    fclose(f);
    return n == sizeof(st);
}

/* Home-screen answer line 1 must be `want`. */
static void expect_ok(const char *in, const char *want)
{
    ce_response_t resp;
    bool read = run_entry(in, &resp);
    check(read, "CE_RESP accepted by ce_response_read", in, resp.error);
    check(resp.ok == 1, "answers ok", in, resp.error);
    check(first_line_is(resp.result, want), want, in, resp.result);
}

static void expect_err(const char *in, const char *want)
{
    ce_response_t resp;
    bool read = run_entry(in, &resp);
    check(read, "CE_RESP accepted by ce_response_read", in, resp.error);
    check(resp.ok == 0, "fails", in, resp.result);
    check(first_line_is(resp.error, want), want, in, resp.error);
}

static void own_module_checks(void)
{
    static ce_shared_state_t st;
    ce_response_t resp;
    const char *line;
    size_t n;
    int lines = 0, lines_ok = 1;

    fileioc_mock_reset();
    fileioc_mock_enable();

    /* Fresh calculator: no CE_STATE.  X has no value, and the hint now
     * says how to store one; every line still fits 26 columns. */
    check(run_entry("X^2", &resp), "CE_RESP accepted", "X^2", resp.error);
    check(resp.ok == 0 && first_line_is(resp.error, "X has no value"),
          "X has no value", "X^2", resp.error);
    check(strstr(resp.error, "\nOr store a value: 5->X") != NULL, "store hint",
          "X^2", resp.error);
    for (line = resp.error; *line; line += n + (line[n] == '\n')) {
        n = strcspn(line, "\n");
        if (n > 26) lines_ok = 0;
        lines++;
    }
    check(lines_ok && lines == 5, "5 hint lines of <= 26 cols", "X^2", resp.error);

#if CE_SAFE_FLOAT_TINY
    /* an internal review: a scalar store of a plain real value runs here (CEBASE),
     * so #140's tiny refusal covers it.  A refused store keeps X unset;
     * CELSCX used to store the flushed 0. */
    expect_err("1E-30*1E-30->X", "Tiny value unsupported");
    check(load_state(&st) && !slot_set(&st.user_vars, 'X'),
          "refused tiny store leaves X unset", "1E-30*1E-30->X", "");
    expect_err("X", "X has no value");
    /* The variable's letter is echoed (it printed "->L" and a raw byte). */
    check(run_entry("5->X", &resp) && resp.ok == 1 &&
          strstr(resp.result, "\nParsed: 5->X") != NULL,
          "Parsed: echoes ->X", "5->X", resp.result);
    {
        char text[32];
        ast_node_t *root;
        static tokenizer_t tok;
        static parser_t par;
        mem_free_all();
        tok_init(&tok, "5->X");
        check(tok_tokenize(&tok) == CE_OK, "tokenize", "5->X", "");
        parser_init(&par, &tok);
        root = parser_parse(&par);
        ast_to_str(root, text, sizeof(text));
        check(root && strcmp(text, "5->X") == 0, "ast_to_str prints 5->X", "5->X", text);
        mem_free_all();
        tok_init(&tok, "5->L1");
        check(tok_tokenize(&tok) == CE_OK, "tokenize", "5->L1", "");
        parser_init(&par, &tok);
        root = parser_parse(&par);
        ast_to_str(root, text, sizeof(text));
        check(root && strcmp(text, "5->L1") == 0, "list store keeps L1", "5->L1", text);
        mem_free_all();
    }
    /* A refused tiny store keeps the previous value too. */
    expect_err("1E-30*1E-30->X", "Tiny value unsupported");
    expect_ok("X", "= 5");
    /* -0 is stored as +0 (the CE's compare printed it as "-0"). */
    expect_ok("-0->W", "= 0");
    {
        static const unsigned char plus_zero[4] = { 0, 0, 0, 0 };
        check(load_state(&st) && slot_set(&st.user_vars, 'W') &&
              memcmp(&st.user_vars.value['W' - 'A'], plus_zero, 4) == 0,
              "-0 store keeps +0 in CE_STATE", "-0->W", "");
    }
    expect_ok("W", "= 0");
    /* A tiny value already in CE_STATE (written by another module) is
     * refused when read, and the table is not changed. */
    {
        static ce_shared_state_t before, after;
        static const unsigned char tiny_bits[4] = { 1, 0, 0, 0 };   /* 2^-149 */
        check(load_state(&st), "CE_STATE loads", "tiny preload", "");
        memcpy(&st.user_vars.value['T' - 'A'], tiny_bits, 4);
        st.user_vars.mask[('T' - 'A') >> 3] |= (uint8_t)(1u << (('T' - 'A') & 7));
        check(ce_state_save(&st), "CE_STATE saves", "tiny preload", "");
        check(load_state(&before), "CE_STATE loads", "tiny preload", "");
        expect_err("T", "Tiny value unsupported");
        expect_err("T+1", "Tiny value unsupported");
        /* Conservative, by design (internal review): a construct that rebinds
         * T locally still reads the stored value to save it, and that read
         * refuses. It never gives a wrong number. */
        expect_err("sum(T,T,1,3)+0", "Tiny value unsupported");
        expect_err("0+int(T,T,0,1)", "Tiny value unsupported");
        /* Only lookup_var's check sees T here: without it the integrand's
         * tiny constant gave "no convergence". */
        expect_err("int(T,x,0,1)", "Tiny value unsupported");
        check(load_state(&after) &&
              memcmp(&before.user_vars, &after.user_vars, sizeof(before.user_vars)) == 0,
              "tiny stored value refused, table unchanged", "T", "");
        /* Clear T again so the rest of the run starts as before. */
        after.user_vars.mask[('T' - 'A') >> 3] &= (uint8_t)~(1u << (('T' - 'A') & 7));
        after.user_vars.mask[('W' - 'A') >> 3] &= (uint8_t)~(1u << (('W' - 'A') & 7));
        after.user_vars.mask[('X' - 'A') >> 3] &= (uint8_t)~(1u << (('X' - 'A') & 7));
        check(ce_state_save(&after), "CE_STATE saves", "tiny cleanup", "");
    }
    /* Small normal values store and read back exactly. */
    expect_ok("1E-7->Y", "= 1e-07");
    expect_ok("Y*1E7", "= 1");
    expect_ok("2^-30*2^-30*2^-30*2^-30*2^-6->Z", "= 1.17549e-38");
    expect_err("Z/2", "Tiny value unsupported");
    expect_ok("Z", "= 1.17549e-38");
    {
        static ce_shared_state_t cl;
        check(load_state(&cl), "CE_STATE loads", "cleanup", "");
        memset(&cl.user_vars, 0, sizeof(cl.user_vars));
        check(ce_state_save(&cl), "CE_STATE saves", "cleanup", "");
    }
    expect_err("X", "X has no value");
    /* Reviews of an internal review.  A store is only ever the root: parser_parse
     * accepts "->" after the whole expression, so no store runs inside a
     * sampler or as an operand.  eval_expr's NODE_STORE case relies on it. */
    {
        static const char *const nested[] = {
            "(5->X)+1", "1+(5->X)", "sum(k->X,k,1,3)", "int(x->Y,x,0,1)",
            "5->X->Y", "5->X+1",
        };
        static tokenizer_t tok;
        static parser_t par;
        size_t i;
        for (i = 0; i < sizeof(nested) / sizeof(nested[0]); i++) {
            ast_node_t *root;
            mem_free_all();
            tok_init(&tok, nested[i]);
            root = tok_tokenize(&tok) == CE_OK ?
                   (parser_init(&par, &tok), parser_parse(&par)) : NULL;
            check(root == NULL, "a store is only accepted at the root", nested[i], "");
        }
        mem_free_all();
    }
    /* A root store whose value passed through a sampler or a tiny
     * intermediate answers exactly as the bare expression, and stores
     * nothing when that is refused.  The root lim( and int( stores below
     * run CEBASE directly: they hold at module level only, because the
     * launcher routes a root command store to CELSCX (known limitation;
     * see SCALAR_STORE_ROUTES in test_module_flag_parity.py). */
    expect_err("sum(1E-37*k/9,k,1,3)", "Tiny value unsupported");
    expect_err("sum(1E-37*k/9,k,1,3)+1->X", "Tiny value unsupported");
    expect_err("lim(1E-37*x/9,x,1)->X", "Tiny value unsupported");
    expect_err("1E-37/10^5*10^5->X", "Tiny value unsupported");
    expect_err("1E-30*1E-30+1->X", "Tiny value unsupported");
    expect_err("int(x*1E-37,x,0,1)->X", "no convergence");
    /* Statistics and polar operands now reach CEBASE when stored; it
     * refuses them exactly as CELSCX did on 967d5b9 (owned ROM). */
    expect_err("1+nCr(5,2)->X", "Statistics not enabled in this module");
    expect_err("1+normalcdf(0,1)->X", "Statistics not enabled in this module");
    expect_err("1+abs(polar(1,2))->X", "Polar/advanced op (use at top level)");
    check(load_state(&st) && !slot_set(&st.user_vars, 'X'),
          "refused stores leave X unset", "sampler/stat stores", "");
    expect_err("X", "X has no value");
#endif

    /* The card's case: store, then use it. */
    expect_ok("5->X", "= 5");
    check(load_state(&st), "CE_STATE loads after a store", "5->X", "");
    check(slot_set(&st.user_vars, 'X') && st.user_vars.value['X' - 'A'] == 5.0f,
          "CE_STATE holds X = 5", "5->X", "");
    check(st.last_answer == 5.0f, "Ans = 5 after the store", "5->X", "");
    expect_ok("X^2", "= 25");
    expect_ok("X", "= 5");
    expect_ok("2X+1", "= 11");
    expect_ok("sqrt(X+4)", "= 3");
    /* Lowercase shares the uppercase slot. */
    expect_ok("x^2", "= 25");
    /* Overwrite, and a store that uses the old value. */
    expect_ok("X+1->X", "= 6");
    expect_ok("X", "= 6");
    expect_ok("-2.5->A", "= -2.5");
    expect_ok("A*X", "= -15");
    /* A store that fails keeps the old value. */
    expect_err("1/0->X", "Division by zero");
    expect_ok("X", "= 6");
    /* Other letters stay unbound. */
    expect_err("B+1", "B has no value");
    expect_err("A+B", "B has no value");
    /* A local binding wins over the stored value; after it, X is 6 again. */
    expect_ok("sum(X,X,1,3)", "= 6");
    expect_ok("sum(X,X,1,4)+X", "= 16");
    /* Ans still works next to stored values. */
    expect_ok("ANS+X", "= 22");

    /* Stored aliases must not bypass an explicit local binding. */
    expect_ok("sum(x,X,1,3)+0", "= 6");
    expect_ok("sum(X,x,1,3)+0", "= 6");
    expect_ok("sum(sum(x,x,1,2)+x,x,1,2)+0", "= 9");
    expect_ok("sum(sum(X,X,1,2)+X,x,1,2)+0", "= 9");
    expect_ok("sum(prod(x,x,1,2)+x,x,1,2)+0", "= 7");
    expect_ok("sum(int(x,x,0,1)+x,x,1,2)+0", "= 4");
    expect_ok("sum(int(X,X,0,1)+X,x,1,2)+0", "= 4");
#if !CE_PROC_CMPLX
    /* Dependency checks must share numeric evaluation's letter aliases. */
    expect_err("0+int(1+100*e^(-1e10*(X-0.123456)^2),x,0,1)", "no convergence");
    expect_err("0+int(1+100*e^(-1e10*(x-0.123456)^2),X,0,1)", "no convergence");
#endif
    expect_ok("X", "= 6");
    {
        eval_ctx_t ctx;
        eval_init(&ctx);
        check(eval_set_var(&ctx, 'x', 2.0f) && eval_set_var(&ctx, 'X', 3.0f) &&
              ctx.vars[0].name == 'X' && ctx.vars[0].value == 3.0f && !ctx.vars[1].bound,
              "mixed-case update shares one local slot", "local binding", "");
        eval_clear_var(&ctx, 'x');
        check(!ctx.vars[0].bound, "mixed-case clear removes local binding", "local binding", "");
    }
    {
        char deep[MAX_INPUT_LEN] = "";
        int j;
        for (j = 0; j < 22; j++) strcat(deep, "sin(");
        strcat(deep, "X");
        for (j = 0; j < 22; j++) strcat(deep, ")");
        strcat(deep, "->A");
        expect_err(deep, "Eval recursion limit");
        expect_ok("A", "= -2.5");
    }

    /* An unreadable response is a failed submission, not a stored update. */
    fail_next_response = true;
    check(!run_entry("7->X", &resp), "response write failure injected", "7->X", "");
    check(load_state(&st) && st.user_vars.value['X' - 'A'] == 6.0f,
          "response failure keeps previous table", "7->X", "");
    {
        static ce_request_t req;
        char error[128];
        memset(&req, 0, sizeof(req));
        check(ce_module_runtime_recover_child_failure("CALCENG", "", &req, &st,
                                                     error, sizeof(error)),
              "launcher recovers failed response", "7->X", error);
    }
    expect_ok("X", "= 6");

    /* A failed state save must not leave a readable success response. */
    fail_next_state = true;
    check(!run_entry("8->X", &resp), "state save failure invalidates response", "8->X", "");
    check(load_state(&st) && st.user_vars.value['X' - 'A'] == 6.0f,
          "failed state save preserves old value", "8->X", "");
    expect_ok("X", "= 6");

    /* Actual worker completion after post-open failure and short writes. */
    {
        int k;
        for (k = 0; k < 5; k++) {
            fail_next_state_op = k < 3 ? MOCK_WRITE : k == 3 ? MOCK_RENAME : MOCK_DELETE;
            fail_next_state_bytes = k == 0 ? 0 : k == 1 ? sizeof(st) / 2 : sizeof(st) - 1;
            check(!run_entry("8->X", &resp), "failed replacement suppresses CE_RESP", "8->X", "");
            check(load_state(&st) && st.user_vars.value['X' - 'A'] == 6.0f &&
                  st.user_vars.value[0] == -2.5f,
                  "failed replacement preserves X and A", "8->X", "");
            expect_ok("X^2+A", "= 33.5");
        }
    }

    /* Relaunch: the next program load starts from CE_STATE only. */
    expect_ok("X^2+A", "= 33.5");

    /* Corrupt mask bits past Z are dropped on load. */
    check(load_state(&st), "CE_STATE loads", "mask", "");
    st.user_vars.mask[3] = 0xFF;
    st.user_vars.value['Y' - 'A'] = 7.0f;
    st.user_vars.value['Z' - 'A'] = 8.0f;
    check(ce_state_save(&st), "CE_STATE saves", "mask", "");
    check(load_state(&st) && st.user_vars.mask[3] == 0x03, "mask bits past Z cleared",
          "mask", "");
    expect_ok("Y+Z", "= 15");

    /* An old (version 5) CE_STATE is not trusted: defaults, no values. */
    check(load_state(&st), "CE_STATE loads", "v5", "");
    st.version = 5;
    check(ce_state_save(&st), "CE_STATE saves", "v5", "");
    expect_err("X", "X has no value");
}

int main(int argc, char **argv)
{
    const char *dump = NULL;
    const char *load = NULL;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    for (i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--dump") == 0) dump = argv[i + 1];
        else if (strcmp(argv[i], "--load") == 0) load = argv[i + 1];
    }

    own_module_checks();

    if (load) {
        /* A CE_STATE written by the other module: its stores are kept. */
        fileioc_mock_reset();
        fileioc_mock_enable();
        check(file_to_state(load), "CE_STATE from the other module loads", load, "");
        expect_ok("X^2", "= 25");
        expect_ok("Q*X", "= -10");
        expect_err("B", "B has no value");
    }
    if (dump) {
        /* The card's sequence for the other module: 5->X here, X^2 there. */
        fileioc_mock_reset();
        fileioc_mock_enable();
        expect_ok("5->X", "= 5");
        expect_ok("-2->Q", "= -2");
        check(state_to_file(dump), "CE_STATE dumped", dump, "");
    }

    printf("split_vars_test (%s): %d checks, %d failed\n", MODULE_NAME, g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
