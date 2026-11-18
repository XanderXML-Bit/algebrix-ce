#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calcengine.h"
#include "token.h"
#include "parser.h"
#include "macros.h"
#include "memory.h"
#include "steps.h"
#include "split/classify.h"
#include "split/processor.h"

typedef struct {
    const char *expr;
    integ_method_t method;
    bool show_steps;
    bool json;
} cli_options_t;

typedef enum {
    CLI_PARSE_OK = 0,
    CLI_PARSE_HELP,
    CLI_PARSE_ERROR
} cli_parse_result_t;

typedef struct {
    char expanded[MAX_INPUT_LEN];
    bool input_had_equals;
    ast_node_t *root;
    ce_domain_t domain;
    const char *program_name;
} parse_context_t;

static const settings_t k_default_settings = {
    ANGLE_RADIANS, true, 6, COMPLEX_OFF, false, STEP_ALL,
    SOLVE_FULL_AUTO, METHOD_AUTO
};

settings_t g_settings = {
    ANGLE_RADIANS, true, 6, COMPLEX_OFF, false, STEP_ALL,
    SOLVE_FULL_AUTO, METHOD_AUTO
};
ce_float_t g_last_answer = 0.0f;

static const char *domain_name(ce_domain_t domain)
{
    switch (domain) {
    case CE_DOMAIN_CORE: return "core";
    case CE_DOMAIN_ALG: return "algebra";
    case CE_DOMAIN_ALG_EXPFAC: return "algebra_expand_factor";
    case CE_DOMAIN_ALG_ADV: return "algebra_advanced";
    case CE_DOMAIN_ALG_GEO: return "algebra_geometry";
    case CE_DOMAIN_CALC: return "calculus";
    case CE_DOMAIN_CALC_TANGENT: return "calculus_tangent";
    case CE_DOMAIN_CALC_LIMIT: return "limits";
    case CE_DOMAIN_CALC_INT: return "integrals";
    case CE_DOMAIN_CALC_POLAR: return "polar";
    case CE_DOMAIN_CALC_SERIES: return "series";
    case CE_DOMAIN_CALC_SEQ: return "sequences";
    case CE_DOMAIN_STAT: return "statistics";
    case CE_DOMAIN_PROB: return "probability";
    case CE_DOMAIN_MATRIX: return "matrix";
    case CE_DOMAIN_GRAPH: return "graph";
    case CE_DOMAIN_FIN: return "finance";
    case CE_DOMAIN_CMPLX: return "lists_complex";
    default: return "unknown";
    }
}

static void print_usage(void)
{
    printf("calcengine -- one-shot desktop solver\n");
    printf("\n");
    printf("Usage:\n");
    printf("  calcengine --solve <expression> [--method=<auto|usub|parts|trigsub|partial-frac>] [--show-steps] [--json]\n");
    printf("\n");
    printf("Options:\n");
    printf("  --solve <expr>        Solve a single expression and exit\n");
    printf("  --method <name>       Force integration method when applicable\n");
    printf("  --show-steps          Include step-by-step output\n");
    printf("  --json                Emit machine-readable JSON\n");
    printf("  --help                Show this help text\n");
}

static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0u;
    if (!dst || dst_size == 0u) return;
    if (!src) src = "";

    while (*src && di + 6u < dst_size) {
        switch (*src) {
        case '\\':
            dst[di++] = '\\';
            dst[di++] = '\\';
            break;
        case '"':
            dst[di++] = '\\';
            dst[di++] = '"';
            break;
        case '\n':
            dst[di++] = '\\';
            dst[di++] = 'n';
            break;
        case '\r':
            dst[di++] = '\\';
            dst[di++] = 'r';
            break;
        case '\t':
            dst[di++] = '\\';
            dst[di++] = 't';
            break;
        default:
            dst[di++] = *src;
            break;
        }
        src++;
    }
    dst[di] = '\0';
}

static bool parse_method_name(const char *value, integ_method_t *out_method)
{
    if (!value || !out_method) return false;
    if (strcmp(value, "auto") == 0) {
        *out_method = METHOD_AUTO;
        return true;
    }
    if (strcmp(value, "usub") == 0 ||
        strcmp(value, "u-sub") == 0 ||
        strcmp(value, "u_sub") == 0) {
        *out_method = METHOD_USUB;
        return true;
    }
    if (strcmp(value, "parts") == 0 ||
        strcmp(value, "by-parts") == 0 ||
        strcmp(value, "byparts") == 0) {
        *out_method = METHOD_PARTS;
        return true;
    }
    if (strcmp(value, "trigsub") == 0 ||
        strcmp(value, "trig-sub") == 0 ||
        strcmp(value, "trig_sub") == 0) {
        *out_method = METHOD_TRIGSUB;
        return true;
    }
    if (strcmp(value, "partial-frac") == 0 ||
        strcmp(value, "partialfrac") == 0 ||
        strcmp(value, "partial-fractions") == 0 ||
        strcmp(value, "partialfractions") == 0) {
        *out_method = METHOD_PARTIAL_FRAC;
        return true;
    }
    return false;
}

