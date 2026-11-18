/* ez80_cli.c -- Headless CLI driver for the eZ80/TI-84 CE emulator
 *
 * Usage:
 *   ez80_cli --rom <path> [--load <8xp_path>] [--keys <sequence>]
 *            [--run <cycles>] [--breakpoint <addr>] [--screenshot-text]
 *            [--screenshot-raw] [--screenshot-png <path>] [--screen-state]
 *            [--json] [--interrupts]
 *
 * Options:
 *   --rom <path>          Load ROM image (required)
 *   --load <path>         Load .8xp program file into emulator
 *   --archived            Load subsequent --load files as archived (default: RAM)
 *   --keys <sequence>     Inject key sequence (comma-separated hex codes)
 *   --run <cycles>        Execute N CPU cycles (default: 1000000)
 *   --cycles <cycles>     Alias for --run
 *   --breakpoint <addr>   Add breakpoint at hex address (can repeat)
 *   --screenshot-text     Dump LCD as ASCII art after execution
 *   --screenshot-raw      Dump LCD as raw RGB565 binary to stdout
 *   --screenshot-png      Write LCD framebuffer as PNG
 *   --screen-state        Dump structured screen/input/step state
 *   --list-programs       Dump loaded TI-OS program metadata
 *   --home-text           Print homescreen text buffer
 *   --dump-state          Print CPU register state
 *   --json                Output results as JSON
 *   --interrupts          Enable interrupt dispatch during execution
 *   --launch <name>       Launch named program after loading
 *   --os-version <M.m.p>  Set emulated OS version (e.g. 5.8.0)
 *   --quiet               Suppress banner and informational output
 *   --agent               Enter interactive JSON agent mode (stdin/stdout)
 *   --interactive         Alias for --agent
 *   --help                Show usage information
 */

#include "../include/ez80_emulator.h"
#include "../include/ez80_8xp.h"
#include "../include/ez80_rom.h"
#include "../include/ez80_agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLI_MAX_KEYS 256u
#define CLI_MAX_LOADS 16u
#define CLI_MAX_BREAKPOINTS 16u
#define CLI_MAX_LAUNCHES 8u
#define CLI_8XP_BUFFER_SIZE (512u * 1024u)

typedef struct {
    const char *rom_path;
    const char *load_paths[CLI_MAX_LOADS];
    bool load_archived[CLI_MAX_LOADS];
    uint32_t load_count;
    uint8_t keys[CLI_MAX_KEYS];
    uint32_t key_count;
    uint32_t breakpoints[CLI_MAX_BREAKPOINTS];
    uint32_t breakpoint_count;
    const char *launch_names[CLI_MAX_LAUNCHES];
    uint32_t launch_count;
    uint32_t run_cycles;
    bool screenshot_text;
    bool screenshot_raw;
    const char *screenshot_png_path;
    bool screen_state;
    bool list_programs;
    bool home_text;
    bool dump_state;
    bool json_output;
    bool interrupts;
    bool quiet;
    bool agent_mode;
    uint8_t os_major;
    uint8_t os_minor;
    uint8_t os_patch;
    bool os_version_set;
} cli_options_t;

static void print_usage(void)
{
    printf("ez80_cli -- Headless eZ80/TI-84 CE emulator CLI\n");
    printf("\n");
    printf("Usage:\n");
    printf("  ez80_cli --rom <path> [options]\n");
    printf("\n");
    printf("Options:\n");
    printf("  --rom <path>          Load ROM image (required)\n");
    printf("  --load <path>         Load .8xp file (repeatable)\n");
    printf("  --archived            Mark next --load as archived\n");
    printf("  --keys <hex,hex,...>  Inject key codes (hex, comma-separated)\n");
    printf("  --run <cycles>        CPU cycles to execute (default: 1000000)\n");
    printf("  --cycles <cycles>     Alias for --run\n");
    printf("  --breakpoint <hex>    Set breakpoint address (repeatable)\n");
    printf("  --screenshot-text     Dump LCD as ASCII art\n");
    printf("  --screenshot-raw      Dump LCD as raw RGB565 to stdout\n");
    printf("  --screenshot-png      Write LCD framebuffer as PNG\n");
    printf("  --screen-state        Dump structured screen/input state\n");
    printf("  --list-programs       Dump loaded program metadata\n");
    printf("  --home-text           Print homescreen text buffer\n");
    printf("  --dump-state          Print CPU register state\n");
    printf("  --json                JSON output mode\n");
    printf("  --interrupts          Enable interrupt dispatch\n");
    printf("  --launch <name>       Launch named program\n");
    printf("  --os-version <M.m.p>  Set OS version\n");
    printf("  --quiet               Suppress informational output\n");
    printf("  --agent               Interactive JSON agent mode (stdin/stdout)\n");
    printf("  --interactive         Alias for --agent\n");
    printf("  --help                Show this help\n");
}

