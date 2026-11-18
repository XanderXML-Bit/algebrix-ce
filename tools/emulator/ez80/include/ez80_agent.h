/*
 * ez80_agent.h — Structured agent interface for the eZ80 emulator
 *
 * Provides a JSON-based command/response protocol for programmatic
 * interaction, state inspection, and automated testing.
 */
#ifndef EZ80_AGENT_H
#define EZ80_AGENT_H

#include "ez80_emulator.h"
#include <stdio.h>

/* Maximum lengths for agent protocol */
#define AGENT_CMD_MAX_LEN 4096u
#define AGENT_RESP_MAX_LEN 16384u
#define AGENT_MAX_ARGS 16u

/* ── Command types ─────────────────────────────────────────── */
typedef enum {
    AGENT_CMD_UNKNOWN = 0,
    AGENT_CMD_INIT,
    AGENT_CMD_RESET,
    AGENT_CMD_LOAD_ROM,
    AGENT_CMD_SET_ROM_LOADED,
    AGENT_CMD_LOAD_8XP,
    AGENT_CMD_INJECT_KEY,
    AGENT_CMD_RELEASE_KEY,
    AGENT_CMD_RELEASE_ALL_KEYS,
    AGENT_CMD_RUN,
    AGENT_CMD_STEP,
    AGENT_CMD_GET_STATE,
    AGENT_CMD_READ_MEMORY,
    AGENT_CMD_WRITE_MEMORY,
    AGENT_CMD_READ_PORT,
    AGENT_CMD_WRITE_PORT,
    AGENT_CMD_ADD_BREAKPOINT,
    AGENT_CMD_REMOVE_BREAKPOINT,
    AGENT_CMD_CLEAR_BREAKPOINTS,
    AGENT_CMD_ADD_WATCHPOINT,
    AGENT_CMD_REMOVE_WATCHPOINT,
    AGENT_CMD_CLEAR_WATCHPOINTS,
    AGENT_CMD_GET_LCD_TEXT,
    AGENT_CMD_GET_LCD_RAW,
    AGENT_CMD_GET_SCREEN_STATE,
    AGENT_CMD_WRITE_LCD_PNG,
    AGENT_CMD_GET_HOMESCREEN,
    AGENT_CMD_LIST_PROGRAMS,
    AGENT_CMD_LAUNCH_PROGRAM,
    AGENT_CMD_FINISH_PROGRAM,
    AGENT_CMD_SET_OS_VERSION,
    AGENT_CMD_ENABLE_INTERRUPTS,
    AGENT_CMD_SNAPSHOT_SAVE,
    AGENT_CMD_SNAPSHOT_RESTORE,
    AGENT_CMD_TRACE_ENABLE,
    AGENT_CMD_TRACE_GET,
    AGENT_CMD_PING,
    AGENT_CMD_QUIT,
    AGENT_CMD_HELP,
    AGENT_CMD_COUNT
} agent_cmd_type_t;

/* ── Parsed command ────────────────────────────────────────── */
typedef struct {
    agent_cmd_type_t type;
    uint32_t id;                 /* client-provided command ID for correlation */
    char str_args[AGENT_MAX_ARGS][256]; /* string arguments */
    uint32_t int_args[AGENT_MAX_ARGS];  /* integer arguments */
    int str_arg_count;
    int int_arg_count;
} agent_command_t;

/* ── Agent session state ───────────────────────────────────── */
typedef struct {
    ez80_emulator_t *emu;
    ez80_snapshot_t *snapshot;  /* held snapshot (if any) */
    bool snapshot_valid;
    bool running;
    uint32_t cmd_count;
    FILE *input;
    FILE *output;
} agent_session_t;

/* ── Agent API ─────────────────────────────────────────────── */

/*
 * Initialize an agent session.
 * emu must be a valid, initialized emulator.
 * input/output are the streams for reading commands / writing responses.
 */
void agent_session_init(agent_session_t *session,
                        ez80_emulator_t *emu,
                        FILE *input, FILE *output);

/*
 * Parse a JSON command string into an agent_command_t.
 * Returns true on success, false on parse error.
 * On error, writes a JSON error response to session->output.
 */
bool agent_parse_command(const char *json_line, agent_command_t *cmd);

/*
 * Execute a parsed command against the emulator.
 * Writes a JSON response to session->output.
 * Returns true if the session should continue, false on QUIT.
 */
bool agent_execute_command(agent_session_t *session,
                           const agent_command_t *cmd);

/*
 * Run the interactive command loop.
 * Reads JSON commands from session->input, executes them,
 * writes JSON responses to session->output.
 * Returns when QUIT command is received or input ends.
 */
void agent_run_loop(agent_session_t *session);

/*
 * Format a CPU state as JSON into buffer.
 * Returns chars written.
 */
int agent_format_cpu_state(const ez80_cpu_state_t *cpu,
                           char *buf, int buf_size);

/*
 * Format a memory region as hex JSON into buffer.
 * Returns chars written.
 */
int agent_format_memory_hex(const uint8_t *data, uint32_t length,
                            char *buf, int buf_size);

/*
 * Get the command type name as a string.
 */
const char *agent_cmd_type_name(agent_cmd_type_t type);

#endif /* EZ80_AGENT_H */