static cli_parse_result_t parse_args(int argc, char **argv, cli_options_t *opts)
{
    int i;

    if (!opts) return CLI_PARSE_ERROR;
    memset(opts, 0, sizeof(*opts));
    opts->method = METHOD_AUTO;

    for (i = 1; i < argc; i++) {
        const char *value = NULL;

        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return CLI_PARSE_HELP;
        }
        if (strcmp(argv[i], "--show-steps") == 0) {
            opts->show_steps = true;
            continue;
        }
        if (strcmp(argv[i], "--json") == 0) {
            opts->json = true;
            continue;
        }
        if (strcmp(argv[i], "--solve") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --solve requires an expression\n");
                return CLI_PARSE_ERROR;
            }
            opts->expr = argv[++i];
            continue;
        }
        if (strncmp(argv[i], "--solve=", 8) == 0) {
            opts->expr = argv[i] + 8;
            continue;
        }
        if (strcmp(argv[i], "--method") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --method requires a value\n");
                return CLI_PARSE_ERROR;
            }
            value = argv[++i];
        } else if (strncmp(argv[i], "--method=", 9) == 0) {
            value = argv[i] + 9;
        }

        if (value) {
            if (!parse_method_name(value, &opts->method)) {
                fprintf(stderr, "Error: unsupported method '%s'\n", value);
                return CLI_PARSE_ERROR;
            }
            continue;
        }

        fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
        return CLI_PARSE_ERROR;
    }

    if (!opts->expr || !opts->expr[0]) {
        fprintf(stderr, "Error: --solve is required\n");
        return CLI_PARSE_ERROR;
    }

    return CLI_PARSE_OK;
}

static bool detect_equals(const char *expr)
{
    const char *p = expr;
    bool in_parens = false;

    if (!expr) return false;
    while (*p) {
        if (*p == '(') in_parens = true;
        else if (*p == ')') in_parens = false;
        else if (*p == '=' && !in_parens) return true;
        p++;
    }
    return false;
}

static bool parse_expression(const char *input, parse_context_t *ctx, char *err, size_t err_size)
{
    tokenizer_t tokenizer;
    parser_t parser;
    char macro_err[80];
    ce_error_t tok_err;

    if (!input || !ctx) return false;
    memset(ctx, 0, sizeof(*ctx));
    if (err && err_size > 0u) err[0] = '\0';

    if (!macros_expand(input, ctx->expanded, sizeof(ctx->expanded), macro_err, sizeof(macro_err))) {
        if (err && err_size > 0u) {
            strncpy(err, macro_err, err_size - 1u);
            err[err_size - 1u] = '\0';
        }
        return false;
    }

    ctx->input_had_equals = detect_equals(ctx->expanded);

    mem_free_all();
    steps_clear();

    tok_init(&tokenizer, ctx->expanded);
    tok_err = tok_tokenize(&tokenizer);
    if (tok_err != CE_OK) {
        if (err && err_size > 0u) {
            snprintf(err, err_size, "Check your input for typos or missing operators");
        }
        return false;
    }

    parser_init(&parser, &tokenizer);
    ctx->root = parser_parse(&parser);
    if (!ctx->root) {
        if (err && err_size > 0u) {
            const char *parser_msg = parser_error_msg(&parser);
            snprintf(err, err_size, "%s",
                     (parser_msg && parser_msg[0]) ? parser_msg : "Could not understand expression");
        }
        return false;
    }

    ctx->domain = ce_classify_expression(ctx->root, ctx->input_had_equals);
    ctx->program_name = ce_domain_program(ctx->domain);
    return true;
}

static void print_json_step_array(void)
{
    int i;
    char escaped[256];

    printf("[");
    if (steps_count() > 0) {
        for (i = 0; i < steps_count(); i++) {
            json_escape(steps_get(i), escaped, sizeof(escaped));
            printf("%s\"%s\"", (i > 0) ? "," : "", escaped);
        }
    } else {
        for (i = 0; i < steps_checkpoint_count(); i++) {
            char line[256];
            const char *desc = steps_get_checkpoint(i);
            const char *result = steps_get_checkpoint_result(i);
            if (result && result[0]) {
                snprintf(line, sizeof(line), "%s -> %s", desc, result);
            } else {
                snprintf(line, sizeof(line), "%s", desc);
            }
            json_escape(line, escaped, sizeof(escaped));
            printf("%s\"%s\"", (i > 0) ? "," : "", escaped);
        }
    }
    printf("]");
}