static int parse_hex(const char *str, uint32_t *out)
{
    char *end = NULL;
    unsigned long val = strtoul(str, &end, 16);
    if (!end || end == str) return 0;
    *out = (uint32_t)val;
    return 1;
}

static int parse_keys(const char *str, uint8_t *out_keys, uint32_t *out_count, uint32_t max_keys)
{
    const char *p = str;
    uint32_t count = 0u;

    while (*p && count < max_keys) {
        char *end = NULL;
        unsigned long val;

        /* Skip leading whitespace and commas */
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        val = strtoul(p, &end, 16);
        if (end == p) {
            fprintf(stderr, "Error: invalid key code at '%s'\n", p);
            return 0;
        }
        if (val > 0xFFu) {
            fprintf(stderr, "Error: key code 0x%lx out of range\n", val);
            return 0;
        }

        out_keys[count++] = (uint8_t)val;
        p = end;
    }

    *out_count = count;
    return 1;
}

static int parse_os_version(const char *str, uint8_t *major, uint8_t *minor, uint8_t *patch)
{
    int ma = 0, mi = 0, pa = 0;
    int n = sscanf(str, "%d.%d.%d", &ma, &mi, &pa);
    if (n < 2) {
        fprintf(stderr, "Error: invalid OS version '%s' (expected M.m or M.m.p)\n", str);
        return 0;
    }
    *major = (uint8_t)ma;
    *minor = (uint8_t)mi;
    *patch = (n >= 3) ? (uint8_t)pa : 0u;
    return 1;
}

static int parse_args(int argc, char **argv, cli_options_t *opts)
{
    int i;
    bool next_archived = false;

    memset(opts, 0, sizeof(*opts));
    opts->run_cycles = 1000000u;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return -1;
        } else if (strcmp(argv[i], "--rom") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --rom requires a path\n"); return 0; }
            opts->rom_path = argv[i];
        } else if (strcmp(argv[i], "--load") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --load requires a path\n"); return 0; }
            if (opts->load_count >= CLI_MAX_LOADS) {
                fprintf(stderr, "Error: too many --load files (max %u)\n", CLI_MAX_LOADS);
                return 0;
            }
            opts->load_paths[opts->load_count] = argv[i];
            opts->load_archived[opts->load_count] = next_archived;
            opts->load_count++;
            next_archived = false;
        } else if (strcmp(argv[i], "--archived") == 0) {
            next_archived = true;
        } else if (strcmp(argv[i], "--keys") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --keys requires arguments\n"); return 0; }
            if (!parse_keys(argv[i], opts->keys, &opts->key_count, CLI_MAX_KEYS)) return 0;
        } else if (strcmp(argv[i], "--run") == 0 || strcmp(argv[i], "--cycles") == 0) {
            const char *flag = argv[i];
            if (++i >= argc) { fprintf(stderr, "Error: %s requires a number\n", flag); return 0; }
            opts->run_cycles = (uint32_t)strtoul(argv[i], NULL, 0);
        } else if (strcmp(argv[i], "--breakpoint") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --breakpoint requires an address\n"); return 0; }
            if (opts->breakpoint_count >= CLI_MAX_BREAKPOINTS) {
                fprintf(stderr, "Error: too many breakpoints (max %u)\n", CLI_MAX_BREAKPOINTS);
                return 0;
            }
            if (!parse_hex(argv[i], &opts->breakpoints[opts->breakpoint_count])) {
                fprintf(stderr, "Error: invalid breakpoint address '%s'\n", argv[i]);
                return 0;
            }
            opts->breakpoint_count++;
        } else if (strcmp(argv[i], "--screenshot-text") == 0) {
            opts->screenshot_text = true;
        } else if (strcmp(argv[i], "--screenshot-raw") == 0) {
            opts->screenshot_raw = true;
        } else if (strcmp(argv[i], "--screenshot-png") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --screenshot-png requires a path\n"); return 0; }
            opts->screenshot_png_path = argv[i];
        } else if (strcmp(argv[i], "--screen-state") == 0) {
            opts->screen_state = true;
        } else if (strcmp(argv[i], "--list-programs") == 0) {
            opts->list_programs = true;
        } else if (strcmp(argv[i], "--home-text") == 0) {
            opts->home_text = true;
        } else if (strcmp(argv[i], "--dump-state") == 0) {
            opts->dump_state = true;
        } else if (strcmp(argv[i], "--json") == 0) {
            opts->json_output = true;
        } else if (strcmp(argv[i], "--interrupts") == 0) {
            opts->interrupts = true;
        } else if (strcmp(argv[i], "--launch") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --launch requires a name\n"); return 0; }
            if (opts->launch_count >= CLI_MAX_LAUNCHES) {
                fprintf(stderr, "Error: too many --launch names (max %u)\n", CLI_MAX_LAUNCHES);
                return 0;
            }
            opts->launch_names[opts->launch_count] = argv[i];
            opts->launch_count++;
        } else if (strcmp(argv[i], "--os-version") == 0) {
            if (++i >= argc) { fprintf(stderr, "Error: --os-version requires M.m.p\n"); return 0; }
            if (!parse_os_version(argv[i], &opts->os_major, &opts->os_minor, &opts->os_patch)) return 0;
            opts->os_version_set = true;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            opts->quiet = true;
        } else if (strcmp(argv[i], "--agent") == 0 || strcmp(argv[i], "--interactive") == 0) {
            opts->agent_mode = true;
        } else {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            return 0;
        }
    }

    /* Agent mode does not require --rom (can be loaded via command) */
    if (!opts->rom_path && !opts->agent_mode) {
        fprintf(stderr, "Error: --rom is required (or use --agent mode)\n");
        return 0;
    }

    return 1;
}

