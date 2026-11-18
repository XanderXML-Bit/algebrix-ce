/*
 * ez80_agent.c — Structured agent interface for the eZ80 emulator
 *
 * JSON-based command/response protocol for programmatic interaction.
 * Each command is a single-line JSON object; each response is a single-line
 * JSON object terminated by newline.
 *
 * Uses a lightweight hand-rolled JSON parser (no external dependencies).
 */
#include "../include/ez80_agent.h"
#include "../include/ez80_emulator.h"
#include "../include/ez80_8xp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define AGENT_8XP_BUFFER_SIZE (512u * 1024u)

static uint8_t g_agent_8xp_buffer[AGENT_8XP_BUFFER_SIZE];
static uint16_t g_agent_lcd_framebuffer[EZ80_GRAPH_WIDTH * EZ80_GRAPH_HEIGHT];
static ez80_snapshot_t g_agent_snapshot_storage;

/* ── Command name table ────────────────────────────────────── */
typedef struct {
    const char *name;
    agent_cmd_type_t type;
} cmd_entry_t;

static const cmd_entry_t cmd_table[] = {
    { "init",                AGENT_CMD_INIT },
    { "reset",               AGENT_CMD_RESET },
    { "load_rom",            AGENT_CMD_LOAD_ROM },
    { "set_rom_loaded",      AGENT_CMD_SET_ROM_LOADED },
    { "load_8xp",           AGENT_CMD_LOAD_8XP },
    { "inject_key",          AGENT_CMD_INJECT_KEY },
    { "release_key",         AGENT_CMD_RELEASE_KEY },
    { "release_all_keys",    AGENT_CMD_RELEASE_ALL_KEYS },
    { "run",                 AGENT_CMD_RUN },
    { "step",                AGENT_CMD_STEP },
    { "get_state",           AGENT_CMD_GET_STATE },
    { "read_memory",         AGENT_CMD_READ_MEMORY },
    { "write_memory",        AGENT_CMD_WRITE_MEMORY },
    { "read_port",           AGENT_CMD_READ_PORT },
    { "write_port",          AGENT_CMD_WRITE_PORT },
    { "add_breakpoint",      AGENT_CMD_ADD_BREAKPOINT },
    { "remove_breakpoint",   AGENT_CMD_REMOVE_BREAKPOINT },
    { "clear_breakpoints",   AGENT_CMD_CLEAR_BREAKPOINTS },
    { "add_watchpoint",      AGENT_CMD_ADD_WATCHPOINT },
    { "remove_watchpoint",   AGENT_CMD_REMOVE_WATCHPOINT },
    { "clear_watchpoints",   AGENT_CMD_CLEAR_WATCHPOINTS },
    { "get_lcd_text",        AGENT_CMD_GET_LCD_TEXT },
    { "get_lcd_raw",         AGENT_CMD_GET_LCD_RAW },
    { "get_screen_state",    AGENT_CMD_GET_SCREEN_STATE },
    { "write_lcd_png",       AGENT_CMD_WRITE_LCD_PNG },
    { "get_homescreen",      AGENT_CMD_GET_HOMESCREEN },
    { "list_programs",       AGENT_CMD_LIST_PROGRAMS },
    { "launch_program",      AGENT_CMD_LAUNCH_PROGRAM },
    { "finish_program",      AGENT_CMD_FINISH_PROGRAM },
    { "set_os_version",      AGENT_CMD_SET_OS_VERSION },
    { "enable_interrupts",   AGENT_CMD_ENABLE_INTERRUPTS },
    { "snapshot_save",       AGENT_CMD_SNAPSHOT_SAVE },
    { "snapshot_restore",    AGENT_CMD_SNAPSHOT_RESTORE },
    { "trace_enable",        AGENT_CMD_TRACE_ENABLE },
    { "trace_get",           AGENT_CMD_TRACE_GET },
    { "ping",                AGENT_CMD_PING },
    { "quit",                AGENT_CMD_QUIT },
    { "help",                AGENT_CMD_HELP },
    { NULL, AGENT_CMD_UNKNOWN }
};

const char *agent_cmd_type_name(agent_cmd_type_t type)
{
    const cmd_entry_t *e;
    for (e = cmd_table; e->name; e++) {
        if (e->type == type) return e->name;
    }
    return "unknown";
}

/* ── Minimal JSON parser helpers ───────────────────────────── */