static void print_json_checkpoints(void)
{
    int i;
    char escaped_desc[256];
    char escaped_result[256];

    printf("[");
    for (i = 0; i < steps_checkpoint_count(); i++) {
        json_escape(steps_get_checkpoint(i), escaped_desc, sizeof(escaped_desc));
        json_escape(steps_get_checkpoint_result(i), escaped_result, sizeof(escaped_result));
        printf("%s{\"description\":\"%s\",\"result\":\"%s\",\"start_step\":%d,\"end_step\":%d}",
               (i > 0) ? "," : "",
               escaped_desc,
               escaped_result,
               steps_get_checkpoint_step(i),
               steps_get_checkpoint_end_step(i));
    }
    printf("]");
}

static int emitted_step_count(bool show_steps)
{
    if (!show_steps) return 0;
    return steps_count() > 0 ? steps_count() : steps_checkpoint_count();
}

static void print_plain_steps(void)
{
    int i;

    if (steps_count() > 0) {
        for (i = 0; i < steps_count(); i++) {
            printf("%d. %s\n", i + 1, steps_get(i));
        }
        return;
    }

    for (i = 0; i < steps_checkpoint_count(); i++) {
        const char *desc = steps_get_checkpoint(i);
        const char *result = steps_get_checkpoint_result(i);
        if (result && result[0]) {
            printf("%d. %s -> %s\n", i + 1, desc, result);
        } else {
            printf("%d. %s\n", i + 1, desc);
        }
    }
}

int main(int argc, char **argv)
{
    cli_options_t opts;
    cli_parse_result_t parse_result;
    parse_context_t ctx;
    ce_response_t resp;
    char err[128];
    char escaped_expr[256];
    char escaped_result[1024];
    char escaped_error[512];
    char escaped_program[64];
    int exit_code = 0;

    macros_init();
    g_settings = k_default_settings;

    parse_result = parse_args(argc, argv, &opts);
    if (parse_result == CLI_PARSE_HELP) {
        return 0;
    }
    if (parse_result != CLI_PARSE_OK) {
        return 1;
    }

    g_settings.integ_method = opts.method;

    if (!parse_expression(opts.expr, &ctx, err, sizeof(err))) {
        if (opts.json) {
            json_escape(opts.expr ? opts.expr : "", escaped_expr, sizeof(escaped_expr));
            json_escape(err, escaped_error, sizeof(escaped_error));
            printf("{\"ok\":false,\"expression\":\"%s\",\"error\":\"%s\"}\n",
                   escaped_expr,
                   escaped_error);
        } else {
            fprintf(stderr, "%s\n", err);
        }
        return 1;
    }

    if (!ce_processor_run_preparsed(ctx.expanded, ctx.root, ctx.input_had_equals, &resp)) {
        if (!resp.error[0]) {
            strncpy(resp.error, "Processing failed", sizeof(resp.error) - 1u);
        }
    }

    if (!resp.ok) {
        exit_code = 1;
    }

    if (opts.json) {
        const int step_count = emitted_step_count(opts.show_steps);
        json_escape(opts.expr, escaped_expr, sizeof(escaped_expr));
        json_escape(resp.result, escaped_result, sizeof(escaped_result));
        json_escape(resp.error, escaped_error, sizeof(escaped_error));
        json_escape(ctx.program_name ? ctx.program_name : "", escaped_program, sizeof(escaped_program));
        printf("{");
        printf("\"ok\":%s,", resp.ok ? "true" : "false");
        printf("\"expression\":\"%s\",", escaped_expr);
        printf("\"domain\":\"%s\",", domain_name(ctx.domain));
        printf("\"program\":\"%s\",", escaped_program);
        printf("\"show_steps\":%s,", opts.show_steps ? "true" : "false");
        printf("\"result\":\"%s\",", escaped_result);
        printf("\"error\":\"%s\",", escaped_error);
        printf("\"step_count\":%d,", step_count);
        printf("\"steps\":");
        if (opts.show_steps) print_json_step_array();
        else printf("[]");
        printf(",\"checkpoints\":");
        if (opts.show_steps) print_json_checkpoints();
        else printf("[]");
        printf("}\n");
    } else if (resp.ok) {
        printf("%s\n", resp.result);
        if (opts.show_steps && (steps_count() > 0 || steps_checkpoint_count() > 0)) {
            printf("\nSteps:\n");
            print_plain_steps();
        }
    } else {
        fprintf(stderr, "%s\n", resp.error[0] ? resp.error : "Processing failed");
    }

    mem_free_all();
    steps_clear();
    return exit_code;
}