static void escape_json_string(const char *src, char *dst, size_t dst_size);

static void print_cpu_state(const ez80_cpu_state_t *cpu, bool json)
{
    if (json) {
        printf("  \"cpu\": {\n");
        printf("    \"pc\": \"0x%06X\",\n", cpu->pc & 0xFFFFFFu);
        printf("    \"sp\": \"0x%06X\",\n", cpu->sp & 0xFFFFFFu);
        printf("    \"af\": \"0x%04X\",\n", cpu->af & 0xFFFFu);
        printf("    \"bc\": \"0x%06X\",\n", cpu->bc & 0xFFFFFFu);
        printf("    \"de\": \"0x%06X\",\n", cpu->de & 0xFFFFFFu);
        printf("    \"hl\": \"0x%06X\",\n", cpu->hl & 0xFFFFFFu);
        printf("    \"ix\": \"0x%06X\",\n", cpu->ix & 0xFFFFFFu);
        printf("    \"iy\": \"0x%06X\",\n", cpu->iy & 0xFFFFFFu);
        printf("    \"i\": \"0x%04X\",\n", cpu->i_reg16);
        printf("    \"r\": \"0x%02X\",\n", cpu->r_reg);
        printf("    \"mbase\": \"0x%02X\",\n", cpu->mbase_reg);
        printf("    \"im\": %u,\n", cpu->interrupt_mode);
        printf("    \"iff1\": %s,\n", cpu->iff1 ? "true" : "false");
        printf("    \"iff2\": %s,\n", cpu->iff2 ? "true" : "false");
        printf("    \"adl\": %s,\n", cpu->adl_mode ? "true" : "false");
        printf("    \"halted\": %s\n", cpu->halted ? "true" : "false");
        printf("  }");
    } else {
        printf("CPU State:\n");
        printf("  PC=%06X  SP=%06X  AF=%04X  BC=%06X\n",
               cpu->pc & 0xFFFFFFu, cpu->sp & 0xFFFFFFu,
               cpu->af & 0xFFFFu, cpu->bc & 0xFFFFFFu);
        printf("  DE=%06X  HL=%06X  IX=%06X  IY=%06X\n",
               cpu->de & 0xFFFFFFu, cpu->hl & 0xFFFFFFu,
               cpu->ix & 0xFFFFFFu, cpu->iy & 0xFFFFFFu);
        printf("  I=%04X  R=%02X  MBASE=%02X  IM=%u\n",
               cpu->i_reg16, cpu->r_reg, cpu->mbase_reg, cpu->interrupt_mode);
        printf("  IFF1=%u  IFF2=%u  ADL=%u  HALTED=%u\n",
               cpu->iff1 ? 1u : 0u, cpu->iff2 ? 1u : 0u,
               cpu->adl_mode ? 1u : 0u, cpu->halted ? 1u : 0u);
    }
}