static const char *skip_ws(const char *p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *parse_json_string(const char *p, char *out, int out_size)
{
    int i = 0;
    if (*p != '"') return NULL;
    p++;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case '"': out[i++] = '"'; break;
                case '\\': out[i++] = '\\'; break;
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    if (*p == '"') p++;
    return p;
}

static const char *parse_json_int(const char *p, uint32_t *out)
{
    char *end = NULL;
    unsigned long val;

    if (*p == '0' && (*(p + 1) == 'x' || *(p + 1) == 'X')) {
        val = strtoul(p, &end, 16);
    } else {
        val = strtoul(p, &end, 10);
    }
    if (end == p) return NULL;
    *out = (uint32_t)val;
    return end;
}

/* ── Parse JSON command ────────────────────────────────────── */

bool agent_parse_command(const char *json_line, agent_command_t *cmd)
{
    const char *p;
    char key[64];
    char val_str[256];

    memset(cmd, 0, sizeof(*cmd));
    cmd->type = AGENT_CMD_UNKNOWN;

    p = skip_ws(json_line);
    if (*p != '{') return false;
    p++;

    while (*p && *p != '}') {
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') break;

        /* Parse key */
        p = parse_json_string(p, key, sizeof(key));
        if (!p) return false;
        p = skip_ws(p);
        if (*p != ':') return false;
        p++;
        p = skip_ws(p);

        /* Parse value */
        if (strcmp(key, "cmd") == 0 || strcmp(key, "command") == 0) {
            p = parse_json_string(p, val_str, sizeof(val_str));
            if (!p) return false;
            /* Look up command type */
            const cmd_entry_t *e;
            for (e = cmd_table; e->name; e++) {
                if (strcmp(e->name, val_str) == 0) {
                    cmd->type = e->type;
                    break;
                }
            }
        } else if (strcmp(key, "id") == 0) {
            p = parse_json_int(p, &cmd->id);
            if (!p) return false;
        } else if (strcmp(key, "path") == 0 || strcmp(key, "name") == 0 ||
                   strcmp(key, "rom") == 0 || strcmp(key, "file") == 0) {
            if (cmd->str_arg_count < AGENT_MAX_ARGS) {
                p = parse_json_string(p, cmd->str_args[cmd->str_arg_count], 256);
                if (!p) return false;
                cmd->str_arg_count++;
            }
        } else if (strcmp(key, "addr") == 0 || strcmp(key, "address") == 0 ||
                   strcmp(key, "value") == 0 || strcmp(key, "cycles") == 0 ||
                   strcmp(key, "length") == 0 || strcmp(key, "key") == 0 ||
                   strcmp(key, "port") == 0 || strcmp(key, "enable") == 0 ||
                   strcmp(key, "type") == 0 || strcmp(key, "major") == 0 ||
                   strcmp(key, "minor") == 0 || strcmp(key, "patch") == 0 ||
                   strcmp(key, "loaded") == 0 ||
                   strcmp(key, "archived") == 0) {
            if (cmd->int_arg_count < AGENT_MAX_ARGS) {
                if (*p == '"') {
                    /* Handle hex strings like "0xD00000" */
                    p = parse_json_string(p, val_str, sizeof(val_str));
                    if (!p) return false;
                    cmd->int_args[cmd->int_arg_count] = (uint32_t)strtoul(val_str, NULL, 0);
                } else if (*p == 't' && strncmp(p, "true", 4) == 0) {
                    cmd->int_args[cmd->int_arg_count] = 1;
                    p += 4;
                } else if (*p == 'f' && strncmp(p, "false", 5) == 0) {
                    cmd->int_args[cmd->int_arg_count] = 0;
                    p += 5;
                } else {
                    p = parse_json_int(p, &cmd->int_args[cmd->int_arg_count]);
                    if (!p) return false;
                }
                cmd->int_arg_count++;
            }
        } else {
            /* Skip unknown value */
            if (*p == '"') {
                p = parse_json_string(p, val_str, sizeof(val_str));
                if (!p) return false;
            } else if (*p == 't') { p += 4; }
            else if (*p == 'f') { p += 5; }
            else if (*p == 'n') { p += 4; }
            else {
                uint32_t dummy;
                p = parse_json_int(p, &dummy);
                if (!p) return false;
            }
        }
    }

    if (*p != '}') return false;
    p++;
    p = skip_ws(p);
    return *p == '\0';
}

/* ── JSON output helpers ───────────────────────────────────── */

static void escape_json(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0u;
    while (*src && di + 6u < dst_size) {
        switch (*src) {
            case '\\': dst[di++] = '\\'; dst[di++] = '\\'; break;
            case '"': dst[di++] = '\\'; dst[di++] = '"'; break;
            case '\n': dst[di++] = '\\'; dst[di++] = 'n'; break;
            case '\r': dst[di++] = '\\'; dst[di++] = 'r'; break;
            case '\t': dst[di++] = '\\'; dst[di++] = 't'; break;
            default: dst[di++] = *src; break;
        }
        src++;
    }
    dst[di] = '\0';
}

static void write_response_ok(agent_session_t *session, uint32_t cmd_id,
                               const char *data)
{
    fprintf(session->output, "{\"id\":%u,\"status\":\"ok\"", cmd_id);
    if (data && *data) {
        fprintf(session->output, ",%s", data);
    }
    fprintf(session->output, "}\n");
    fflush(session->output);
}

static void write_response_error(agent_session_t *session, uint32_t cmd_id,
                                  const char *msg)
{
    char escaped[512];
    escape_json(msg, escaped, sizeof(escaped));
    fprintf(session->output,
            "{\"id\":%u,\"status\":\"error\",\"message\":\"%s\"}\n",
            cmd_id, escaped);
    fflush(session->output);
}

/* ── Format helpers ────────────────────────────────────────── */

int agent_format_cpu_state(const ez80_cpu_state_t *cpu, char *buf, int buf_size)
{
    return snprintf(buf, (size_t)buf_size,
        "\"cpu\":{"
        "\"pc\":\"0x%06X\","
        "\"sp\":\"0x%06X\","
        "\"af\":\"0x%04X\","
        "\"bc\":\"0x%06X\","
        "\"de\":\"0x%06X\","
        "\"hl\":\"0x%06X\","
        "\"ix\":\"0x%06X\","
        "\"iy\":\"0x%06X\","
        "\"i\":\"0x%04X\","
        "\"r\":\"0x%02X\","
        "\"mbase\":\"0x%02X\","
        "\"im\":%u,"
        "\"iff1\":%s,"
        "\"iff2\":%s,"
        "\"adl\":%s,"
        "\"halted\":%s"
        "}",
        cpu->pc & 0xFFFFFFu,
        cpu->sp & 0xFFFFFFu,
        cpu->af & 0xFFFFu,
        cpu->bc & 0xFFFFFFu,
        cpu->de & 0xFFFFFFu,
        cpu->hl & 0xFFFFFFu,
        cpu->ix & 0xFFFFFFu,
        cpu->iy & 0xFFFFFFu,
        cpu->i_reg16,
        cpu->r_reg,
        cpu->mbase_reg,
        cpu->interrupt_mode,
        cpu->iff1 ? "true" : "false",
        cpu->iff2 ? "true" : "false",
        cpu->adl_mode ? "true" : "false",
        cpu->halted ? "true" : "false");
}

int agent_format_memory_hex(const uint8_t *data, uint32_t length,
                            char *buf, int buf_size)
{
    int written = 0;
    uint32_t i;

    written += snprintf(buf + written, (size_t)(buf_size - written), "\"hex\":\"");
    for (i = 0; i < length && written + 4 < buf_size; i++) {
        written += snprintf(buf + written, (size_t)(buf_size - written), "%02X", data[i]);
    }
    written += snprintf(buf + written, (size_t)(buf_size - written), "\"");
    return written;
}

static void write_json_string_array(FILE *output,
                                    const char values[][EZ80_HOME_COLS + 1u],
                                    uint32_t count)
{
    uint32_t i;
    char escaped[256];

    fputc('[', output);
    for (i = 0u; i < count; i++) {
        escape_json(values[i], escaped, sizeof(escaped));
        fprintf(output, "%s\"%s\"", (i > 0u) ? "," : "", escaped);
    }
    fputc(']', output);
}

static bool slot_is_active_program(const ez80_emulator_t *emu,
                                   const ez80_os_var_slot_t *slot)
{
    if (!emu || !slot || !slot->in_use) return false;
    if (slot->type != EZ80_OS_VAR_PROGRAM) return false;
    if (emu->os.active_program_name[0] == '\0') return false;
    return strncmp(emu->os.active_program_name,
                   slot->name,
                   EZ80_OS_VAR_NAME_MAX + 1u) == 0;
}

static uint32_t write_program_list_json(FILE *output, const ez80_emulator_t *emu)
{
    uint32_t i;
    uint32_t count = 0u;

    fputc('[', output);
    for (i = 0u; i < EZ80_OS_MAX_VARS; i++) {
        const ez80_os_var_slot_t *slot = &emu->os.vars[i];
        if (!slot->in_use || slot->type != EZ80_OS_VAR_PROGRAM) continue;

        fprintf(output,
                "%s{\"name\":\"%s\",\"archived\":%s,\"loaded_in_ram\":%s,"
                "\"size_bytes\":%lu,\"archive_addr\":\"0x%06X\","
                "\"ram_addr\":\"0x%06X\",\"active\":%s}",
                (count > 0u) ? "," : "",
                slot->name,
                slot->archived ? "true" : "false",
                slot->loaded_in_ram ? "true" : "false",
                (unsigned long)slot->size_bytes,
                slot->archive_addr,
                slot->ram_addr,
                slot_is_active_program(emu, slot) ? "true" : "false");
        count++;
    }
    fputc(']', output);
    return count;
}

static uint32_t count_program_slots(const ez80_emulator_t *emu)
{
    uint32_t i;
    uint32_t count = 0u;

    for (i = 0u; i < EZ80_OS_MAX_VARS; i++) {
        const ez80_os_var_slot_t *slot = &emu->os.vars[i];
        if (slot->in_use && slot->type == EZ80_OS_VAR_PROGRAM) count++;
    }
    return count;
}

/* ── Command execution ─────────────────────────────────────── */

void agent_session_init(agent_session_t *session,
                        ez80_emulator_t *emu,
                        FILE *input, FILE *output)
{
    memset(session, 0, sizeof(*session));
    session->emu = emu;
    session->snapshot = &g_agent_snapshot_storage;
    session->snapshot_valid = false;
    session->input = input;
    session->output = output;
    session->running = true;
    memset(session->snapshot, 0, sizeof(*session->snapshot));
}

static void cmd_init(agent_session_t *session, const agent_command_t *cmd)
{
    ez80_status_t st = ez80_emulator_init(session->emu);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_reset(agent_session_t *session, const agent_command_t *cmd)
{
    ez80_status_t st = ez80_emulator_reset(session->emu);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_load_rom(agent_session_t *session, const agent_command_t *cmd)
{
    if (cmd->str_arg_count < 1) {
        write_response_error(session, cmd->id, "missing 'path' argument");
        return;
    }
    ez80_status_t st = ez80_emulator_load_rom(session->emu, cmd->str_args[0]);
    if (st == EZ80_OK) {
        char data[128];
        snprintf(data, sizeof(data), "\"bytes_loaded\":%lu",
                 (unsigned long)session->emu->memory.flash_bytes_loaded);
        write_response_ok(session, cmd->id, data);
    } else {
        write_response_error(session, cmd->id, ez80_status_string(st));
    }
}

static void cmd_set_rom_loaded(agent_session_t *session, const agent_command_t *cmd)
{
    bool loaded = (cmd->int_arg_count > 0) ? (cmd->int_args[0] != 0u) : true;
    session->emu->rom_loaded = loaded;
    write_response_ok(session, cmd->id, loaded ? "\"rom_loaded\":true" : "\"rom_loaded\":false");
}

static void cmd_load_8xp(agent_session_t *session, const agent_command_t *cmd)
{
    size_t bytes_read = 0u;
    bool archived = (cmd->int_arg_count > 0) ? (cmd->int_args[0] != 0u) : false;
    ez80_8xp_parsed_t parsed;
    ez80_status_t st;
    uint32_t index;

    if (cmd->str_arg_count < 1) {
        write_response_error(session, cmd->id, "missing 'path' argument");
        return;
    }

    st = ez80_8xp_load_file(cmd->str_args[0],
                            g_agent_8xp_buffer,
                            AGENT_8XP_BUFFER_SIZE,
                            &bytes_read);
    if (st != EZ80_OK) {
        write_response_error(session, cmd->id, ez80_status_string(st));
        return;
    }

    st = ez80_8xp_parse(g_agent_8xp_buffer, bytes_read, &parsed);
    if (st != EZ80_OK) {
        write_response_error(session, cmd->id, ez80_status_string(st));
        return;
    }

    for (index = 0u; index < parsed.var_entry_count; index++) {
        st = ez80_8xp_load_into_emulator(session->emu, &parsed, index, archived);
        if (st != EZ80_OK) {
            write_response_error(session, cmd->id, ez80_status_string(st));
            return;
        }
    }

    {
        char data[192];
        snprintf(data, sizeof(data),
                 "\"entries\":%lu,\"bytes_loaded\":%lu,\"archived\":%s",
                 (unsigned long)parsed.var_entry_count,
                 (unsigned long)bytes_read,
                 archived ? "true" : "false");
        write_response_ok(session, cmd->id, data);
    }
}

static void cmd_inject_key(agent_session_t *session, const agent_command_t *cmd)
{
    if (cmd->int_arg_count < 1) {
        write_response_error(session, cmd->id, "missing 'key' argument");
        return;
    }
    ez80_status_t st = ez80_emulator_inject_key(session->emu, (uint8_t)cmd->int_args[0]);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_release_key(agent_session_t *session, const agent_command_t *cmd)
{
    if (cmd->int_arg_count < 1) {
        write_response_error(session, cmd->id, "missing 'key' argument");
        return;
    }
    ez80_status_t st = ez80_emulator_release_key(session->emu, (uint8_t)cmd->int_args[0]);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_release_all_keys(agent_session_t *session, const agent_command_t *cmd)
{
    ez80_status_t st = ez80_emulator_release_all_keys(session->emu);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_run(agent_session_t *session, const agent_command_t *cmd)
{
    uint32_t cycles = (cmd->int_arg_count > 0) ? cmd->int_args[0] : 1000000u;
    ez80_run_result_t result;
    char data[256];

    memset(&result, 0, sizeof(result));
    ez80_status_t st = ez80_emulator_run_cycles(session->emu, cycles, &result);
    if (st == EZ80_OK) {
        snprintf(data, sizeof(data),
                 "\"cycles_executed\":%lu,"
                 "\"hit_breakpoint\":%s,"
                 "\"hit_halt\":%s,"
                 "\"hit_watchpoint\":%s",
                 (unsigned long)result.cycles_executed,
                 result.hit_breakpoint ? "true" : "false",
                 result.hit_halt ? "true" : "false",
                 result.hit_watchpoint ? "true" : "false");
        write_response_ok(session, cmd->id, data);
    } else {
        write_response_error(session, cmd->id, ez80_status_string(st));
    }
}

static void cmd_step(agent_session_t *session, const agent_command_t *cmd)
{
    ez80_cpu_state_t cpu_before;
    ez80_cpu_state_t cpu_after;
    ez80_instruction_t decoded;
    char data[512];
    char mnemonic_esc[64];
    char operands_esc[128];

    ez80_emulator_get_state(session->emu, &cpu_before);
    memset(&decoded, 0, sizeof(decoded));
    ez80_status_t st = ez80_emulator_step(session->emu, &decoded);
    if (st == EZ80_OK) {
        ez80_emulator_get_state(session->emu, &cpu_after);
        escape_json(decoded.mnemonic, mnemonic_esc, sizeof(mnemonic_esc));
        escape_json(decoded.operands, operands_esc, sizeof(operands_esc));
        snprintf(data, sizeof(data),
                 "\"pc\":\"0x%06X\","
                 "\"next_pc\":\"0x%06X\","
                 "\"opcode\":\"0x%02X\","
                 "\"length\":%u,"
                 "\"cycles\":%u,"
                 "\"mnemonic\":\"%s\","
                 "\"operands\":\"%s\"",
                 cpu_before.pc & 0xFFFFFFu,
                 cpu_after.pc & 0xFFFFFFu,
                 decoded.opcode, decoded.length, decoded.cycles,
                 mnemonic_esc, operands_esc);
        write_response_ok(session, cmd->id, data);
    } else {
        write_response_error(session, cmd->id, ez80_status_string(st));
    }
}

static void cmd_get_state(agent_session_t *session, const agent_command_t *cmd)
{
    ez80_cpu_state_t cpu;
    char data[1024];

    ez80_emulator_get_state(session->emu, &cpu);
    agent_format_cpu_state(&cpu, data, sizeof(data));
    write_response_ok(session, cmd->id, data);
}

static void cmd_read_memory(agent_session_t *session, const agent_command_t *cmd)
{
    uint32_t addr, length;
    char data[8192];
    int written;
    uint32_t i;

    if (cmd->int_arg_count < 2) {
        write_response_error(session, cmd->id,
                             "missing 'addr' and 'length' arguments");
        return;
    }
    addr = cmd->int_args[0];
    length = cmd->int_args[1];
    if (length > 1024u) length = 1024u;

    written = snprintf(data, sizeof(data),
                       "\"addr\":\"0x%06X\",\"length\":%u,\"hex\":\"",
                       addr, length);
    for (i = 0; i < length && written + 4 < (int)sizeof(data); i++) {
        uint8_t val;
        ez80_status_t st = ez80_memory_read8(session->emu, addr + i, &val);
        if (st != EZ80_OK) val = 0xFFu;
        written += snprintf(data + written, sizeof(data) - (size_t)written,
                            "%02X", val);
    }
    written += snprintf(data + written, sizeof(data) - (size_t)written, "\"");

    write_response_ok(session, cmd->id, data);
}

static void cmd_write_memory(agent_session_t *session, const agent_command_t *cmd)
{
    if (cmd->int_arg_count < 2) {
        write_response_error(session, cmd->id,
                             "missing 'addr' and 'value' arguments");
        return;
    }
    ez80_status_t st = ez80_memory_write8(session->emu,
                                           cmd->int_args[0],
                                           (uint8_t)cmd->int_args[1]);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_read_port(agent_session_t *session, const agent_command_t *cmd)
{
    uint16_t port;
    uint8_t value = 0u;
    ez80_status_t st;
    char data[96];

    if (cmd->int_arg_count < 1) {
        write_response_error(session, cmd->id, "missing 'port' argument");
        return;
    }

    port = (uint16_t)(cmd->int_args[0] & 0xFFFFu);
    st = ez80_memory_read8(session->emu, EZ80_PORT_BASE + port, &value);
    if (st != EZ80_OK) {
        write_response_error(session, cmd->id, ez80_status_string(st));
        return;
    }

    snprintf(data, sizeof(data),
             "\"port\":\"0x%04X\",\"value\":\"0x%02X\"",
             port, value);
    write_response_ok(session, cmd->id, data);
}

static void cmd_write_port(agent_session_t *session, const agent_command_t *cmd)
{
    uint16_t port;
    uint8_t value;
    ez80_status_t st;
    char data[96];

    if (cmd->int_arg_count < 2) {
        write_response_error(session, cmd->id, "missing 'port' and 'value' arguments");
        return;
    }

    port = (uint16_t)(cmd->int_args[0] & 0xFFFFu);
    value = (uint8_t)(cmd->int_args[1] & 0xFFu);
    st = ez80_memory_write8(session->emu, EZ80_PORT_BASE + port, value);
    if (st != EZ80_OK) {
        write_response_error(session, cmd->id, ez80_status_string(st));
        return;
    }

    snprintf(data, sizeof(data),
             "\"port\":\"0x%04X\",\"value\":\"0x%02X\"",
             port, value);
    write_response_ok(session, cmd->id, data);
}

static void cmd_add_breakpoint(agent_session_t *session, const agent_command_t *cmd)
{
    if (cmd->int_arg_count < 1) {
        write_response_error(session, cmd->id, "missing 'addr' argument");
        return;
    }
    ez80_status_t st = ez80_emulator_add_breakpoint(session->emu, cmd->int_args[0]);
    if (st == EZ80_OK) {
        char data[64];
        snprintf(data, sizeof(data), "\"addr\":\"0x%06X\"", cmd->int_args[0]);
        write_response_ok(session, cmd->id, data);
    } else {
        write_response_error(session, cmd->id, ez80_status_string(st));
    }
}

static void cmd_remove_breakpoint(agent_session_t *session, const agent_command_t *cmd)
{
    if (cmd->int_arg_count < 1) {
        write_response_error(session, cmd->id, "missing 'addr' argument");
        return;
    }
    ez80_status_t st = ez80_emulator_remove_breakpoint(session->emu, cmd->int_args[0]);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_clear_breakpoints(agent_session_t *session, const agent_command_t *cmd)
{
    ez80_status_t st = ez80_emulator_clear_breakpoints(session->emu);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_add_watchpoint(agent_session_t *session, const agent_command_t *cmd)
{
    uint32_t addr, length;
    uint8_t type;
    if (cmd->int_arg_count < 3) {
        write_response_error(session, cmd->id,
                             "missing 'addr', 'length', 'type' arguments");
        return;
    }
    addr = cmd->int_args[0];
    length = cmd->int_args[1];
    type = (uint8_t)cmd->int_args[2];
    ez80_status_t st = ez80_emulator_add_watchpoint(session->emu, addr, length, type);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_remove_watchpoint(agent_session_t *session, const agent_command_t *cmd)
{
    if (cmd->int_arg_count < 1) {
        write_response_error(session, cmd->id, "missing 'addr' argument");
        return;
    }
    ez80_status_t st = ez80_emulator_remove_watchpoint(session->emu, cmd->int_args[0]);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_clear_watchpoints(agent_session_t *session, const agent_command_t *cmd)
{
    ez80_status_t st = ez80_emulator_clear_watchpoints(session->emu);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_get_lcd_text(agent_session_t *session, const agent_command_t *cmd)
{
    char ascii_buf[4096];
    uint32_t ascii_len = 0;
    char escaped[8192];

    ez80_status_t st = ez80_emulator_lcd_to_ascii(session->emu,
                                                    ascii_buf, sizeof(ascii_buf),
                                                    &ascii_len);
    if (st == EZ80_OK) {
        char data[8192 + 32];
        escape_json(ascii_buf, escaped, sizeof(escaped));
        snprintf(data, sizeof(data), "\"lcd_text\":\"%s\"", escaped);
        write_response_ok(session, cmd->id, data);
    } else {
        write_response_error(session, cmd->id, ez80_status_string(st));
    }
}

static void cmd_get_lcd_raw(agent_session_t *session, const agent_command_t *cmd)
{
    size_t pixel_count = (size_t)EZ80_GRAPH_WIDTH * (size_t)EZ80_GRAPH_HEIGHT;
    ez80_status_t st;
    size_t i;

    st = ez80_emulator_lcd_get_framebuffer(session->emu, g_agent_lcd_framebuffer, pixel_count);
    if (st != EZ80_OK) {
        write_response_error(session, cmd->id, ez80_status_string(st));
        return;
    }

    fprintf(session->output,
            "{\"id\":%u,\"status\":\"ok\",\"width\":%u,\"height\":%u,\"rgb565_hex\":\"",
            cmd->id,
            EZ80_GRAPH_WIDTH,
            EZ80_GRAPH_HEIGHT);
    for (i = 0u; i < pixel_count; i++) {
        fprintf(session->output, "%04X", g_agent_lcd_framebuffer[i]);
    }
    fprintf(session->output, "\"}\n");
    fflush(session->output);
}

static void cmd_get_screen_state(agent_session_t *session, const agent_command_t *cmd)
{
    ez80_screen_text_state_t state;
    ez80_status_t st;
    char escaped_input[256];
    char escaped_ascii[4096];

    st = ez80_emulator_get_screen_text_state(session->emu, &state);
    if (st != EZ80_OK) {
        write_response_error(session, cmd->id, ez80_status_string(st));
        return;
    }

    escape_json(state.input_line, escaped_input, sizeof(escaped_input));
    escape_json(state.ascii, escaped_ascii, sizeof(escaped_ascii));

    fprintf(session->output,
            "{\"id\":%u,\"status\":\"ok\",\"screen\":{"
            "\"homescreen_active\":%s,"
            "\"cursor_row\":%u,"
            "\"cursor_col\":%u,"
            "\"input_line\":\"%s\","
            "\"visible_line_count\":%u,"
            "\"step_line_count\":%u,"
            "\"visible_lines\":",
            cmd->id,
            state.homescreen_active ? "true" : "false",
            state.cursor_row,
            state.cursor_col,
            escaped_input,
            state.visible_line_count,
            state.step_line_count);
    write_json_string_array(session->output, state.visible_lines, state.visible_line_count);
    fprintf(session->output, ",\"step_lines\":");
    write_json_string_array(session->output, state.step_lines, state.step_line_count);
    fprintf(session->output,
            ",\"ascii\":\"%s\","
            "\"active_program\":\"%s\","
            "\"active_program_ram\":\"0x%06X\"}}\n",
            escaped_ascii,
            session->emu->os.active_program_name,
            session->emu->os.active_program_ram_addr);
    fflush(session->output);
}

static void cmd_write_lcd_png(agent_session_t *session, const agent_command_t *cmd)
{
    ez80_status_t st;
    char escaped_path[512];
    char data[640];

    if (cmd->str_arg_count < 1) {
        write_response_error(session, cmd->id, "missing 'path' argument");
        return;
    }

    st = ez80_emulator_lcd_write_png(session->emu, cmd->str_args[0]);
    if (st != EZ80_OK) {
        write_response_error(session, cmd->id, ez80_status_string(st));
        return;
    }

    escape_json(cmd->str_args[0], escaped_path, sizeof(escaped_path));
    snprintf(data, sizeof(data),
             "\"path\":\"%s\",\"width\":%u,\"height\":%u",
             escaped_path,
             EZ80_GRAPH_WIDTH,
             EZ80_GRAPH_HEIGHT);
    write_response_ok(session, cmd->id, data);
}

static void cmd_get_homescreen(agent_session_t *session, const agent_command_t *cmd)
{
    char data[2048];
    int written = 0;
    uint8_t row;
    char line[EZ80_HOME_COLS + 1u];
    char escaped_line[64];

    written += snprintf(data + written, sizeof(data) - (size_t)written,
                        "\"homescreen\":[");
    for (row = 0; row < EZ80_HOME_ROWS; row++) {
        ez80_status_t st = ez80_emulator_home_get_line(session->emu, row,
                                                        line, sizeof(line));
        if (st != EZ80_OK) {
            memset(line, ' ', EZ80_HOME_COLS);
            line[EZ80_HOME_COLS] = '\0';
        }
        escape_json(line, escaped_line, sizeof(escaped_line));
        written += snprintf(data + written, sizeof(data) - (size_t)written,
                            "%s\"%s\"", row > 0 ? "," : "", escaped_line);
    }
    written += snprintf(data + written, sizeof(data) - (size_t)written, "]");
    write_response_ok(session, cmd->id, data);
}

static void cmd_list_programs(agent_session_t *session, const agent_command_t *cmd)
{
    uint32_t count = count_program_slots(session->emu);

    fprintf(session->output, "{\"id\":%u,\"status\":\"ok\",\"program_count\":", cmd->id);
    fprintf(session->output, "%u,\"programs\":", count);
    write_program_list_json(session->output, session->emu);
    fprintf(session->output, "}\n");
    fflush(session->output);
}

static void cmd_launch_program(agent_session_t *session, const agent_command_t *cmd)
{
    uint32_t ram_addr = 0;
    if (cmd->str_arg_count < 1) {
        write_response_error(session, cmd->id, "missing 'name' argument");
        return;
    }
    ez80_status_t st = ez80_emulator_launch_program(session->emu,
                                                      cmd->str_args[0],
                                                      &ram_addr);
    if (st == EZ80_OK) {
        char data[64];
        snprintf(data, sizeof(data), "\"ram_addr\":\"0x%06X\"", ram_addr);
        write_response_ok(session, cmd->id, data);
    } else {
        write_response_error(session, cmd->id, ez80_status_string(st));
    }
}

static void cmd_finish_program(agent_session_t *session, const agent_command_t *cmd)
{
    uint32_t archive_addr = 0;
    const char *name = NULL;
    ez80_status_t st;

    if (cmd->str_arg_count >= 1 && cmd->str_args[0][0] != '\0') {
        name = cmd->str_args[0];
    }

    st = ez80_emulator_finish_program(session->emu, name, &archive_addr);
    if (st == EZ80_OK) {
        char data[68];
        snprintf(data, sizeof(data), "\"archive_addr\":\"0x%06X\"", archive_addr);
        write_response_ok(session, cmd->id, data);
    } else {
        write_response_error(session, cmd->id, ez80_status_string(st));
    }
}

static void cmd_set_os_version(agent_session_t *session, const agent_command_t *cmd)
{
    if (cmd->int_arg_count < 2) {
        write_response_error(session, cmd->id,
                             "missing 'major' and 'minor' arguments");
        return;
    }
    uint8_t major = (uint8_t)cmd->int_args[0];
    uint8_t minor = (uint8_t)cmd->int_args[1];
    uint8_t patch = (cmd->int_arg_count >= 3) ? (uint8_t)cmd->int_args[2] : 0;

    ez80_status_t st = ez80_emulator_set_os_version(session->emu,
                                                      major, minor, patch);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_enable_interrupts(agent_session_t *session, const agent_command_t *cmd)
{
    bool enable = (cmd->int_arg_count > 0) ? (cmd->int_args[0] != 0) : true;
    ez80_status_t st = ez80_emulator_enable_interrupts(session->emu, enable);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_snapshot_save(agent_session_t *session, const agent_command_t *cmd)
{
    ez80_status_t st = ez80_emulator_snapshot_save(session->emu, session->snapshot);
    if (st == EZ80_OK) {
        char data[64];
        session->snapshot_valid = true;
        snprintf(data, sizeof(data), "\"snapshot_id\":%u",
                 session->snapshot->snapshot_id);
        write_response_ok(session, cmd->id, data);
    } else {
        write_response_error(session, cmd->id, ez80_status_string(st));
    }
}

static void cmd_snapshot_restore(agent_session_t *session, const agent_command_t *cmd)
{
    if (!session->snapshot_valid || !session->snapshot) {
        write_response_error(session, cmd->id, "no snapshot saved");
        return;
    }
    ez80_status_t st = ez80_emulator_snapshot_restore(session->emu, session->snapshot);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_trace_enable(agent_session_t *session, const agent_command_t *cmd)
{
    bool enable = (cmd->int_arg_count > 0) ? (cmd->int_args[0] != 0) : true;
    ez80_status_t st = ez80_emulator_trace_enable(session->emu, enable);
    if (st == EZ80_OK)
        write_response_ok(session, cmd->id, NULL);
    else
        write_response_error(session, cmd->id, ez80_status_string(st));
}

static void cmd_trace_get(agent_session_t *session, const agent_command_t *cmd)
{
    uint32_t count = 0;
    char data[4096];
    int written = 0;
    uint32_t i;

    ez80_emulator_trace_get_count(session->emu, &count);

    written += snprintf(data + written, sizeof(data) - (size_t)written,
                        "\"count\":%u,\"entries\":[", count);

    for (i = 0; i < count && i < 64u && written + 64 < (int)sizeof(data); i++) {
        ez80_trace_entry_t entry;
        ez80_emulator_trace_get_entry(session->emu, i, &entry);
        written += snprintf(data + written, sizeof(data) - (size_t)written,
                            "%s{\"pc\":\"0x%06X\",\"opcode\":\"0x%02X\",\"length\":%u}",
                            i > 0 ? "," : "",
                            entry.pc, entry.opcode, entry.length);
    }

    written += snprintf(data + written, sizeof(data) - (size_t)written, "]");
    write_response_ok(session, cmd->id, data);
}

static void cmd_ping(agent_session_t *session, const agent_command_t *cmd)
{
    char data[64];
    snprintf(data, sizeof(data), "\"pong\":true,\"cmd_count\":%u",
             session->cmd_count);
    write_response_ok(session, cmd->id, data);
}

static void cmd_help(agent_session_t *session, const agent_command_t *cmd)
{
    const char *help =
        "\"commands\":["
        "\"init\",\"reset\",\"load_rom\",\"set_rom_loaded\",\"load_8xp\","
        "\"inject_key\",\"release_key\",\"release_all_keys\","
        "\"run\",\"step\",\"get_state\","
        "\"read_memory\",\"write_memory\","
        "\"read_port\",\"write_port\","
        "\"add_breakpoint\",\"remove_breakpoint\",\"clear_breakpoints\","
        "\"add_watchpoint\",\"remove_watchpoint\",\"clear_watchpoints\","
        "\"get_lcd_text\",\"get_lcd_raw\",\"get_screen_state\",\"write_lcd_png\",\"get_homescreen\","
        "\"list_programs\",\"launch_program\",\"finish_program\",\"set_os_version\",\"enable_interrupts\","
        "\"snapshot_save\",\"snapshot_restore\","
        "\"trace_enable\",\"trace_get\","
        "\"ping\",\"quit\",\"help\""
        "]";
    write_response_ok(session, cmd->id, help);
}

/* ── Command dispatch ──────────────────────────────────────── */

bool agent_execute_command(agent_session_t *session, const agent_command_t *cmd)
{
    session->cmd_count++;

    switch (cmd->type) {
        case AGENT_CMD_INIT:               cmd_init(session, cmd); break;
        case AGENT_CMD_RESET:              cmd_reset(session, cmd); break;
        case AGENT_CMD_LOAD_ROM:           cmd_load_rom(session, cmd); break;
        case AGENT_CMD_SET_ROM_LOADED:     cmd_set_rom_loaded(session, cmd); break;
        case AGENT_CMD_LOAD_8XP:           cmd_load_8xp(session, cmd); break;
        case AGENT_CMD_INJECT_KEY:         cmd_inject_key(session, cmd); break;
        case AGENT_CMD_RELEASE_KEY:        cmd_release_key(session, cmd); break;
        case AGENT_CMD_RELEASE_ALL_KEYS:   cmd_release_all_keys(session, cmd); break;
        case AGENT_CMD_RUN:                cmd_run(session, cmd); break;
        case AGENT_CMD_STEP:               cmd_step(session, cmd); break;
        case AGENT_CMD_GET_STATE:          cmd_get_state(session, cmd); break;
        case AGENT_CMD_READ_MEMORY:        cmd_read_memory(session, cmd); break;
        case AGENT_CMD_WRITE_MEMORY:       cmd_write_memory(session, cmd); break;
        case AGENT_CMD_READ_PORT:          cmd_read_port(session, cmd); break;
        case AGENT_CMD_WRITE_PORT:         cmd_write_port(session, cmd); break;
        case AGENT_CMD_ADD_BREAKPOINT:     cmd_add_breakpoint(session, cmd); break;
        case AGENT_CMD_REMOVE_BREAKPOINT:  cmd_remove_breakpoint(session, cmd); break;
        case AGENT_CMD_CLEAR_BREAKPOINTS:  cmd_clear_breakpoints(session, cmd); break;
        case AGENT_CMD_ADD_WATCHPOINT:     cmd_add_watchpoint(session, cmd); break;
        case AGENT_CMD_REMOVE_WATCHPOINT:  cmd_remove_watchpoint(session, cmd); break;
        case AGENT_CMD_CLEAR_WATCHPOINTS:  cmd_clear_watchpoints(session, cmd); break;
        case AGENT_CMD_GET_LCD_TEXT:       cmd_get_lcd_text(session, cmd); break;
        case AGENT_CMD_GET_LCD_RAW:        cmd_get_lcd_raw(session, cmd); break;
        case AGENT_CMD_GET_SCREEN_STATE:   cmd_get_screen_state(session, cmd); break;
        case AGENT_CMD_WRITE_LCD_PNG:      cmd_write_lcd_png(session, cmd); break;
        case AGENT_CMD_GET_HOMESCREEN:     cmd_get_homescreen(session, cmd); break;
        case AGENT_CMD_LIST_PROGRAMS:      cmd_list_programs(session, cmd); break;
        case AGENT_CMD_LAUNCH_PROGRAM:     cmd_launch_program(session, cmd); break;
        case AGENT_CMD_FINISH_PROGRAM:     cmd_finish_program(session, cmd); break;
        case AGENT_CMD_SET_OS_VERSION:     cmd_set_os_version(session, cmd); break;
        case AGENT_CMD_ENABLE_INTERRUPTS:  cmd_enable_interrupts(session, cmd); break;
        case AGENT_CMD_SNAPSHOT_SAVE:      cmd_snapshot_save(session, cmd); break;
        case AGENT_CMD_SNAPSHOT_RESTORE:   cmd_snapshot_restore(session, cmd); break;
        case AGENT_CMD_TRACE_ENABLE:       cmd_trace_enable(session, cmd); break;
        case AGENT_CMD_TRACE_GET:          cmd_trace_get(session, cmd); break;
        case AGENT_CMD_PING:               cmd_ping(session, cmd); break;
        case AGENT_CMD_HELP:               cmd_help(session, cmd); break;
        case AGENT_CMD_QUIT:
            write_response_ok(session, cmd->id, "\"goodbye\":true");
            session->running = false;
            return false;
        default:
            write_response_error(session, cmd->id, "unknown command");
            break;
    }
    return true;
}

void agent_run_loop(agent_session_t *session)
{
    char line[AGENT_CMD_MAX_LEN];
    agent_command_t cmd;

    /* Write ready banner */
    fprintf(session->output,
            "{\"status\":\"ready\",\"protocol\":\"ez80-agent\",\"version\":1}\n");
    fflush(session->output);

    while (session->running) {
        if (!fgets(line, (int)sizeof(line), session->input)) break;

        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0) continue;

        if (!agent_parse_command(line, &cmd)) {
            write_response_error(session, 0, "invalid JSON command");
            continue;
        }

        if (!agent_execute_command(session, &cmd))
            break;
    }

    session->snapshot_valid = false;
}