static void print_home_text(const ez80_emulator_t *emu, bool json)
{
    uint8_t row;
    char line[EZ80_HOME_COLS + 1u];

    if (json) {
        printf("  \"homescreen\": [\n");
    } else {
        printf("Homescreen:\n");
    }

    for (row = 0; row < EZ80_HOME_ROWS; row++) {
        ez80_status_t st = ez80_emulator_home_get_line(emu, row, line, sizeof(line));
        if (st != EZ80_OK) {
            memset(line, ' ', EZ80_HOME_COLS);
            line[EZ80_HOME_COLS] = '\0';
        }
        if (json) {
            printf("    \"%s\"%s\n", line, (row + 1u < EZ80_HOME_ROWS) ? "," : "");
        } else {
            printf("  |%s|\n", line);
        }
    }

    if (json) {
        printf("  ]");
    }
}

static void print_json_string_array(const char values[][EZ80_HOME_COLS + 1u], uint32_t count)
{
    uint32_t i;
    char escaped[256];

    printf("[");
    for (i = 0u; i < count; i++) {
        escape_json_string(values[i], escaped, sizeof(escaped));
        printf("%s\"%s\"", (i > 0u) ? "," : "", escaped);
    }
    printf("]");
}

static void print_screen_state(const ez80_emulator_t *emu, bool json)
{
    ez80_screen_text_state_t state;
    ez80_status_t st;
    char escaped_input[256];
    char escaped_ascii[4096];

    st = ez80_emulator_get_screen_text_state(emu, &state);
    if (st != EZ80_OK) {
        if (json) {
            printf("  \"screen_error\": \"%s\"", ez80_status_string(st));
        } else {
            printf("Screen state error: %s\n", ez80_status_string(st));
        }
        return;
    }

    escape_json_string(state.input_line, escaped_input, sizeof(escaped_input));
    escape_json_string(state.ascii, escaped_ascii, sizeof(escaped_ascii));

    if (json) {
        printf("  \"screen\": {\n");
        printf("    \"homescreen_active\": %s,\n", state.homescreen_active ? "true" : "false");
        printf("    \"cursor_row\": %u,\n", state.cursor_row);
        printf("    \"cursor_col\": %u,\n", state.cursor_col);
        printf("    \"input_line\": \"%s\",\n", escaped_input);
        printf("    \"visible_line_count\": %u,\n", state.visible_line_count);
        printf("    \"step_line_count\": %u,\n", state.step_line_count);
        printf("    \"visible_lines\": ");
        print_json_string_array(state.visible_lines, state.visible_line_count);
        printf(",\n");
        printf("    \"step_lines\": ");
        print_json_string_array(state.step_lines, state.step_line_count);
        printf(",\n");
        printf("    \"ascii\": \"%s\",\n", escaped_ascii);
        printf("    \"active_program\": \"%s\",\n", emu->os.active_program_name);
        printf("    \"active_program_ram\": \"0x%06X\"\n", emu->os.active_program_ram_addr);
        printf("  }");
    } else {
        uint32_t row;
        printf("Screen State:\n");
        printf("  homescreen_active=%u cursor=(%u,%u)\n",
               state.homescreen_active ? 1u : 0u,
               state.cursor_row,
               state.cursor_col);
        printf("  input: %s\n", state.input_line);
        printf("  active_program=%s ram=0x%06X\n",
               emu->os.active_program_name,
               emu->os.active_program_ram_addr);
        printf("  steps:\n");
        for (row = 0u; row < state.step_line_count; row++) {
            printf("    [%02u] %s\n", row, state.step_lines[row]);
        }
        printf("  visible_lines:\n");
        for (row = 0u; row < state.visible_line_count; row++) {
            printf("    [%02u] %s\n", row, state.visible_lines[row]);
        }
        printf("  ascii:\n%s", state.ascii);
    }
}

static void print_program_list(const ez80_emulator_t *emu, bool json)
{
    uint32_t i;
    uint32_t count = 0u;

    if (json) {
        printf("  \"program_count\": ");
        for (i = 0u; i < EZ80_OS_MAX_VARS; i++) {
            const ez80_os_var_slot_t *slot = &emu->os.vars[i];
            if (slot->in_use && slot->type == EZ80_OS_VAR_PROGRAM) count++;
        }
        printf("%lu,\n", (unsigned long)count);
        printf("  \"programs\": [");
        count = 0u;
        for (i = 0u; i < EZ80_OS_MAX_VARS; i++) {
            const ez80_os_var_slot_t *slot = &emu->os.vars[i];
            if (!slot->in_use || slot->type != EZ80_OS_VAR_PROGRAM) continue;

            printf("%s{\"name\":\"%s\",\"archived\":%s,\"loaded_in_ram\":%s,"
                   "\"size_bytes\":%lu,\"archive_addr\":\"0x%06X\","
                   "\"ram_addr\":\"0x%06X\",\"active\":%s}",
                   (count > 0u) ? "," : "",
                   slot->name,
                   slot->archived ? "true" : "false",
                   slot->loaded_in_ram ? "true" : "false",
                   (unsigned long)slot->size_bytes,
                   slot->archive_addr,
                   slot->ram_addr,
                   (emu->os.active_program_name[0] != '\0' &&
                    strncmp(emu->os.active_program_name,
                            slot->name,
                            EZ80_OS_VAR_NAME_MAX + 1u) == 0) ? "true" : "false");
            count++;
        }
        printf("]");
        return;
    }

    printf("Programs:\n");
    for (i = 0u; i < EZ80_OS_MAX_VARS; i++) {
        const ez80_os_var_slot_t *slot = &emu->os.vars[i];
        if (!slot->in_use || slot->type != EZ80_OS_VAR_PROGRAM) continue;

        printf("  [%02lu] %s archived=%u loaded_in_ram=%u size=%lu archive=0x%06X ram=0x%06X%s\n",
               (unsigned long)count,
               slot->name,
               slot->archived ? 1u : 0u,
               slot->loaded_in_ram ? 1u : 0u,
               (unsigned long)slot->size_bytes,
               slot->archive_addr,
               slot->ram_addr,
               (emu->os.active_program_name[0] != '\0' &&
                strncmp(emu->os.active_program_name,
                        slot->name,
                        EZ80_OS_VAR_NAME_MAX + 1u) == 0) ? " active" : "");
        count++;
    }
    if (count == 0u) {
        printf("  (none)\n");
    }
}

static void escape_json_string(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0u;
    const char *p = src;
    while (*p && di + 6u < dst_size) {
        if (*p == '\\') { dst[di++] = '\\'; dst[di++] = '\\'; }
        else if (*p == '"') { dst[di++] = '\\'; dst[di++] = '"'; }
        else if (*p == '\n') { dst[di++] = '\\'; dst[di++] = 'n'; }
        else if (*p == '\r') { dst[di++] = '\\'; dst[di++] = 'r'; }
        else if (*p == '\t') { dst[di++] = '\\'; dst[di++] = 't'; }
        else { dst[di++] = *p; }
        p++;
    }
    dst[di] = '\0';
}

int main(int argc, char **argv)
{
    cli_options_t opts;
    ez80_emulator_t emu;
    ez80_status_t status;
    ez80_run_result_t run_result;
    ez80_cpu_state_t cpu_state;
    int parse_result;
    uint32_t i;

    /* Static buffer for .8xp file loading */
    static uint8_t file_buffer[CLI_8XP_BUFFER_SIZE];

    parse_result = parse_args(argc, argv, &opts);
    if (parse_result < 0) return 0;  /* --help */
    if (parse_result == 0) return 1;  /* error */

    /* Agent mode: interactive JSON command loop */
    if (opts.agent_mode) {
        agent_session_t session;
        status = ez80_emulator_init(&emu);
        if (status != EZ80_OK) {
            fprintf(stderr, "Error: emulator init failed: %s\n",
                    ez80_status_string(status));
            return 1;
        }
        /* If ROM was also specified, pre-load it */
        if (opts.rom_path) {
            status = ez80_emulator_load_rom(&emu, opts.rom_path);
            if (status != EZ80_OK) {
                fprintf(stderr, "Error: load ROM failed: %s\n",
                        ez80_status_string(status));
                return 1;
            }
        }
        agent_session_init(&session, &emu, stdin, stdout);
        agent_run_loop(&session);
        return 0;
    }

    /* Initialize emulator */
    status = ez80_emulator_init(&emu);
    if (status != EZ80_OK) {
        fprintf(stderr, "Error: emulator init failed: %s\n", ez80_status_string(status));
        return 1;
    }

    /* Set OS version if specified */
    if (opts.os_version_set) {
        status = ez80_emulator_set_os_version(&emu, opts.os_major, opts.os_minor, opts.os_patch);
        if (status != EZ80_OK) {
            fprintf(stderr, "Error: set OS version failed: %s\n", ez80_status_string(status));
            return 1;
        }
    }

    /* Load ROM */
    status = ez80_emulator_load_rom(&emu, opts.rom_path);
    if (status != EZ80_OK) {
        fprintf(stderr, "Error: load ROM failed: %s\n", ez80_status_string(status));
        return 1;
    }

    if (!opts.quiet) {
        fprintf(stderr, "ROM loaded: %s (%lu bytes)\n",
                opts.rom_path,
                (unsigned long)emu.memory.flash_bytes_loaded);
    }

    /* Enable interrupts if requested */
    if (opts.interrupts) {
        ez80_emulator_enable_interrupts(&emu, true);
    }

    /* Add breakpoints */
    for (i = 0u; i < opts.breakpoint_count; i++) {
        status = ez80_emulator_add_breakpoint(&emu, opts.breakpoints[i]);
        if (status != EZ80_OK) {
            fprintf(stderr, "Error: add breakpoint 0x%06X failed: %s\n",
                    opts.breakpoints[i], ez80_status_string(status));
            return 1;
        }
        if (!opts.quiet) {
            fprintf(stderr, "Breakpoint set at 0x%06X\n", opts.breakpoints[i]);
        }
    }

    /* Load .8xp files */
    for (i = 0u; i < opts.load_count; i++) {
        size_t bytes_read = 0u;
        ez80_8xp_parsed_t parsed;
        uint32_t vi;

        status = ez80_8xp_load_file(opts.load_paths[i], file_buffer, CLI_8XP_BUFFER_SIZE, &bytes_read);
        if (status != EZ80_OK) {
            fprintf(stderr, "Error: load file '%s' failed: %s\n",
                    opts.load_paths[i], ez80_status_string(status));
            return 1;
        }

        status = ez80_8xp_parse(file_buffer, bytes_read, &parsed);
        if (status != EZ80_OK) {
            fprintf(stderr, "Error: parse '%s' failed: %s\n",
                    opts.load_paths[i], ez80_status_string(status));
            return 1;
        }

        if (!opts.quiet) {
            fprintf(stderr, "Loaded: %s (%lu bytes, %u entries, checksum %s)\n",
                    opts.load_paths[i],
                    (unsigned long)bytes_read,
                    parsed.var_entry_count,
                    parsed.checksum_valid ? "OK" : "MISMATCH");
            for (vi = 0u; vi < parsed.var_entry_count; vi++) {
                fprintf(stderr, "  [%u] name='%s' type=%s size=%u\n",
                        vi,
                        parsed.entries[vi].name,
                        ez80_8xp_type_name(parsed.entries[vi].type_id),
                        parsed.entries[vi].program_data_size);
            }
        }

        /* Load all variable entries into emulator */
        for (vi = 0u; vi < parsed.var_entry_count; vi++) {
            status = ez80_8xp_load_into_emulator(&emu, &parsed, vi, opts.load_archived[i]);
            if (status != EZ80_OK) {
                fprintf(stderr, "Error: load entry '%s' into emulator failed: %s\n",
                        parsed.entries[vi].name, ez80_status_string(status));
                return 1;
            }
        }
    }

    /* Launch programs if requested */
    for (i = 0u; i < opts.launch_count; i++) {
        uint32_t ram_addr = 0u;
        status = ez80_emulator_launch_program(&emu, opts.launch_names[i], &ram_addr);
        if (status != EZ80_OK) {
            fprintf(stderr, "Error: launch '%s' failed: %s\n",
                    opts.launch_names[i], ez80_status_string(status));
            return 1;
        }
        if (!opts.quiet) {
            fprintf(stderr, "Launched '%s' at RAM 0x%06X\n", opts.launch_names[i], ram_addr);
        }
    }

    /* Inject keys */
    for (i = 0u; i < opts.key_count; i++) {
        status = ez80_emulator_inject_key(&emu, opts.keys[i]);
        if (status != EZ80_OK) {
            fprintf(stderr, "Error: inject key 0x%02X failed: %s\n",
                    opts.keys[i], ez80_status_string(status));
            return 1;
        }
    }

    /* Run CPU */
    memset(&run_result, 0, sizeof(run_result));
    status = ez80_emulator_run_cycles(&emu, opts.run_cycles, &run_result);
    if (status != EZ80_OK) {
        fprintf(stderr, "Error: run failed after %lu cycles: %s\n",
                (unsigned long)run_result.cycles_executed, ez80_status_string(status));
        return 1;
    }

    /* Get final CPU state */
    ez80_emulator_get_state(&emu, &cpu_state);

    if (opts.screenshot_png_path) {
        status = ez80_emulator_lcd_write_png(&emu, opts.screenshot_png_path);
        if (status != EZ80_OK) {
            fprintf(stderr, "Error: write PNG screenshot failed: %s\n", ez80_status_string(status));
            return 1;
        }
    }

    /* Output results */
    if (opts.json_output) {
        printf("{\n");
        printf("  \"status\": \"ok\",\n");
        printf("  \"cycles_executed\": %lu,\n", (unsigned long)run_result.cycles_executed);
        printf("  \"hit_breakpoint\": %s,\n", run_result.hit_breakpoint ? "true" : "false");
        printf("  \"hit_halt\": %s,\n", run_result.hit_halt ? "true" : "false");
        printf("  \"interrupts_serviced\": %lu", (unsigned long)emu.hw.total_interrupts_serviced);

        if (opts.dump_state) {
            printf(",\n");
            print_cpu_state(&cpu_state, true);
        }
        if (opts.home_text) {
            printf(",\n");
            print_home_text(&emu, true);
        }
        if (opts.screen_state) {
            printf(",\n");
            print_screen_state(&emu, true);
        }
        if (opts.list_programs) {
            printf(",\n");
            print_program_list(&emu, true);
        }
        if (opts.screenshot_text) {
            char ascii_buf[2048];
            uint32_t ascii_len = 0u;
            char escaped[4096];
            status = ez80_emulator_lcd_to_ascii(&emu, ascii_buf, sizeof(ascii_buf), &ascii_len);
            if (status == EZ80_OK) {
                escape_json_string(ascii_buf, escaped, sizeof(escaped));
                printf(",\n  \"lcd_ascii\": \"%s\"", escaped);
            }
        }
        if (opts.screenshot_png_path) {
            char escaped_path[512];
            escape_json_string(opts.screenshot_png_path, escaped_path, sizeof(escaped_path));
            printf(",\n  \"lcd_png_path\": \"%s\"", escaped_path);
        }
        printf("\n}\n");
    } else {
        if (!opts.quiet) {
            printf("Execution complete: %lu cycles",
                   (unsigned long)run_result.cycles_executed);
            if (run_result.hit_breakpoint) printf(", hit breakpoint");
            if (run_result.hit_halt) printf(", halted");
            if (emu.hw.total_interrupts_serviced > 0u) {
                printf(", %lu interrupts serviced",
                       (unsigned long)emu.hw.total_interrupts_serviced);
            }
            printf("\n");
        }

        if (opts.dump_state) {
            print_cpu_state(&cpu_state, false);
        }
        if (opts.home_text) {
            print_home_text(&emu, false);
        }
        if (opts.screen_state) {
            print_screen_state(&emu, false);
        }
        if (opts.list_programs) {
            print_program_list(&emu, false);
        }
        if (opts.screenshot_text) {
            char ascii_buf[2048];
            uint32_t ascii_len = 0u;
            status = ez80_emulator_lcd_to_ascii(&emu, ascii_buf, sizeof(ascii_buf), &ascii_len);
            if (status == EZ80_OK) {
                printf("LCD ASCII:\n%s", ascii_buf);
            }
        }
    }
    if (opts.screenshot_png_path && !opts.quiet && !opts.json_output) {
        printf("LCD PNG: %s\n", opts.screenshot_png_path);
    }

    /* Raw screenshot to stdout (binary) */
    if (opts.screenshot_raw) {
        uint16_t fb[320u * 240u];
        status = ez80_emulator_lcd_get_framebuffer(&emu, fb, 320u * 240u);
        if (status == EZ80_OK) {
            fwrite(fb, sizeof(uint16_t), 320u * 240u, stdout);
        }
    }

    return 0;
}
