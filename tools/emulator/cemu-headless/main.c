/*
 * cemu-headless/main.c
 *
 * Pass 71 dual-mode backend:
 *   - NATIVE mode (CEMU_HEADLESS_HAS_CEMU_CORE=1): drives the CEmu core
 *     directly — emu_load/emu_run/emu_keypad_event/emu_lcd_drawframe — with
 *     optional PNG capture via stb_image_write.
 *   - WRAPPER mode (CEMU_HEADLESS_HAS_CEMU_CORE=0): replays the session into
 *     a temporary autotester.exe JSON file and parses the CRC from its output.
 *     PNG is not available in wrapper mode.
 *
 * Both modes speak the same JSON-line stdin/stdout protocol.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#define CEMU_HEADLESS_POPEN  _popen
#define CEMU_HEADLESS_PCLOSE _pclose
#else
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#define CEMU_HEADLESS_POPEN  popen
#define CEMU_HEADLESS_PCLOSE pclose
#endif

/* ── CEmu native-core headers ──────────────────────────────────────────────── */
#if CEMU_HEADLESS_HAS_CEMU_CORE
#include "emu.h"
#include "asic.h"
#include "bootver.h"
#include "lcd.h"
#include "keypad.h"
#include "link.h"
#include "schedule.h"
#include "extras.h"
#endif

/* ── stb_image_write (single-header PNG exporter) ──────────────────────────── */
#if CEMU_HEADLESS_PNG_EXPORT
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif

/* ── Version ──────────────────────────────────────────────────────────────── */
#if CEMU_HEADLESS_HAS_CEMU_CORE
#define CEMU_HEADLESS_VERSION       "0.3.0-native-core"
#else
#define CEMU_HEADLESS_VERSION       "0.2.0-autotester-wrapper"
#endif
#define INPUT_BUFFER_SIZE           4096
#define MAX_PATH_LENGTH             2048
#define MAX_KEY_NAME_LENGTH         64
#define MAX_ERROR_LENGTH            1024
#define MAX_APPS                    256
#define MAX_KEY_EVENTS              4096
#define MAX_TARGET_NAME_LENGTH      9
#define MAX_OUTPUT_EXCERPT          4096
#define MAX_COMMAND_LENGTH          8192
#define MAX_LINE_LENGTH             2048
#define HASH_NAME_CAPTURE           "cemu_headless_capture"
#define HASH_START_NAME             "vram_start"
#define HASH_SIZE_NAME              "vram_16_size"
#define DEFAULT_LAUNCH_DELAY_MS     4000
#define DEFAULT_HASH_TIMEOUT_MS     12000
#define DEFAULT_DELAY_GENERAL_MS    250
#define DEFAULT_DELAY_ENTER_MS      2000
#define DEFAULT_DELAY_MENU_MS       1200
#define DEFAULT_DELAY_CLEAR_MS      400
#define DEFAULT_DELAY_NAV_MS        300
#define DEFAULT_DELAY_SHIFT_MS      200
#define MAX_DELAY_MS                30000
#define CYCLES_PER_MILLISECOND      48000L

typedef struct {
    char key[MAX_KEY_NAME_LENGTH];
    long duration_ms;
} cemu_headless_key_event_t;

typedef struct {
    int rom_loaded;
    int running;
    int halted;
    long total_cycles;
    long pending_delay_ms;
    char rom_path[MAX_PATH_LENGTH];
    /* wrapper-mode fields */
    char autotester_path[MAX_PATH_LENGTH];
    int autotester_resolved;
    char target_name[MAX_TARGET_NAME_LENGTH];
    int target_is_asm;
    int app_count;
    char app_paths[MAX_APPS][MAX_PATH_LENGTH];
    int key_count;
    cemu_headless_key_event_t key_history[MAX_KEY_EVENTS];
    char last_crc[16];
    int last_autotester_exit;
    char last_output_excerpt[MAX_OUTPUT_EXCERPT];
    /* native-mode flag — set when cemu-core is active */
    int native_mode_active;
} cemu_headless_session_t;

static cemu_headless_session_t g_session;
static int g_should_exit = 0;
static int g_command_failed = 0;
static char g_last_script_path[MAX_PATH_LENGTH];

static int ascii_casecmp(const char *left, const char *right) {
    unsigned char c_left;
    unsigned char c_right;

    if (!left && !right) {
        return 0;
    }
    if (!left) {
        return -1;
    }
    if (!right) {
        return 1;
    }

    while (*left != '\0' && *right != '\0') {
        c_left = (unsigned char)tolower((unsigned char)*left);
        c_right = (unsigned char)tolower((unsigned char)*right);
        if (c_left != c_right) {
            return (int)c_left - (int)c_right;
        }
        left++;
        right++;
    }

    return (int)(unsigned char)tolower((unsigned char)*left) -
           (int)(unsigned char)tolower((unsigned char)*right);
}

static const char *path_basename_ptr(const char *path) {
    const char *base = path;
    const char *scan = path;

    if (!path) {
        return "";
    }

    while (*scan != '\0') {
        if (*scan == '\\' || *scan == '/') {
            base = scan + 1;
        }
        scan++;
    }

    return base;
}

static int file_exists(const char *path) {
    FILE *file;
    if (!path || path[0] == '\0') {
        return 0;
    }
    file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    fclose(file);
    return 1;
}

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static int has_extension_ci(const char *path, const char *extension) {
    const char *dot;
    if (!path || !extension) {
        return 0;
    }
    dot = strrchr(path, '.');
    if (!dot) {
        return 0;
    }
    return ascii_casecmp(dot, extension) == 0;
}

static int string_contains_ci(const char *haystack, const char *needle) {
    size_t needle_len;
    size_t i;

    if (!haystack || !needle) {
        return 0;
    }
    needle_len = strlen(needle);
    if (needle_len == 0) {
        return 1;
    }

    for (i = 0; haystack[i] != '\0'; i++) {
        size_t j;
        for (j = 0; j < needle_len; j++) {
            char c_left = haystack[i + j];
            char c_right = needle[j];
            if (c_left == '\0') {
                return 0;
            }
            if (tolower((unsigned char)c_left) != tolower((unsigned char)c_right)) {
                break;
            }
        }
        if (j == needle_len) {
            return 1;
        }
    }

    return 0;
}

static void normalise_target_name(const char *path, char *out, size_t out_size) {
    const char *base;
    const char *dot;
    size_t length;
    size_t i;
    size_t o = 0;

    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!path) {
        return;
    }

    base = path_basename_ptr(path);
    dot = strrchr(base, '.');
    length = dot && dot > base ? (size_t)(dot - base) : strlen(base);
    if (length > 32) {
        length = 32;
    }

    for (i = 0; i < length && o < out_size - 1; i++) {
        unsigned char c = (unsigned char)base[i];
        if (isalnum(c) || c == '_') {
            out[o++] = (char)toupper(c);
        }
    }

    if (o > MAX_TARGET_NAME_LENGTH - 1) {
        o = MAX_TARGET_NAME_LENGTH - 1;
    }
    out[o] = '\0';
}

static int infer_target_is_asm(const char *path, const char *target_name) {
    if (path && string_contains_ci(path, "artifice")) {
        return 0;
    }
    if (target_name && ascii_casecmp(target_name, "A") == 0 && path &&
        string_contains_ci(path, "A.8xp")) {
        return 0;
    }
    return 1;
}

static void session_clear_runtime_state(void) {
    g_session.running = g_session.rom_loaded;
    g_session.halted = 0;
    g_session.total_cycles = 0;
    g_session.pending_delay_ms = 0;
    g_session.key_count = 0;
    g_session.last_crc[0] = '\0';
    g_session.last_output_excerpt[0] = '\0';
    g_session.last_autotester_exit = 0;
}

static void session_refresh_default_target(void) {
    int i;
    int preferred_index = -1;
    int fallback_index = -1;
    char target_name[MAX_TARGET_NAME_LENGTH];

    for (i = 0; i < g_session.app_count; i++) {
        if (has_extension_ci(g_session.app_paths[i], ".8xp")) {
            if (fallback_index < 0) {
                fallback_index = i;
            }
            normalise_target_name(g_session.app_paths[i], target_name, sizeof(target_name));
            if (ascii_casecmp(target_name, "CALCENG") == 0) {
                preferred_index = i;
                break;
            }
        }
    }

    if (preferred_index < 0) {
        preferred_index = fallback_index;
    }
    if (preferred_index < 0 && g_session.app_count > 0) {
        preferred_index = 0;
    }

    if (preferred_index >= 0) {
        normalise_target_name(g_session.app_paths[preferred_index],
                              g_session.target_name,
                              sizeof(g_session.target_name));
        g_session.target_is_asm = infer_target_is_asm(g_session.app_paths[preferred_index],
                                                      g_session.target_name);
    } else {
        g_session.target_name[0] = '\0';
        g_session.target_is_asm = 1;
    }
}

static int session_has_app_path(const char *path) {
    int i;
    for (i = 0; i < g_session.app_count; i++) {
        if (ascii_casecmp(g_session.app_paths[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

static int session_add_app_path(const char *path) {
    if (!path || path[0] == '\0') {
        return 0;
    }
    if (g_session.app_count >= MAX_APPS) {
        return -1;
    }
    if (session_has_app_path(path)) {
        return 0;
    }
    copy_string(g_session.app_paths[g_session.app_count],
                sizeof(g_session.app_paths[g_session.app_count]),
                path);
    g_session.app_count++;
    session_refresh_default_target();
    return 1;
}

static int path_compare_qsort(const void *left, const void *right) {
    const char *const *path_left = (const char *const *)left;
    const char *const *path_right = (const char *const *)right;
    return ascii_casecmp(*path_left, *path_right);
}

static int session_add_app_directory(const char *directory_path, int *added_count) {
    int local_added = 0;
#if defined(_WIN32)
    WIN32_FIND_DATAA find_data;
    HANDLE handle;
    char search_pattern[MAX_PATH_LENGTH];
    char collected_storage[MAX_APPS][MAX_PATH_LENGTH];
    char *collected_paths[MAX_APPS];
    int collected_count = 0;
    int i;

    if (!directory_path || directory_path[0] == '\0') {
        return 0;
    }

    snprintf(search_pattern, sizeof(search_pattern), "%s\\*", directory_path);
    handle = FindFirstFileA(search_pattern, &find_data);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }

    do {
        char full_path[MAX_PATH_LENGTH];

        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (!has_extension_ci(find_data.cFileName, ".8xp") &&
            !has_extension_ci(find_data.cFileName, ".8xv")) {
            continue;
        }
        if (collected_count >= MAX_APPS) {
            FindClose(handle);
            return -1;
        }

        snprintf(full_path, sizeof(full_path), "%s\\%s", directory_path, find_data.cFileName);
        copy_string(collected_storage[collected_count],
                    sizeof(collected_storage[collected_count]),
                    full_path);
        collected_paths[collected_count] = collected_storage[collected_count];
        collected_count++;
    } while (FindNextFileA(handle, &find_data) != 0);

    FindClose(handle);
    qsort(collected_paths, (size_t)collected_count, sizeof(collected_paths[0]), path_compare_qsort);

    for (i = 0; i < collected_count; i++) {
        int add_result = session_add_app_path(collected_paths[i]);
        if (add_result < 0) {
            return -1;
        }
        if (add_result > 0) {
            local_added++;
        }
    }
#else
    DIR *directory;
    struct dirent *entry;
    char collected_storage[MAX_APPS][MAX_PATH_LENGTH];
    char *collected_paths[MAX_APPS];
    int collected_count = 0;
    int i;

    directory = opendir(directory_path);
    if (!directory) {
        return 0;
    }

    while ((entry = readdir(directory)) != NULL) {
        char full_path[MAX_PATH_LENGTH];

        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!has_extension_ci(entry->d_name, ".8xp") &&
            !has_extension_ci(entry->d_name, ".8xv")) {
            continue;
        }
        if (collected_count >= MAX_APPS) {
            closedir(directory);
            return -1;
        }

        snprintf(full_path, sizeof(full_path), "%s/%s", directory_path, entry->d_name);
        copy_string(collected_storage[collected_count],
                    sizeof(collected_storage[collected_count]),
                    full_path);
        collected_paths[collected_count] = collected_storage[collected_count];
        collected_count++;
    }

    closedir(directory);
    qsort(collected_paths, (size_t)collected_count, sizeof(collected_paths[0]), path_compare_qsort);

    for (i = 0; i < collected_count; i++) {
        int add_result = session_add_app_path(collected_paths[i]);
        if (add_result < 0) {
            return -1;
        }
        if (add_result > 0) {
            local_added++;
        }
    }
#endif

    if (added_count) {
        *added_count = local_added;
    }
    return 1;
}

static int json_escape_string(const char *in, char *out, int out_size) {
    int i = 0;
    int o = 0;

    if (!in || !out || out_size <= 0) {
        return 0;
    }

    while (in[i] != '\0' && o < out_size - 4) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
            case '"':
                out[o++] = '\\';
                out[o++] = '"';
                break;
            case '\\':
                out[o++] = '\\';
                out[o++] = '\\';
                break;
            case '\n':
                out[o++] = '\\';
                out[o++] = 'n';
                break;
            case '\r':
                out[o++] = '\\';
                out[o++] = 'r';
                break;
            case '\t':
                out[o++] = '\\';
                out[o++] = 't';
                break;
            default:
                if (c < 0x20) {
                    if (o + 7 >= out_size) {
                        goto done;
                    }
                    out[o++] = '\\';
                    out[o++] = 'u';
                    out[o++] = '0';
                    out[o++] = '0';
                    out[o++] = "0123456789abcdef"[(c >> 4) & 0xF];
                    out[o++] = "0123456789abcdef"[c & 0xF];
                } else {
                    out[o++] = (char)c;
                }
                break;
        }
        i++;
    }

done:
    out[o] = '\0';
    return o;
}

static void emit_ok(const char *cmd, const char *extra_json) {
    if (extra_json && extra_json[0] != '\0') {
        printf("{\"ok\":true,\"cmd\":\"%s\",%s}\n", cmd, extra_json);
    } else {
        printf("{\"ok\":true,\"cmd\":\"%s\"}\n", cmd);
    }
    fflush(stdout);
}

static void emit_error(const char *cmd, const char *fmt, ...) {
    char msg[MAX_ERROR_LENGTH];
    char escaped[MAX_ERROR_LENGTH * 2];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    json_escape_string(msg, escaped, (int)sizeof(escaped));
    printf("{\"ok\":false,\"cmd\":\"%s\",\"error\":\"%s\"}\n", cmd, escaped);
    fflush(stdout);

    g_command_failed = 1;
}

static int json_get_string(const char *json, const char *key, char *out, int out_size) {
    char search[128];
    const char *p;
    int o;

    if (!json || !key || !out || out_size <= 0) {
        return 0;
    }

    snprintf(search, sizeof(search), "\"%s\"", key);
    p = strstr(json, search);
    if (!p) {
        return 0;
    }

    p += (int)strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p != '"') {
        return 0;
    }

    p++;
    o = 0;
    while (*p != '\0' && *p != '"' && o < out_size - 1) {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':
                    out[o++] = '"';
                    break;
                case '\\':
                    out[o++] = '\\';
                    break;
                case 'n':
                    out[o++] = '\n';
                    break;
                case 'r':
                    out[o++] = '\r';
                    break;
                case 't':
                    out[o++] = '\t';
                    break;
                case '/':
                    out[o++] = '/';
                    break;
                default:
                    out[o++] = *p;
                    break;
            }
        } else {
            out[o++] = *p;
        }
        p++;
    }

    out[o] = '\0';
    return 1;
}

static int json_get_int(const char *json, const char *key, long *out) {
    char search[128];
    const char *p;
    char *end;
    long value;

    if (!json || !key || !out) {
        return 0;
    }

    snprintf(search, sizeof(search), "\"%s\"", key);
    p = strstr(json, search);
    if (!p) {
        return 0;
    }

    p += (int)strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (!isdigit((unsigned char)*p) && *p != '-') {
        return 0;
    }

    value = strtol(p, &end, 10);
    if (end == p) {
        return 0;
    }

    *out = value;
    return 1;
}

static int json_get_bool(const char *json, const char *key, int *out) {
    char search[128];
    const char *p;

    if (!json || !key || !out) {
        return 0;
    }

    snprintf(search, sizeof(search), "\"%s\"", key);
    p = strstr(json, search);
    if (!p) {
        return 0;
    }

    p += (int)strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r' || *p == '\n') {
        p++;
    }

    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    if (*p == '1') {
        *out = 1;
        return 1;
    }
    if (*p == '0') {
        *out = 0;
        return 1;
    }

    return 0;
}

static int normalise_autotester_key(const char *input, char *out, size_t out_size) {
    struct {
        const char *alias;
        const char *canonical;
    } aliases[] = {
        {"0", "0"}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"},
        {"5", "5"}, {"6", "6"}, {"7", "7"}, {"8", "8"}, {"9", "9"},
        {"+", "+"}, {"add", "+"}, {"plus", "+"},
        {"-", "-"}, {"sub", "-"}, {"minus", "-"}, {"subtract", "-"},
        {"*", "*"}, {"mul", "*"}, {"times", "*"},
        {"/", "/"}, {"div", "/"}, {"divide", "/"},
        {"^", "^"}, {"pow", "^"}, {"power", "^"},
        {".", "."}, {"decimal", "."}, {"dot", "."}, {"point", "."},
        {"(", "("}, {"lparen", "("}, {"leftparen", "("},
        {")", ")"}, {"rparen", ")"}, {"rightparen", ")"},
        {"(-)", "(-)"}, {"neg", "(-)"}, {"negative", "(-)"},
        {"enter", "enter"}, {"return", "enter"},
        {"clear", "clear"}, {"clr", "clear"},
        {"mode", "mode"},
        {"y=", "yequ"}, {"yequ", "yequ"},
        {"window", "window"},
        {"zoom", "zoom"},
        {"trace", "trace"},
        {"graph", "graph"},
        {"2nd", "2nd"}, {"second", "2nd"},
        {"alpha", "alpha"},
        {"math", "math"},
        {"apps", "apps"}, {"app", "apps"},
        {"prgm", "prgm"},
        {"sin", "sin"}, {"cos", "cos"}, {"tan", "tan"},
        {"ln", "ln"}, {"log", "log"},
        {"^2", "^2"}, {"square", "^2"},
        {"-1", "-1"}, {"recip", "-1"},
        {"xton", "xton"}, {"x", "xton"},
        {"left", "left"}, {"right", "right"},
        {"up", "up"}, {"down", "down"},
        {"del", "del"}, {"delete", "del"},
        {",", ","},
        {"sto", "sto"},
        {"sto>", "sto"},
        {"store", "sto"},
        {"storeto", "sto"}
    };
    size_t i;

    if (!input || !out || out_size == 0) {
        return 0;
    }

    for (i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (ascii_casecmp(input, aliases[i].alias) == 0) {
            copy_string(out, out_size, aliases[i].canonical);
            return 1;
        }
    }

    return 0;
}

static long default_delay_for_key(const char *key, long requested_duration_ms) {
    long baseline = DEFAULT_DELAY_GENERAL_MS;

    if (!key) {
        return requested_duration_ms > 0 ? requested_duration_ms : baseline;
    }

    if (ascii_casecmp(key, "enter") == 0) {
        baseline = DEFAULT_DELAY_ENTER_MS;
    } else if (ascii_casecmp(key, "mode") == 0 ||
               ascii_casecmp(key, "yequ") == 0 ||
               ascii_casecmp(key, "window") == 0 ||
               ascii_casecmp(key, "zoom") == 0 ||
               ascii_casecmp(key, "trace") == 0 ||
               ascii_casecmp(key, "graph") == 0 ||
               ascii_casecmp(key, "apps") == 0 ||
               ascii_casecmp(key, "math") == 0 ||
               ascii_casecmp(key, "prgm") == 0) {
        baseline = DEFAULT_DELAY_MENU_MS;
    } else if (ascii_casecmp(key, "clear") == 0) {
        baseline = DEFAULT_DELAY_CLEAR_MS;
    } else if (ascii_casecmp(key, "left") == 0 ||
               ascii_casecmp(key, "right") == 0 ||
               ascii_casecmp(key, "up") == 0 ||
               ascii_casecmp(key, "down") == 0 ||
               ascii_casecmp(key, "del") == 0) {
        baseline = DEFAULT_DELAY_NAV_MS;
    } else if (ascii_casecmp(key, "2nd") == 0 ||
               ascii_casecmp(key, "alpha") == 0) {
        baseline = DEFAULT_DELAY_SHIFT_MS;
    }

    if (requested_duration_ms > baseline) {
        baseline = requested_duration_ms;
    }
    if (baseline < 0) {
        baseline = 0;
    }
    if (baseline > MAX_DELAY_MS) {
        baseline = MAX_DELAY_MS;
    }

    return baseline;
}

static int resolve_autotester_path(char *out_path, size_t out_size, char *out_source, size_t source_size) {
    const char *env_direct = getenv("CEMU_HEADLESS_AUTOTESTER_PATH");
    const char *env_cemu = getenv("CEMU_AUTOTESTER_PATH");
    char home[MAX_PATH_LENGTH];
    const char *candidates[16];
    int candidate_count = 0;
    int i;

    if (env_direct && env_direct[0] != '\0' && file_exists(env_direct)) {
        copy_string(out_path, out_size, env_direct);
        copy_string(out_source, source_size, "env:CEMU_HEADLESS_AUTOTESTER_PATH");
        return 1;
    }

    if (env_cemu && env_cemu[0] != '\0' && file_exists(env_cemu)) {
        copy_string(out_path, out_size, env_cemu);
        copy_string(out_source, source_size, "env:CEMU_AUTOTESTER_PATH");
        return 1;
    }

#if defined(_WIN32)
    {
        char found[MAX_PATH_LENGTH];
        DWORD resolved = SearchPathA(NULL, "autotester.exe", NULL,
                                     (DWORD)sizeof(found), found, NULL);
        if (resolved > 0 && resolved < sizeof(found) && file_exists(found)) {
            copy_string(out_path, out_size, found);
            copy_string(out_source, source_size, "PATH");
            return 1;
        }
    }

    home[0] = '\0';
    {
        const char *user_profile = getenv("USERPROFILE");
        if (user_profile && user_profile[0] != '\0') {
            copy_string(home, sizeof(home), user_profile);
        }
    }

    candidates[candidate_count++] = ".\\tools\\CEmu\\autotester.exe";
    candidates[candidate_count++] = ".\\.tools\\CEmu\\autotester.exe";
    candidates[candidate_count++] = ".\\third_party\\CEmu\\autotester.exe";
    candidates[candidate_count++] = "C:\\Tools\\CEmu\\autotester.exe";
    candidates[candidate_count++] = "C:\\Tools\\CEmu\\v2.0\\autotester.exe";
    candidates[candidate_count++] = "C:\\tools\\cemu\\autotester.exe";
    if (home[0] != '\0') {
        static char home_candidate_1[MAX_PATH_LENGTH];
        static char home_candidate_2[MAX_PATH_LENGTH];
        static char home_candidate_3[MAX_PATH_LENGTH];
        snprintf(home_candidate_1, sizeof(home_candidate_1), "%s\\Tools\\CEmu\\autotester.exe", home);
        snprintf(home_candidate_2, sizeof(home_candidate_2), "%s\\Tools\\CEmu\\v2.0\\autotester.exe", home);
        snprintf(home_candidate_3, sizeof(home_candidate_3), "%s\\Tools\\CEmu\\latest\\autotester.exe", home);
        candidates[candidate_count++] = home_candidate_1;
        candidates[candidate_count++] = home_candidate_2;
        candidates[candidate_count++] = home_candidate_3;
    }
#else
    candidates[candidate_count++] = "./tools/CEmu/autotester.exe";
    candidates[candidate_count++] = "./third_party/CEmu/autotester.exe";
    candidates[candidate_count++] = "/usr/local/bin/autotester.exe";
    candidates[candidate_count++] = "/usr/bin/autotester.exe";
#endif

    for (i = 0; i < candidate_count; i++) {
        if (candidates[i] && candidates[i][0] != '\0' && file_exists(candidates[i])) {
            copy_string(out_path, out_size, candidates[i]);
            copy_string(out_source, source_size, "common_install");
            return 1;
        }
    }

    return 0;
}

static int ensure_autotester_resolved(char *error_message, size_t error_size) {
    char source[64];

    if (g_session.autotester_resolved && file_exists(g_session.autotester_path)) {
        return 1;
    }

    if (resolve_autotester_path(g_session.autotester_path,
                                sizeof(g_session.autotester_path),
                                source,
                                sizeof(source))) {
        g_session.autotester_resolved = 1;
        return 1;
    }

    if (error_message && error_size > 0) {
        snprintf(error_message, error_size,
                 "autotester.exe could not be resolved; set CEMU_HEADLESS_AUTOTESTER_PATH "
                 "or CEMU_AUTOTESTER_PATH, or install it to a known location");
    }
    return 0;
}

static int make_temp_json_path(char *out_path, size_t out_size) {
    char temp_dir[MAX_PATH_LENGTH];

    if (!out_path || out_size == 0) {
        return 0;
    }

#if defined(_WIN32)
    if (GetTempPathA((DWORD)sizeof(temp_dir), temp_dir) == 0) {
        copy_string(temp_dir, sizeof(temp_dir), ".\\");
    }
    snprintf(out_path, out_size,
             "%scemu-headless-%lu-%lu.json",
             temp_dir,
             (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());
#else
    {
        const char *tmpdir = getenv("TMPDIR");
        if (!tmpdir || tmpdir[0] == '\0') {
            tmpdir = "/tmp";
        }
        snprintf(temp_dir, sizeof(temp_dir), "%s/", tmpdir);
    }
    snprintf(out_path, out_size,
             "%scemu-headless-%ld-%ld.json",
             temp_dir,
             (long)getpid(),
             (long)time(NULL));
#endif

    return 1;
}

static int build_autotester_script(const char *json_path, char *error_message, size_t error_size) {
    FILE *file;
    int i;
    char escaped_path[MAX_PATH_LENGTH * 2];

    if (!json_path || json_path[0] == '\0') {
        snprintf(error_message, error_size, "temporary script path was empty");
        return 0;
    }
    if (!g_session.rom_loaded) {
        snprintf(error_message, error_size, "no ROM loaded");
        return 0;
    }
    if (g_session.app_count <= 0) {
        snprintf(error_message, error_size, "no transfer files queued; call load_app first");
        return 0;
    }
    if (g_session.target_name[0] == '\0') {
        snprintf(error_message, error_size,
                 "no launch target could be inferred from queued transfer files");
        return 0;
    }

    file = fopen(json_path, "wb");
    if (!file) {
        snprintf(error_message, error_size, "cannot create temporary autotester script '%s'", json_path);
        return 0;
    }

    fprintf(file, "{\n");
    json_escape_string(g_session.rom_path, escaped_path, (int)sizeof(escaped_path));
    fprintf(file, "  \"rom\": \"%s\",\n", escaped_path);
    fprintf(file, "  \"transfer_files\": [\n");
    for (i = 0; i < g_session.app_count; i++) {
        json_escape_string(g_session.app_paths[i], escaped_path, (int)sizeof(escaped_path));
        fprintf(file, "    \"%s\"%s\n",
                escaped_path,
                (i + 1 < g_session.app_count) ? "," : "");
    }
    fprintf(file, "  ],\n");
    fprintf(file, "  \"target\": {\n");
    fprintf(file, "    \"name\": \"%s\",\n", g_session.target_name);
    fprintf(file, "    \"isASM\": %s\n", g_session.target_is_asm ? "true" : "false");
    fprintf(file, "  },\n");
    fprintf(file, "  \"delay_after_key\": 0,\n");
    fprintf(file, "  \"delay_after_step\": 0,\n");
    fprintf(file, "  \"sequence\": [\n");
    fprintf(file, "    \"action|launch\",\n");
    fprintf(file, "    \"delay|%d\"", DEFAULT_LAUNCH_DELAY_MS);

    for (i = 0; i < g_session.key_count; i++) {
        long key_delay = default_delay_for_key(g_session.key_history[i].key,
                                               g_session.key_history[i].duration_ms);
        fprintf(file, ",\n    \"key|%s\"", g_session.key_history[i].key);
        fprintf(file, ",\n    \"delay|%ld\"", key_delay);
    }

    if (g_session.pending_delay_ms > 0) {
        fprintf(file, ",\n    \"delay|%ld\"", g_session.pending_delay_ms);
    }

    fprintf(file, ",\n    \"hash|%s\"\n", HASH_NAME_CAPTURE);
    fprintf(file, "  ],\n");
    fprintf(file, "  \"hashes\": {\n");
    fprintf(file, "    \"%s\": {\n", HASH_NAME_CAPTURE);
    fprintf(file, "      \"description\": \"cemu-headless wrapper capture\",\n");
    fprintf(file, "      \"start\": \"%s\",\n", HASH_START_NAME);
    fprintf(file, "      \"size\": \"%s\",\n", HASH_SIZE_NAME);
    fprintf(file, "      \"expected_CRCs\": [\"00000000\"],\n");
    fprintf(file, "      \"timeout\": %d\n", DEFAULT_HASH_TIMEOUT_MS);
    fprintf(file, "    }\n");
    fprintf(file, "  }\n");
    fprintf(file, "}\n");

    fclose(file);
    return 1;
}

static int parse_observed_crc_line(const char *line, char *out_crc, size_t out_crc_size) {
    const char *got_pos;
    const char *scan;
    char crc[16];
    size_t o = 0;

    if (!line || !out_crc || out_crc_size == 0) {
        return 0;
    }

    got_pos = strstr(line, "(got ");
    if (!got_pos) {
        got_pos = strstr(line, " got ");
    }
    if (!got_pos) {
        return 0;
    }

    scan = strstr(got_pos, "got ");
    if (!scan) {
        return 0;
    }
    scan += 4;
    if (scan[0] == '0' && (scan[1] == 'x' || scan[1] == 'X')) {
        scan += 2;
    }

    while (*scan != '\0' && isxdigit((unsigned char)*scan) && o < sizeof(crc) - 1) {
        crc[o++] = (char)toupper((unsigned char)*scan);
        scan++;
    }
    crc[o] = '\0';

    if (o == 0) {
        return 0;
    }
    while (o < 8) {
        memmove(crc + 1, crc, o + 1);
        crc[0] = '0';
        o++;
    }

    copy_string(out_crc, out_crc_size, crc);
    return 1;
}

static void append_output_excerpt(char *excerpt, size_t excerpt_size, const char *line) {
    size_t current_length;
    size_t line_length;

    if (!excerpt || excerpt_size == 0 || !line) {
        return;
    }

    current_length = strlen(excerpt);
    line_length = strlen(line);
    if (current_length + line_length + 2 >= excerpt_size) {
        return;
    }

    memcpy(excerpt + current_length, line, line_length);
    current_length += line_length;
    if (current_length == 0 || excerpt[current_length - 1] != '\n') {
        excerpt[current_length++] = '\n';
    }
    excerpt[current_length] = '\0';
}

static int run_autotester_capture(char *observed_crc,
                                  size_t observed_crc_size,
                                  int *exit_code,
                                  char *output_excerpt,
                                  size_t output_excerpt_size,
                                  char *error_message,
                                  size_t error_size) {
    char command[MAX_COMMAND_LENGTH];
    char line[MAX_LINE_LENGTH];
#if defined(_WIN32)
    char output_path[MAX_PATH_LENGTH];
    FILE *output_file;
#else
    FILE *pipe;
#endif

    if (!ensure_autotester_resolved(error_message, error_size)) {
        return 0;
    }

    observed_crc[0] = '\0';
    output_excerpt[0] = '\0';

#if defined(_WIN32)
    snprintf(output_path, sizeof(output_path), "%s.out", g_last_script_path);
    remove(output_path);

    snprintf(command, sizeof(command),
             "cmd.exe /d /c \"\"%s\" \"%s\" > \"%s\" 2>&1\"",
             g_session.autotester_path,
             g_last_script_path,
             output_path);

    *exit_code = system(command);

    output_file = fopen(output_path, "rb");
    if (output_file) {
        while (fgets(line, sizeof(line), output_file) != NULL) {
            char parsed_crc[16];
            append_output_excerpt(output_excerpt, output_excerpt_size, line);
            if (parse_observed_crc_line(line, parsed_crc, sizeof(parsed_crc))) {
                copy_string(observed_crc, observed_crc_size, parsed_crc);
            }
        }
        fclose(output_file);
        remove(output_path);
    }
#else
    snprintf(command, sizeof(command),
             "\"%s\" \"%s\" 2>&1",
             g_session.autotester_path,
             g_last_script_path);

    pipe = CEMU_HEADLESS_POPEN(command, "r");
    if (!pipe) {
        snprintf(error_message, error_size, "failed to launch autotester.exe command");
        return 0;
    }

    while (fgets(line, sizeof(line), pipe) != NULL) {
        char parsed_crc[16];
        append_output_excerpt(output_excerpt, output_excerpt_size, line);
        if (parse_observed_crc_line(line, parsed_crc, sizeof(parsed_crc))) {
            copy_string(observed_crc, observed_crc_size, parsed_crc);
        }
    }

    *exit_code = CEMU_HEADLESS_PCLOSE(pipe);
#endif

    if (observed_crc[0] == '\0') {
        if (output_excerpt[0] != '\0') {
            snprintf(error_message, error_size,
                     "autotester.exe did not report an observed CRC for hash '%s'. Output: %s",
                     HASH_NAME_CAPTURE,
                     output_excerpt);
        } else {
            snprintf(error_message, error_size,
                     "autotester.exe did not report an observed CRC for hash '%s'",
                     HASH_NAME_CAPTURE);
        }
        return 0;
    }

    return 1;
}

static int capture_screen_crc(char *crc_out,
                              size_t crc_out_size,
                              int *autotester_exit_out,
                              char *output_excerpt_out,
                              size_t output_excerpt_size,
                              char *error_message,
                              size_t error_size) {
    int ok;
    int exit_code = 0;

    if (!make_temp_json_path(g_last_script_path, sizeof(g_last_script_path))) {
        snprintf(error_message, error_size, "failed to allocate temporary autotester path");
        return 0;
    }

    if (!build_autotester_script(g_last_script_path, error_message, error_size)) {
        return 0;
    }

    ok = run_autotester_capture(crc_out,
                                crc_out_size,
                                &exit_code,
                                output_excerpt_out,
                                output_excerpt_size,
                                error_message,
                                error_size);

    remove(g_last_script_path);
    g_last_script_path[0] = '\0';

    if (!ok) {
        return 0;
    }

    if (autotester_exit_out) {
        *autotester_exit_out = exit_code;
    }
    return 1;
}

static int write_state_snapshot(const char *path, char *error_message, size_t error_size) {
    FILE *file;
    int i;
    char escaped_path[MAX_PATH_LENGTH * 2];
    char escaped_crc[32];

    file = fopen(path, "wb");
    if (!file) {
        snprintf(error_message, error_size, "cannot create state snapshot '%s'", path);
        return 0;
    }

    fprintf(file, "{\n");
    fprintf(file, "  \"schema_version\": 1,\n");
    fprintf(file, "  \"backend\": \"autotester_wrapper\",\n");
    fprintf(file, "  \"version\": \"%s\",\n", CEMU_HEADLESS_VERSION);
    fprintf(file, "  \"rom_loaded\": %s,\n", g_session.rom_loaded ? "true" : "false");
    json_escape_string(g_session.rom_path, escaped_path, (int)sizeof(escaped_path));
    fprintf(file, "  \"rom_path\": \"%s\",\n", escaped_path);
    json_escape_string(g_session.autotester_path, escaped_path, (int)sizeof(escaped_path));
    fprintf(file, "  \"autotester_path\": \"%s\",\n", escaped_path);
    fprintf(file, "  \"target\": {\"name\": \"%s\", \"isASM\": %s},\n",
            g_session.target_name,
            g_session.target_is_asm ? "true" : "false");
    fprintf(file, "  \"total_cycles\": %ld,\n", g_session.total_cycles);
    fprintf(file, "  \"pending_delay_ms\": %ld,\n", g_session.pending_delay_ms);
    json_escape_string(g_session.last_crc, escaped_crc, (int)sizeof(escaped_crc));
    fprintf(file, "  \"last_crc\": \"%s\",\n", escaped_crc);
    fprintf(file, "  \"transfer_files\": [\n");
    for (i = 0; i < g_session.app_count; i++) {
        json_escape_string(g_session.app_paths[i], escaped_path, (int)sizeof(escaped_path));
        fprintf(file, "    \"%s\"%s\n",
                escaped_path,
                (i + 1 < g_session.app_count) ? "," : "");
    }
    fprintf(file, "  ],\n");
    fprintf(file, "  \"key_history\": [\n");
    for (i = 0; i < g_session.key_count; i++) {
        fprintf(file,
                "    {\"key\": \"%s\", \"duration_ms\": %ld}%s\n",
                g_session.key_history[i].key,
                g_session.key_history[i].duration_ms,
                (i + 1 < g_session.key_count) ? "," : "");
    }
    fprintf(file, "  ]\n");
    fprintf(file, "}\n");

    fclose(file);
    return 1;
}

static void cmd_load_rom(const char *json) {
    char path[MAX_PATH_LENGTH];
    char autotester_path[MAX_PATH_LENGTH];
    char escaped_path[MAX_PATH_LENGTH * 2];
    char escaped_autotester[MAX_PATH_LENGTH * 2];
    char error_message[MAX_ERROR_LENGTH];

    if (!json_get_string(json, "path", path, sizeof(path))) {
        emit_error("load_rom", "missing required field: path");
        return;
    }
    if (!file_exists(path)) {
        emit_error("load_rom", "ROM path '%s' does not exist", path);
        return;
    }

    copy_string(g_session.rom_path, sizeof(g_session.rom_path), path);
    g_session.rom_loaded = 1;
    session_clear_runtime_state();

    if (json_get_string(json, "autotester_path", autotester_path, sizeof(autotester_path))) {
        if (!file_exists(autotester_path)) {
            emit_error("load_rom", "autotester_path '%s' does not exist", autotester_path);
            return;
        }
        copy_string(g_session.autotester_path, sizeof(g_session.autotester_path), autotester_path);
        g_session.autotester_resolved = 1;
    } else if (!ensure_autotester_resolved(error_message, sizeof(error_message))) {
        emit_error("load_rom", "%s", error_message);
        return;
    }

    json_escape_string(g_session.rom_path, escaped_path, (int)sizeof(escaped_path));
    json_escape_string(g_session.autotester_path,
                       escaped_autotester,
                       (int)sizeof(escaped_autotester));
    printf("{\"ok\":true,\"cmd\":\"load_rom\",\"path\":\"%s\","
           "\"backend\":\"autotester_wrapper\","
           "\"autotester_path\":\"%s\"}\n",
           escaped_path,
           escaped_autotester);
    fflush(stdout);
}

static void cmd_load_app(const char *json) {
    char path[MAX_PATH_LENGTH];
    char target_name[MAX_TARGET_NAME_LENGTH];
    char explicit_target_name[MAX_TARGET_NAME_LENGTH];
    int target_name_set = 0;
    int target_is_asm = 1;
    int target_is_asm_set = 0;
    int add_result;
    int added_count = 0;
    char escaped_path[MAX_PATH_LENGTH * 2];
    char escaped_target[MAX_TARGET_NAME_LENGTH * 2];

    if (!json_get_string(json, "path", path, sizeof(path))) {
        emit_error("load_app", "missing required field: path");
        return;
    }
    if (!g_session.rom_loaded) {
        emit_error("load_app", "no ROM loaded; call load_rom first");
        return;
    }

    if (json_get_string(json, "target_name", target_name, sizeof(target_name))) {
        char sanitized_target[MAX_TARGET_NAME_LENGTH];
        normalise_target_name(target_name, sanitized_target, sizeof(sanitized_target));
        copy_string(explicit_target_name, sizeof(explicit_target_name), sanitized_target);
        copy_string(g_session.target_name, sizeof(g_session.target_name), sanitized_target);
        target_name_set = 1;
    }
    if (json_get_bool(json, "target_is_asm", &target_is_asm)) {
        g_session.target_is_asm = target_is_asm ? 1 : 0;
        target_is_asm_set = 1;
    }

    if (has_extension_ci(path, ".8xp") || has_extension_ci(path, ".8xv")) {
        if (!file_exists(path)) {
            emit_error("load_app", "transfer file '%s' does not exist", path);
            return;
        }
        add_result = session_add_app_path(path);
        if (add_result < 0) {
            emit_error("load_app", "transfer queue is full (%d files)", MAX_APPS);
            return;
        }
        added_count = add_result > 0 ? 1 : 0;
    } else {
        int dir_result = session_add_app_directory(path, &added_count);
        if (dir_result < 0) {
            emit_error("load_app", "directory '%s' contains more than %d transfer files", path, MAX_APPS);
            return;
        }
        if (dir_result == 0 || added_count == 0) {
            emit_error("load_app",
                       "path '%s' is neither a .8xp/.8xv file nor a directory containing transfer files",
                       path);
            return;
        }
    }

    if (target_name_set) {
        copy_string(g_session.target_name, sizeof(g_session.target_name), explicit_target_name);
    }
    if (target_is_asm_set) {
        g_session.target_is_asm = target_is_asm ? 1 : 0;
    }
    if (!target_name_set && !target_is_asm_set) {
        session_refresh_default_target();
    }

    json_escape_string(path, escaped_path, (int)sizeof(escaped_path));
    json_escape_string(g_session.target_name, escaped_target, (int)sizeof(escaped_target));
    printf("{\"ok\":true,\"cmd\":\"load_app\",\"path\":\"%s\","
           "\"added_count\":%d,\"app_count\":%d,"
           "\"target_name\":\"%s\",\"target_is_asm\":%s,"
           "\"backend\":\"autotester_wrapper\"}\n",
           escaped_path,
           added_count,
           g_session.app_count,
           escaped_target,
           g_session.target_is_asm ? "true" : "false");
    fflush(stdout);
}

static void cmd_step(const char *json) {
    long cycles = 1000;
    long added_delay_ms;

    json_get_int(json, "cycles", &cycles);
    if (cycles <= 0) {
        cycles = 1;
    }
    if (cycles > 1000000000L) {
        cycles = 1000000000L;
    }

    if (!g_session.rom_loaded) {
        emit_error("step", "no ROM loaded; call load_rom first");
        return;
    }

    added_delay_ms = cycles / CYCLES_PER_MILLISECOND;
    if (added_delay_ms <= 0) {
        added_delay_ms = 1;
    }
    if (added_delay_ms > MAX_DELAY_MS) {
        added_delay_ms = MAX_DELAY_MS;
    }

    g_session.total_cycles += cycles;
    g_session.pending_delay_ms += added_delay_ms;
    if (g_session.pending_delay_ms > MAX_DELAY_MS) {
        g_session.pending_delay_ms = MAX_DELAY_MS;
    }

    printf("{\"ok\":true,\"cmd\":\"step\",\"cycles\":%ld,"
           "\"total_cycles\":%ld,\"approx_delay_ms\":%ld,"
           "\"pending_delay_ms\":%ld,"
           "\"backend\":\"autotester_wrapper\"}\n",
           cycles,
           g_session.total_cycles,
           added_delay_ms,
           g_session.pending_delay_ms);
    fflush(stdout);
}

static void cmd_snapshot(const char *json) {
    char path[MAX_PATH_LENGTH];
    char crc[16];
    char output_excerpt[MAX_OUTPUT_EXCERPT];
    char escaped_path[MAX_PATH_LENGTH * 2];
    char escaped_excerpt[MAX_OUTPUT_EXCERPT * 2];
    char error_message[MAX_ERROR_LENGTH];
    int autotester_exit = 0;

    if (!json_get_string(json, "path", path, sizeof(path))) {
        emit_error("snapshot", "missing required field: path");
        return;
    }
    if (!g_session.rom_loaded) {
        emit_error("snapshot", "no ROM loaded; call load_rom first");
        return;
    }

    if (has_extension_ci(path, ".png")) {
        emit_error("snapshot",
                   "PNG snapshots require a native cemu-core build; wrapper mode only supports CRC capture and state JSON snapshots");
        return;
    }

    if (!write_state_snapshot(path, error_message, sizeof(error_message))) {
        emit_error("snapshot", "%s", error_message);
        return;
    }

    if (!capture_screen_crc(crc,
                            sizeof(crc),
                            &autotester_exit,
                            output_excerpt,
                            sizeof(output_excerpt),
                            error_message,
                            sizeof(error_message))) {
        emit_error("snapshot", "%s", error_message);
        return;
    }

    copy_string(g_session.last_crc, sizeof(g_session.last_crc), crc);
    copy_string(g_session.last_output_excerpt,
                sizeof(g_session.last_output_excerpt),
                output_excerpt);
    g_session.last_autotester_exit = autotester_exit;

    json_escape_string(path, escaped_path, (int)sizeof(escaped_path));
    json_escape_string(output_excerpt, escaped_excerpt, (int)sizeof(escaped_excerpt));
    printf("{\"ok\":true,\"cmd\":\"snapshot\",\"type\":\"state\","
           "\"path\":\"%s\",\"crc\":\"%s\",\"autotester_exit\":%d,"
           "\"backend\":\"autotester_wrapper\",\"output_excerpt\":\"%s\"}\n",
           escaped_path,
           crc,
           autotester_exit,
           escaped_excerpt);
    fflush(stdout);
}

static void cmd_get_screen(const char *json) {
    char format[32];
    char crc[16];
    char output_excerpt[MAX_OUTPUT_EXCERPT];
    char escaped_excerpt[MAX_OUTPUT_EXCERPT * 2];
    char error_message[MAX_ERROR_LENGTH];
    int autotester_exit = 0;

    copy_string(format, sizeof(format), "crc");
    json_get_string(json, "format", format, sizeof(format));

    if (!g_session.rom_loaded) {
        emit_error("get_screen", "no ROM loaded; call load_rom first");
        return;
    }
    if (ascii_casecmp(format, "crc") != 0) {
        emit_error("get_screen",
                   "format '%s' is unavailable in autotester wrapper mode; use format 'crc' until cemu-core is wired",
                   format);
        return;
    }

    if (!capture_screen_crc(crc,
                            sizeof(crc),
                            &autotester_exit,
                            output_excerpt,
                            sizeof(output_excerpt),
                            error_message,
                            sizeof(error_message))) {
        emit_error("get_screen", "%s", error_message);
        return;
    }

    copy_string(g_session.last_crc, sizeof(g_session.last_crc), crc);
    copy_string(g_session.last_output_excerpt,
                sizeof(g_session.last_output_excerpt),
                output_excerpt);
    g_session.last_autotester_exit = autotester_exit;

    json_escape_string(output_excerpt, escaped_excerpt, (int)sizeof(escaped_excerpt));
    printf("{\"ok\":true,\"cmd\":\"get_screen\",\"format\":\"crc\","
           "\"crc\":\"%s\",\"autotester_exit\":%d,"
           "\"backend\":\"autotester_wrapper\",\"output_excerpt\":\"%s\"}\n",
           crc,
           autotester_exit,
           escaped_excerpt);
    fflush(stdout);
}

static void cmd_inject_key(const char *json) {
    char requested_key[MAX_KEY_NAME_LENGTH];
    char canonical_key[MAX_KEY_NAME_LENGTH];
    long duration_ms = 0;

    if (!json_get_string(json, "key", requested_key, sizeof(requested_key))) {
        emit_error("inject_key", "missing required field: key");
        return;
    }
    if (!g_session.rom_loaded) {
        emit_error("inject_key", "no ROM loaded; call load_rom first");
        return;
    }
    if (!normalise_autotester_key(requested_key, canonical_key, sizeof(canonical_key))) {
        emit_error("inject_key", "unsupported key '%s' for autotester wrapper mode", requested_key);
        return;
    }
    json_get_int(json, "duration_ms", &duration_ms);
    if (duration_ms < 0) {
        duration_ms = 0;
    }
    if (duration_ms > MAX_DELAY_MS) {
        duration_ms = MAX_DELAY_MS;
    }
    if (g_session.key_count >= MAX_KEY_EVENTS) {
        emit_error("inject_key", "key history is full (%d events)", MAX_KEY_EVENTS);
        return;
    }

    copy_string(g_session.key_history[g_session.key_count].key,
                sizeof(g_session.key_history[g_session.key_count].key),
                canonical_key);
    g_session.key_history[g_session.key_count].duration_ms = duration_ms;
    g_session.key_count++;

    printf("{\"ok\":true,\"cmd\":\"inject_key\",\"key\":\"%s\","
           "\"duration_ms\":%ld,\"key_count\":%d,"
           "\"backend\":\"autotester_wrapper\"}\n",
           canonical_key,
           duration_ms,
           g_session.key_count);
    fflush(stdout);
}

static void cmd_get_state(void) {
    char escaped_rom[MAX_PATH_LENGTH * 2];
    char escaped_autotester[MAX_PATH_LENGTH * 2];
    char escaped_target[MAX_TARGET_NAME_LENGTH * 2];
    char escaped_last_crc[32];

    json_escape_string(g_session.rom_path, escaped_rom, (int)sizeof(escaped_rom));
    json_escape_string(g_session.autotester_path,
                       escaped_autotester,
                       (int)sizeof(escaped_autotester));
    json_escape_string(g_session.target_name, escaped_target, (int)sizeof(escaped_target));
    json_escape_string(g_session.last_crc, escaped_last_crc, (int)sizeof(escaped_last_crc));

    printf("{\"ok\":true,\"cmd\":\"get_state\","
           "\"backend\":\"%s\","
           "\"rom_loaded\":%s,"
           "\"running\":%s,"
           "\"halted\":%s,"
           "\"total_cycles\":%ld,"
           "\"pending_delay_ms\":%ld,"
           "\"app_count\":%d,"
           "\"key_count\":%d,"
           "\"target_name\":\"%s\","
           "\"target_is_asm\":%s,"
           "\"rom_path\":\"%s\","
           "\"autotester_path\":\"%s\","
           "\"last_crc\":\"%s\","
           "\"last_autotester_exit\":%d,"
           "\"native_mode\":%s,"
           "\"version\":\"%s\"}\n",
           g_session.native_mode_active ? "native_core" : "autotester_wrapper",
           g_session.rom_loaded ? "true" : "false",
           g_session.running ? "true" : "false",
           g_session.halted ? "true" : "false",
           g_session.total_cycles,
           g_session.pending_delay_ms,
           g_session.app_count,
           g_session.key_count,
           escaped_target,
           g_session.target_is_asm ? "true" : "false",
           escaped_rom,
           escaped_autotester,
           escaped_last_crc,
           g_session.last_autotester_exit,
           g_session.native_mode_active ? "true" : "false",
           CEMU_HEADLESS_VERSION);
    fflush(stdout);
}

static void cmd_reset(void) {
    if (!g_session.rom_loaded) {
        emit_error("reset", "no ROM loaded; call load_rom first");
        return;
    }
    session_clear_runtime_state();
    emit_ok("reset",
            "\"backend\":\"autotester_wrapper\",\"note\":\"session replay state cleared; queued transfer files preserved\"");
}

static void cmd_exit(void) {
    emit_ok("exit", "\"note\":\"shutting down\"");
    g_should_exit = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * NATIVE CEmu-CORE MODE — compiled in when CEMU_HEADLESS_HAS_CEMU_CORE=1
 * ═══════════════════════════════════════════════════════════════════════════ */
#if CEMU_HEADLESS_HAS_CEMU_CORE

/* ── GUI callbacks required by CEmu core ─────────────────────────────────── */

void gui_console_clear(void) { /* noop in headless mode */ }

void gui_console_printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
}

void gui_console_err_printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
}

asic_rev_t gui_handle_reset(const boot_ver_t *boot_ver,
                            asic_rev_t loaded_rev,
                            asic_rev_t default_rev,
                            emu_device_t device,
                            bool *python) {
    (void)boot_ver;
    (void)loaded_rev;
    (void)default_rev;
    (void)device;
    (void)python;
    return ASIC_REV_AUTO;
}

#ifdef DEBUG_SUPPORT
void gui_debug_open(int reason, uint32_t data) { (void)reason; (void)data; }
void gui_debug_close(void) {}
#endif

/* ── Watchdog: limits real-time duration of emu_run in step/inject_key ──────
 *
 * Problem: when CalcEngine calls os_RunPrgm() to dispatch to a worker module,
 * or enters a GetKey() busy-wait, emu_run() may spin for seconds of real time
 * before CLOCK_RUN fires enough times.  With a long duration_ms (e.g. 2000 ms)
 * the Python caller times out after 90 s because cemu-headless never replies.
 *
 * Fix: a background thread wakes every 50 ms and calls emu_exit() if the
 * deadline has passed.  emu_exit() sets CPU_SIGNAL_EXIT which causes emu_run()
 * to break on its next iteration.  We then reply to the Python caller with
 * "timed_out": true so it can log a warning and continue rather than crashing.
 *
 * The watchdog is ONLY armed around inject_key and step emu_run calls.
 * Boot (load_rom) and transfer (load_app) emu_run calls are left unguarded
 * because they legitimately take longer real time.
 * ─────────────────────────────────────────────────────────────────────────── */
#if defined(_WIN32)

#define EMU_WATCHDOG_TIMEOUT_MS  10000  /* 10 s real-time max per guarded call */

static volatile LONG  g_wdog_active   = 0;  /* 1 = armed, 0 = idle            */
static volatile LONG  g_wdog_fired    = 0;  /* 1 = watchdog fired this call    */
static volatile LONG  g_wdog_deadline = 0;  /* GetTickCount() expiry           */
static HANDLE         g_wdog_thread   = NULL;

static DWORD WINAPI wdog_proc(LPVOID arg) {
    (void)arg;
    for (;;) {
        if (InterlockedOr(&g_wdog_active, 0)) {
            DWORD now      = GetTickCount();
            DWORD deadline = (DWORD)InterlockedOr(&g_wdog_deadline, 0);
            /* Signed comparison handles 32-bit wrap */
            if ((LONG)(now - deadline) >= 0) {
                InterlockedExchange(&g_wdog_fired, 1);
                emu_exit();                        /* sets CPU_SIGNAL_EXIT      */
                InterlockedExchange(&g_wdog_active, 0); /* don't re-fire       */
            }
        }
        Sleep(50);
    }
    return 0;
}

static void watchdog_global_init(void) {
    g_wdog_thread = CreateThread(NULL, 0, wdog_proc, NULL, 0, NULL);
}

static void wdog_arm(int timeout_ms) {
    InterlockedExchange(&g_wdog_fired, 0);
    InterlockedExchange(&g_wdog_deadline,
                        (LONG)(GetTickCount() + (DWORD)(unsigned int)timeout_ms));
    InterlockedExchange(&g_wdog_active, 1);
}

static void wdog_disarm(void) {
    InterlockedExchange(&g_wdog_active, 0);
}

static int wdog_did_fire(void) {
    return (int)InterlockedOr(&g_wdog_fired, 0);
}

/* Guarded wrapper: arm watchdog, run, disarm */
static void native_emu_run_guarded(uint64_t ticks) {
    wdog_arm(EMU_WATCHDOG_TIMEOUT_MS);
    emu_run(ticks);
    wdog_disarm();
}

#else  /* !_WIN32 — fallback: no watchdog */

static void watchdog_global_init(void) {}
static void native_emu_run_guarded(uint64_t ticks) { emu_run(ticks); }
static int  wdog_did_fire(void) { return 0; }

#endif /* _WIN32 */

/* ── Key name → (row, col) mapping (matches autotester.cpp valid_keys) ────── */

typedef struct { int row; int col; } native_key_pos_t;
typedef struct { const char *name; native_key_pos_t pos; } native_key_entry_t;

static const native_key_entry_t g_native_key_map[] = {
    /* col 1 */
    { "graph",   { 0, 1 } },
    { "trace",   { 1, 1 } },
    { "zoom",    { 2, 1 } },
    { "window",  { 3, 1 } },
    { "y=",      { 4, 1 } },
    { "yequ",    { 4, 1 } }, /* alias */
    { "2nd",     { 5, 1 } },
    { "mode",    { 6, 1 } },
    { "del",     { 7, 1 } },
    /* col 2 */
    { "on",      { 0, 2 } },
    { "sto",     { 1, 2 } },
    { "sto>",    { 1, 2 } }, /* alias */
    { "store",   { 1, 2 } }, /* alias */
    { "storeto", { 1, 2 } }, /* alias */
    { "ln",      { 2, 2 } },
    { "log",     { 3, 2 } },
    { "^2",      { 4, 2 } },
    { "-1",      { 5, 2 } },
    { "math",    { 6, 2 } },
    { "alpha",   { 7, 2 } },
    /* col 3 */
    { "0",       { 0, 3 } },
    { "1",       { 1, 3 } },
    { "4",       { 2, 3 } },
    { "7",       { 3, 3 } },
    { ",",       { 4, 3 } },
    { "sin",     { 5, 3 } },
    { "apps",    { 6, 3 } },
    { "xton",    { 7, 3 } },
    /* col 4 */
    { ".",       { 0, 4 } },
    { "2",       { 1, 4 } },
    { "5",       { 2, 4 } },
    { "8",       { 3, 4 } },
    { "(",       { 4, 4 } },
    { "cos",     { 5, 4 } },
    { "prgm",    { 6, 4 } },
    { "stat",    { 7, 4 } },
    /* col 5 */
    { "(-)",     { 0, 5 } },
    { "3",       { 1, 5 } },
    { "6",       { 2, 5 } },
    { "9",       { 3, 5 } },
    { ")",       { 4, 5 } },
    { "tan",     { 5, 5 } },
    { "vars",    { 6, 5 } },
    /* col 6 */
    { "enter",   { 0, 6 } },
    { "+",       { 1, 6 } },
    { "-",       { 2, 6 } },
    { "*",       { 3, 6 } },
    { "/",       { 4, 6 } },
    { "^",       { 5, 6 } },
    { "clear",   { 6, 6 } },
    /* col 7 */
    { "down",    { 0, 7 } },
    { "left",    { 1, 7 } },
    { "right",   { 2, 7 } },
    { "up",      { 3, 7 } },
};

static const int g_native_key_map_count =
    (int)(sizeof(g_native_key_map) / sizeof(g_native_key_map[0]));

static int native_find_key(const char *name, native_key_pos_t *pos_out) {
    int i;
    /* strip leading "key|" prefix if present */
    if (strncmp(name, "key|", 4) == 0) {
        name += 4;
    }
    for (i = 0; i < g_native_key_map_count; i++) {
        if (ascii_casecmp(g_native_key_map[i].name, name) == 0) {
            *pos_out = g_native_key_map[i].pos;
            return 1;
        }
    }
    return 0;
}

/* ── Simple CRC-32 for LCD framebuffer fingerprinting ─────────────────────── */

static uint32_t native_crc32(const void *data, size_t length) {
    const uint8_t *buf = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;
    for (i = 0; i < length; i++) {
        crc ^= (uint32_t)buf[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

/* ── Native LCD capture ───────────────────────────────────────────────────── */
/* RGBA8888, 320×240 pixels = 307200 bytes */
#define NATIVE_LCD_PIXELS  (LCD_WIDTH * LCD_HEIGHT)
#define NATIVE_LCD_BYTES   (NATIVE_LCD_PIXELS * 4)

static uint32_t g_native_lcd_buf[NATIVE_LCD_PIXELS];

static void native_capture_lcd(void) {
    emu_lcd_drawframe(g_native_lcd_buf);
}

static void native_lcd_crc_string(char *out, size_t out_size) {
    uint32_t crc;
    native_capture_lcd();
    crc = native_crc32(g_native_lcd_buf, NATIVE_LCD_BYTES);
    snprintf(out, out_size, "%08X", (unsigned int)crc);
}

/* ── LCD ASCII text extraction ───────────────────────────────────────────────
 *
 * Extracts printable ASCII text from the 320×240 RGBA8888 LCD framebuffer by
 * matching 8×8 pixel blocks against the graphx default large font table.
 *
 * Font source: graphx default 8-bit font, characters 0x20–0x7E (95 glyphs).
 * Extracted from the GRAPHX.8xv AppVar (CP437 table at offset 9901, starting
 * from character 0x00; printable ASCII begins at index 0x20).
 * Format: 8 bytes per character, MSB = leftmost pixel, 1 = foreground.
 *
 * The extraction scans the buffer in 8-pixel-tall horizontal strips. For each
 * strip, it tries all 8 possible horizontal alignments (0–7) and picks the
 * alignment that produces the most confident font matches. This handles text
 * rendered at non-multiple-of-8 x positions (CalcEngine uses TEXT_MARGIN_X=4
 * and CONTENT_MARGIN_X=6).
 *
 * Pixel thresholding: a pixel is "dark" (foreground) when R+G+B < 384
 * (average below 128 per channel). This cleanly separates graphx black/dark
 * text from white/light backgrounds across all palette entries CalcEngine uses.
 *
 * Match tolerance: ≤ LCD_TEXT_MATCH_ERR_MAX total Hamming-distance bits across
 * all 8 rows. Exact matches score 0; the best-matching character with distance
 * ≤ threshold is selected. A '?' is emitted for unrecognised blocks.
 *
 * ──────────────────────────────────────────────────────────────────────────── */

/* graphx default 8×8 font, characters 0x20 (space) through 0x7E (~).
 * Each row byte: MSB = leftmost pixel, 1 = foreground. */
static const uint8_t g_graphx_font[95][8] = {
    /* 0x20 ' ' */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* 0x21 '!' */ {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0x00, 0xC0, 0x00},
    /* 0x22 '"' */ {0xD8, 0xD8, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* 0x23 '#' */ {0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C, 0x6C, 0x00},
    /* 0x24 '$' */ {0x18, 0x7E, 0xC0, 0x7C, 0x06, 0xFC, 0x18, 0x00},
    /* 0x25 '%' */ {0x00, 0xC6, 0xCC, 0x18, 0x30, 0x66, 0xC6, 0x00},
    /* 0x26 '&' */ {0x38, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0x76, 0x00},
    /* 0x27 '\'' */ {0x30, 0x30, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* 0x28 '(' */ {0x30, 0x60, 0xC0, 0xC0, 0xC0, 0x60, 0x30, 0x00},
    /* 0x29 ')' */ {0xC0, 0x60, 0x30, 0x30, 0x30, 0x60, 0xC0, 0x00},
    /* 0x2A '*' */ {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},
    /* 0x2B '+' */ {0x00, 0x30, 0x30, 0xFC, 0xFC, 0x30, 0x30, 0x00},
    /* 0x2C ',' */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x60, 0xC0},
    /* 0x2D '-' */ {0x00, 0x00, 0x00, 0xFC, 0x00, 0x00, 0x00, 0x00},
    /* 0x2E '.' */ {0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0xC0, 0x00},
    /* 0x2F '/' */ {0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00},
    /* 0x30 '0' */ {0x7C, 0xCE, 0xDE, 0xF6, 0xE6, 0xC6, 0x7C, 0x00},
    /* 0x31 '1' */ {0x30, 0x70, 0x30, 0x30, 0x30, 0x30, 0xFC, 0x00},
    /* 0x32 '2' */ {0x7C, 0xC6, 0x06, 0x7C, 0xC0, 0xC0, 0xFE, 0x00},
    /* 0x33 '3' */ {0xFC, 0x06, 0x06, 0x3C, 0x06, 0x06, 0xFC, 0x00},
    /* 0x34 '4' */ {0x0C, 0xCC, 0xCC, 0xCC, 0xFE, 0x0C, 0x0C, 0x00},
    /* 0x35 '5' */ {0xFE, 0xC0, 0xFC, 0x06, 0x06, 0xC6, 0x7C, 0x00},
    /* 0x36 '6' */ {0x7C, 0xC0, 0xC0, 0xFC, 0xC6, 0xC6, 0x7C, 0x00},
    /* 0x37 '7' */ {0xFE, 0x06, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x00},
    /* 0x38 '8' */ {0x7C, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6, 0x7C, 0x00},
    /* 0x39 '9' */ {0x7C, 0xC6, 0xC6, 0x7E, 0x06, 0x06, 0x7C, 0x00},
    /* 0x3A ':' */ {0x00, 0xC0, 0xC0, 0x00, 0x00, 0xC0, 0xC0, 0x00},
    /* 0x3B ';' */ {0x00, 0x60, 0x60, 0x00, 0x00, 0x60, 0x60, 0xC0},
    /* 0x3C '<' */ {0x18, 0x30, 0x60, 0xC0, 0x60, 0x30, 0x18, 0x00},
    /* 0x3D '=' */ {0x00, 0x00, 0xFC, 0x00, 0xFC, 0x00, 0x00, 0x00},
    /* 0x3E '>' */ {0xC0, 0x60, 0x30, 0x18, 0x30, 0x60, 0xC0, 0x00},
    /* 0x3F '?' */ {0x78, 0xCC, 0x18, 0x30, 0x30, 0x00, 0x30, 0x00},
    /* 0x40 '@' */ {0x7C, 0xC6, 0xDE, 0xDE, 0xDE, 0xC0, 0x7E, 0x00},
    /* 0x41 'A' */ {0x38, 0x6C, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0x00},
    /* 0x42 'B' */ {0xFC, 0xC6, 0xC6, 0xFC, 0xC6, 0xC6, 0xFC, 0x00},
    /* 0x43 'C' */ {0x7C, 0xC6, 0xC0, 0xC0, 0xC0, 0xC6, 0x7C, 0x00},
    /* 0x44 'D' */ {0xF8, 0xCC, 0xC6, 0xC6, 0xC6, 0xCC, 0xF8, 0x00},
    /* 0x45 'E' */ {0xFE, 0xC0, 0xC0, 0xF8, 0xC0, 0xC0, 0xFE, 0x00},
    /* 0x46 'F' */ {0xFE, 0xC0, 0xC0, 0xF8, 0xC0, 0xC0, 0xC0, 0x00},
    /* 0x47 'G' */ {0x7C, 0xC6, 0xC0, 0xC0, 0xCE, 0xC6, 0x7C, 0x00},
    /* 0x48 'H' */ {0xC6, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0x00},
    /* 0x49 'I' */ {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},
    /* 0x4A 'J' */ {0x06, 0x06, 0x06, 0x06, 0x06, 0xC6, 0x7C, 0x00},
    /* 0x4B 'K' */ {0xC6, 0xCC, 0xD8, 0xF0, 0xD8, 0xCC, 0xC6, 0x00},
    /* 0x4C 'L' */ {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xFE, 0x00},
    /* 0x4D 'M' */ {0xC6, 0xEE, 0xFE, 0xFE, 0xD6, 0xC6, 0xC6, 0x00},
    /* 0x4E 'N' */ {0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00},
    /* 0x4F 'O' */ {0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00},
    /* 0x50 'P' */ {0xFC, 0xC6, 0xC6, 0xFC, 0xC0, 0xC0, 0xC0, 0x00},
    /* 0x51 'Q' */ {0x7C, 0xC6, 0xC6, 0xC6, 0xD6, 0xDE, 0x7C, 0x06},
    /* 0x52 'R' */ {0xFC, 0xC6, 0xC6, 0xFC, 0xD8, 0xCC, 0xC6, 0x00},
    /* 0x53 'S' */ {0x7C, 0xC6, 0xC0, 0x7C, 0x06, 0xC6, 0x7C, 0x00},
    /* 0x54 'T' */ {0xFF, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00},
    /* 0x55 'U' */ {0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xFE, 0x00},
    /* 0x56 'V' */ {0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x38, 0x00},
    /* 0x57 'W' */ {0xC6, 0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0x6C, 0x00},
    /* 0x58 'X' */ {0xC6, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0xC6, 0x00},
    /* 0x59 'Y' */ {0xC6, 0xC6, 0xC6, 0x7C, 0x18, 0x30, 0xE0, 0x00},
    /* 0x5A 'Z' */ {0xFE, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xFE, 0x00},
    /* 0x5B '[' */ {0xF0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xF0, 0x00},
    /* 0x5C '\\' */ {0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00},
    /* 0x5D ']' */ {0xF0, 0x30, 0x30, 0x30, 0x30, 0x30, 0xF0, 0x00},
    /* 0x5E '^' */ {0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00},
    /* 0x5F '_' */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},
    /* 0x60 '`' */ {0xC0, 0xC0, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* 0x61 'a' */ {0x00, 0x00, 0x7C, 0x06, 0x7E, 0xC6, 0x7E, 0x00},
    /* 0x62 'b' */ {0xC0, 0xC0, 0xC0, 0xFC, 0xC6, 0xC6, 0xFC, 0x00},
    /* 0x63 'c' */ {0x00, 0x00, 0x7C, 0xC6, 0xC0, 0xC6, 0x7C, 0x00},
    /* 0x64 'd' */ {0x06, 0x06, 0x06, 0x7E, 0xC6, 0xC6, 0x7E, 0x00},
    /* 0x65 'e' */ {0x00, 0x00, 0x7C, 0xC6, 0xFE, 0xC0, 0x7C, 0x00},
    /* 0x66 'f' */ {0x1C, 0x36, 0x30, 0x78, 0x30, 0x30, 0x78, 0x00},
    /* 0x67 'g' */ {0x00, 0x00, 0x7E, 0xC6, 0xC6, 0x7E, 0x06, 0xFC},
    /* 0x68 'h' */ {0xC0, 0xC0, 0xFC, 0xC6, 0xC6, 0xC6, 0xC6, 0x00},
    /* 0x69 'i' */ {0x60, 0x00, 0xE0, 0x60, 0x60, 0x60, 0xF0, 0x00},
    /* 0x6A 'j' */ {0x06, 0x00, 0x06, 0x06, 0x06, 0x06, 0xC6, 0x7C},
    /* 0x6B 'k' */ {0xC0, 0xC0, 0xCC, 0xD8, 0xF8, 0xCC, 0xC6, 0x00},
    /* 0x6C 'l' */ {0xE0, 0x60, 0x60, 0x60, 0x60, 0x60, 0xF0, 0x00},
    /* 0x6D 'm' */ {0x00, 0x00, 0xCC, 0xFE, 0xFE, 0xD6, 0xD6, 0x00},
    /* 0x6E 'n' */ {0x00, 0x00, 0xFC, 0xC6, 0xC6, 0xC6, 0xC6, 0x00},
    /* 0x6F 'o' */ {0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0x7C, 0x00},
    /* 0x70 'p' */ {0x00, 0x00, 0xFC, 0xC6, 0xC6, 0xFC, 0xC0, 0xC0},
    /* 0x71 'q' */ {0x00, 0x00, 0x7E, 0xC6, 0xC6, 0x7E, 0x06, 0x06},
    /* 0x72 'r' */ {0x00, 0x00, 0xFC, 0xC6, 0xC0, 0xC0, 0xC0, 0x00},
    /* 0x73 's' */ {0x00, 0x00, 0x7E, 0xC0, 0x7C, 0x06, 0xFC, 0x00},
    /* 0x74 't' */ {0x30, 0x30, 0xFC, 0x30, 0x30, 0x30, 0x1C, 0x00},
    /* 0x75 'u' */ {0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0x7E, 0x00},
    /* 0x76 'v' */ {0x00, 0x00, 0xC6, 0xC6, 0xC6, 0x7C, 0x38, 0x00},
    /* 0x77 'w' */ {0x00, 0x00, 0xC6, 0xC6, 0xD6, 0xFE, 0x6C, 0x00},
    /* 0x78 'x' */ {0x00, 0x00, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0x00},
    /* 0x79 'y' */ {0x00, 0x00, 0xC6, 0xC6, 0xC6, 0x7E, 0x06, 0xFC},
    /* 0x7A 'z' */ {0x00, 0x00, 0xFE, 0x0C, 0x38, 0x60, 0xFE, 0x00},
    /* 0x7B '{' */ {0x1C, 0x30, 0x30, 0xE0, 0x30, 0x30, 0x1C, 0x00},
    /* 0x7C '|' */ {0xC0, 0xC0, 0xC0, 0x00, 0xC0, 0xC0, 0xC0, 0x00},
    /* 0x7D '}' */ {0xE0, 0x30, 0x30, 0x1C, 0x30, 0x30, 0xE0, 0x00},
    /* 0x7E '~' */ {0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

/* OCR parameters */
#define LCD_FONT_W          8    /* graphx large font character width, pixels  */
#define LCD_FONT_H          8    /* graphx large font character height, pixels */
#define LCD_TEXT_BUF_SIZE   4096 /* max get_screen_text output bytes           */
/* Maximum total Hamming-distance bits (across all 8 rows) for a font match.
 * 0 = exact match only; 16 = up to 2 wrong pixels per row on average. */
#define LCD_TEXT_MATCH_ERR  16

/* Bit-count of set bits in a byte (Brian Kernighan / lookup approach).
 * Avoids relying on __builtin_popcount which may be absent in C99 mode. */
static int popcount8(uint8_t b) {
    b = (uint8_t)(b - ((b >> 1) & 0x55u));
    b = (uint8_t)((b & 0x33u) + ((b >> 2) & 0x33u));
    return (int)(((b + (b >> 4)) & 0x0Fu));
}

/* Sample one row of an 8×8 cell from the RGBA8888 framebuffer.
 * Returns a byte where bit (7-col) = 1 if the pixel at column col is "dark"
 * (R+G+B < 384, i.e. average brightness below 128 per channel).
 * buf    : RGBA8888 buffer, LCD_WIDTH×LCD_HEIGHT uint32 values in ARGB8888 format
 *          produced by emu_lcd_drawframe(). Each uint32 = 0xAARRGGBB.
 * cx, cy : top-left corner of the 8×8 cell in screen coordinates.
 * row    : which row within the cell (0 = top).
 */
static uint8_t lcd_sample_row(const uint32_t *buf, int cx, int cy, int row) {
    uint8_t result = 0;
    int col;
    int py = cy + row;
    if (py < 0 || py >= LCD_HEIGHT) { return 0; }
    for (col = 0; col < LCD_FONT_W; col++) {
        int px = cx + col;
        if (px >= 0 && px < LCD_WIDTH) {
            uint32_t pixel = buf[(size_t)py * LCD_WIDTH + (size_t)px];
            /* ARGB8888: A=bits31:24, R=bits23:16, G=bits15:8, B=bits7:0 */
            int r = (int)((pixel >> 16) & 0xFFu);
            int g = (int)((pixel >>  8) & 0xFFu);
            int b = (int)(pixel         & 0xFFu);
            if (r + g + b < 384) {
                result |= (uint8_t)(1u << (7 - col));
            }
        }
    }
    return result;
}

/* Match an 8×8 cell at (cx, cy) against all 95 printable ASCII font glyphs.
 * Returns the best-match character code (0x20–0x7E), or ' ' if no glyph
 * achieves a Hamming distance ≤ LCD_TEXT_MATCH_ERR.
 * Also writes the distance to *out_dist when out_dist != NULL.
 */
static char lcd_match_char(const uint32_t *buf, int cx, int cy, int *out_dist) {
    int best_idx  = 0;   /* index into g_graphx_font, 0 = space */
    int best_dist = (LCD_TEXT_MATCH_ERR + 1) * 10; /* worse than any real match */
    int ci, ri;

    for (ci = 0; ci < 95; ci++) {
        int dist = 0;
        for (ri = 0; ri < LCD_FONT_H; ri++) {
            uint8_t pixel_row = lcd_sample_row(buf, cx, cy, ri);
            uint8_t font_row  = g_graphx_font[ci][ri];
            dist += popcount8((uint8_t)(pixel_row ^ font_row));
        }
        if (dist < best_dist) {
            best_dist = dist;
            best_idx  = ci;
        }
    }

    if (out_dist) { *out_dist = best_dist; }
    if (best_dist > LCD_TEXT_MATCH_ERR) { return ' '; }
    return (char)(0x20 + best_idx);
}

/* Extract printable ASCII text from the current framebuffer (already in
 * g_native_lcd_buf via a prior native_capture_lcd() call).
 *
 * Scans the 320×240 image in LCD_FONT_H-pixel-tall horizontal strips. For
 * each strip, tries all LCD_FONT_W horizontal starting offsets (0..7) and
 * picks the one that produces the most confident glyph matches (lowest total
 * Hamming distance over all cells in the strip). Empty strips are omitted.
 * Strips are separated by newlines in the output.
 *
 * out      : destination buffer
 * out_size : capacity of out (including NUL terminator)
 * Returns  : number of characters written (excluding NUL).
 */
static int native_lcd_extract_text(char *out, int out_size) {
    int y_strip, x_off, ci;
    int out_pos    = 0;
    int first_line = 1;

    /* Scratch buffers: one row of characters per alignment attempt */
    /* Max cols = 320/8 + 1 = 41 */
#define LCD_MAX_COLS 41
    char row_buf[LCD_MAX_COLS + 1];
    char best_buf[LCD_MAX_COLS + 1];

    for (y_strip = 0; y_strip + LCD_FONT_H <= LCD_HEIGHT; y_strip += LCD_FONT_H) {
        int best_score = -1;

        /* Try each of the 8 possible horizontal alignment offsets */
        for (x_off = 0; x_off < LCD_FONT_W; x_off++) {
            int x, buf_pos = 0, score = 0;

            for (x = x_off; x + LCD_FONT_W <= LCD_WIDTH; x += LCD_FONT_W) {
                int dist = 0;
                char c = lcd_match_char(g_native_lcd_buf, x, y_strip, &dist);
                row_buf[buf_pos++] = c;
                /* Score = sum of confidence (lower distance = higher score).
                 * Only non-space characters with good distance contribute.  */
                if (c != ' ' && dist <= LCD_TEXT_MATCH_ERR) {
                    score += (LCD_TEXT_MATCH_ERR - dist + 1);
                }
            }
            row_buf[buf_pos] = '\0';

            if (score > best_score) {
                best_score = score;
                memcpy(best_buf, row_buf, (size_t)(buf_pos + 1));
            }
        }

        /* Trim trailing spaces from the best row */
        {
            int len = (int)strlen(best_buf);
            while (len > 0 && best_buf[len - 1] == ' ') { len--; }
            best_buf[len] = '\0';

            if (len > 0) {
                if (!first_line) {
                    if (out_pos < out_size - 1) { out[out_pos++] = '\n'; }
                }
                for (ci = 0; ci < len && out_pos < out_size - 1; ci++) {
                    out[out_pos++] = best_buf[ci];
                }
                first_line = 0;
            }
        }
    }
#undef LCD_MAX_COLS

    out[out_pos] = '\0';
    return out_pos;
}

/* ── Native command implementations ─────────────────────────────────────────
 * These mirror the wrapper cmd_* functions but call the CEmu core directly.
 * ────────────────────────────────────────────────────────────────────────── */

/* emu_run() ticks are in CLOCK_RUN (60 Hz) units.
 *   1 tick  = 1/60 s ≈ 16.67 ms
 *   60 ticks = 1 second of emulated time
 *
 * These values are intentionally short for headless use; the emulator runs
 * unthrottled so each tick is executed as fast as the host CPU allows.
 */
#define NATIVE_RUN_HZ           60u
#define NATIVE_MS_TO_TICKS(ms)  ((uint64_t)((ms) * NATIVE_RUN_HZ + 999u) / 1000u)
#define NATIVE_BOOT_TICKS       NATIVE_MS_TO_TICKS(6000)  /* 360 ticks = 6 s  */
#define NATIVE_TRANSFER_TICKS   NATIVE_MS_TO_TICKS(3000)  /* 180 ticks = 3 s  */
#define NATIVE_KEY_HOLD_TICKS   NATIVE_MS_TO_TICKS(50)    /*   3 ticks = 50ms */
#define NATIVE_KEY_POST_TICKS   NATIVE_MS_TO_TICKS(250)   /*  15 ticks = 250ms*/

static void native_cmd_load_rom(const char *json) {
    char path[MAX_PATH_LENGTH];
    emu_state_t state;

    if (!json_get_string(json, "path", path, sizeof(path))) {
        emit_error("load_rom", "missing required field: path");
        return;
    }
    if (!file_exists(path)) {
        emit_error("load_rom", "ROM path '%s' does not exist", path);
        return;
    }

    state = emu_load(EMU_DATA_ROM, path);
    if (state != EMU_STATE_VALID) {
        emit_error("load_rom",
                   "emu_load failed for '%s' (state=%d; expected EMU_STATE_VALID)",
                   path, (int)state);
        return;
    }

    /* Boot the OS */
    emu_run(NATIVE_BOOT_TICKS);

    copy_string(g_session.rom_path, sizeof(g_session.rom_path), path);
    g_session.rom_loaded = 1;
    g_session.running = 1;
    g_session.native_mode_active = 1;

    {
        char escaped_path[MAX_PATH_LENGTH * 2];
        json_escape_string(path, escaped_path, (int)sizeof(escaped_path));
        printf("{\"ok\":true,\"cmd\":\"load_rom\",\"path\":\"%s\","
               "\"backend\":\"native_core\",\"boot_ticks\":%llu}\n",
               escaped_path,
               (unsigned long long)NATIVE_BOOT_TICKS);
    }
    fflush(stdout);
}

static void native_cmd_load_app(const char *json) {
    char path[MAX_PATH_LENGTH];
    char target_name[MAX_TARGET_NAME_LENGTH];
    char explicit_target_name[MAX_TARGET_NAME_LENGTH];
    char previous_target_name[MAX_TARGET_NAME_LENGTH];
    int previous_target_is_asm;
    int previous_app_count;
    int target_name_set = 0;
    int target_is_asm = 1;
    int target_is_asm_set = 0;
    int added_count = 0;
    int result;
    int i;

    if (!json_get_string(json, "path", path, sizeof(path))) {
        emit_error("load_app", "missing required field: path");
        return;
    }
    if (!g_session.rom_loaded) {
        emit_error("load_app", "no ROM loaded; call load_rom first");
        return;
    }

    previous_app_count = g_session.app_count;
    previous_target_is_asm = g_session.target_is_asm;
    copy_string(previous_target_name, sizeof(previous_target_name), g_session.target_name);

    if (json_get_string(json, "target_name", target_name, sizeof(target_name))) {
        char sanitized_target[MAX_TARGET_NAME_LENGTH];
        normalise_target_name(target_name, sanitized_target, sizeof(sanitized_target));
        copy_string(explicit_target_name, sizeof(explicit_target_name), sanitized_target);
        copy_string(g_session.target_name, sizeof(g_session.target_name), sanitized_target);
        target_name_set = 1;
    }
    if (json_get_bool(json, "target_is_asm", &target_is_asm)) {
        g_session.target_is_asm = target_is_asm ? 1 : 0;
        target_is_asm_set = 1;
    }

    if (has_extension_ci(path, ".8xp") || has_extension_ci(path, ".8xv")) {
        int add_result;
        if (!file_exists(path)) {
            emit_error("load_app", "transfer file '%s' does not exist", path);
            return;
        }
        add_result = session_add_app_path(path);
        if (add_result < 0) {
            emit_error("load_app", "transfer queue is full (%d files)", MAX_APPS);
            return;
        }
        added_count = add_result > 0 ? 1 : 0;
    } else {
        int dir_result = session_add_app_directory(path, &added_count);
        if (dir_result < 0) {
            emit_error("load_app", "directory '%s' contains more than %d transfer files", path, MAX_APPS);
            return;
        }
        if (dir_result == 0 || added_count == 0) {
            emit_error("load_app",
                       "path '%s' is neither a .8xp/.8xv file nor a directory containing transfer files",
                       path);
            return;
        }
    }

    if (target_name_set) {
        copy_string(g_session.target_name, sizeof(g_session.target_name), explicit_target_name);
    }
    if (target_is_asm_set) {
        g_session.target_is_asm = target_is_asm ? 1 : 0;
    }
    if (!target_name_set && !target_is_asm_set) {
        session_refresh_default_target();
    }

    for (i = previous_app_count; i < g_session.app_count; i++) {
        result = emu_send_variable(g_session.app_paths[i], LINK_ARCH);
        if (result != LINK_GOOD) {
            g_session.app_count = previous_app_count;
            copy_string(g_session.target_name, sizeof(g_session.target_name), previous_target_name);
            g_session.target_is_asm = previous_target_is_asm;
            emit_error("load_app",
                       "emu_send_variable failed for '%s' (result=%d; "
                       "LINK_GOOD=0, LINK_WARN=1, LINK_ERR=2)",
                       g_session.app_paths[i], result);
            return;
        }
        emu_run(NATIVE_TRANSFER_TICKS);
    }

    {
        char escaped_path[MAX_PATH_LENGTH * 2];
        char escaped_target[MAX_TARGET_NAME_LENGTH * 2];
        json_escape_string(path, escaped_path, (int)sizeof(escaped_path));
        json_escape_string(g_session.target_name, escaped_target, (int)sizeof(escaped_target));
        printf("{\"ok\":true,\"cmd\":\"load_app\",\"path\":\"%s\","
               "\"added_count\":%d,\"app_count\":%d,"
               "\"target_name\":\"%s\",\"target_is_asm\":%s,"
               "\"backend\":\"native_core\"}\n",
               escaped_path,
               added_count,
               g_session.app_count,
               escaped_target,
               g_session.target_is_asm ? "true" : "false");
    }
    fflush(stdout);
}

static void native_cmd_step(const char *json) {
    long cycles_ms = 500; /* default 500 ms */
    uint64_t ticks;

    json_get_int(json, "cycles_ms", &cycles_ms);
    if (cycles_ms <= 0) { cycles_ms = 1; }
    if (cycles_ms > MAX_DELAY_MS) { cycles_ms = MAX_DELAY_MS; }

    if (!g_session.rom_loaded) {
        emit_error("step", "no ROM loaded; call load_rom first");
        return;
    }

    ticks = NATIVE_MS_TO_TICKS(cycles_ms);
    if (ticks == 0) { ticks = 1; }
    native_emu_run_guarded(ticks);
    g_session.total_cycles += cycles_ms * (long)CYCLES_PER_MILLISECOND;

    printf("{\"ok\":true,\"cmd\":\"step\",\"cycles_ms\":%ld,"
           "\"total_cycles\":%ld,\"timed_out\":%s,\"backend\":\"native_core\"}\n",
           cycles_ms, g_session.total_cycles,
           wdog_did_fire() ? "true" : "false");
    fflush(stdout);
}

static void native_cmd_inject_key(const char *json) {
    char key_name[MAX_KEY_NAME_LENGTH];
    long duration_ms = DEFAULT_DELAY_GENERAL_MS;
    native_key_pos_t pos;
    uint64_t press_ticks;
    uint64_t release_ticks;

    if (!json_get_string(json, "key", key_name, sizeof(key_name))) {
        emit_error("inject_key", "missing required field: key");
        return;
    }
    if (!g_session.rom_loaded) {
        emit_error("inject_key", "no ROM loaded; call load_rom first");
        return;
    }

    json_get_int(json, "duration_ms", &duration_ms);
    if (duration_ms <= 0) { duration_ms = DEFAULT_DELAY_GENERAL_MS; }
    if (duration_ms > MAX_DELAY_MS) { duration_ms = MAX_DELAY_MS; }

    if (!native_find_key(key_name, &pos)) {
        emit_error("inject_key", "unknown key '%s'", key_name);
        return;
    }

    /* Press key, hold, then release */
    press_ticks   = NATIVE_KEY_HOLD_TICKS;
    release_ticks = NATIVE_MS_TO_TICKS(duration_ms);
    if (release_ticks == 0) { release_ticks = 1; }

    /* The autotester map stores {x, y} but calls emu_keypad_event(y, x).
     * pos.row = x (first number), pos.col = y (second number).
     * We must pass (col, row) = (y, x) to match the CEmu core convention. */
    emu_keypad_event((unsigned int)pos.col, (unsigned int)pos.row, 1 /* press */);
    native_emu_run_guarded(press_ticks);
    emu_keypad_event((unsigned int)pos.col, (unsigned int)pos.row, 0 /* release */);
    native_emu_run_guarded(release_ticks);

    /* Convert CLOCK_RUN ticks back to milliseconds for total_cycles tracking */
    g_session.total_cycles += (long)((press_ticks + release_ticks) * 1000 / NATIVE_RUN_HZ * CYCLES_PER_MILLISECOND);

    if (g_session.key_count < MAX_KEY_EVENTS) {
        copy_string(g_session.key_history[g_session.key_count].key,
                    MAX_KEY_NAME_LENGTH, key_name);
        g_session.key_history[g_session.key_count].duration_ms = duration_ms;
        g_session.key_count++;
    }

    printf("{\"ok\":true,\"cmd\":\"inject_key\",\"key\":\"%s\","
           "\"emu_row\":%d,\"emu_col\":%d,\"duration_ms\":%ld,"
           "\"timed_out\":%s,\"backend\":\"native_core\"}\n",
           key_name, pos.col, pos.row, duration_ms,
           wdog_did_fire() ? "true" : "false");
    fflush(stdout);
}

static void native_cmd_get_screen(const char *json) {
    char format[32];
    char crc_str[16];

    copy_string(format, sizeof(format), "crc");
    json_get_string(json, "format", format, sizeof(format));

    if (!g_session.rom_loaded) {
        emit_error("get_screen", "no ROM loaded; call load_rom first");
        return;
    }

    if (ascii_casecmp(format, "png") == 0) {
#if CEMU_HEADLESS_PNG_EXPORT
        char path[MAX_PATH_LENGTH];
        char escaped_path[MAX_PATH_LENGTH * 2];

        if (!json_get_string(json, "path", path, sizeof(path))) {
            emit_error("get_screen", "format 'png' requires a 'path' field");
            return;
        }
        native_capture_lcd();
        /* RGBA8888 → 4 channels, 320×240 */
        if (!stbi_write_png(path, LCD_WIDTH, LCD_HEIGHT, 4,
                            g_native_lcd_buf, LCD_WIDTH * 4)) {
            emit_error("get_screen", "stbi_write_png failed writing to '%s'", path);
            return;
        }
        native_lcd_crc_string(crc_str, sizeof(crc_str));
        json_escape_string(path, escaped_path, (int)sizeof(escaped_path));
        printf("{\"ok\":true,\"cmd\":\"get_screen\",\"format\":\"png\","
               "\"path\":\"%s\",\"crc\":\"%s\",\"backend\":\"native_core\"}\n",
               escaped_path, crc_str);
        fflush(stdout);
#else
        emit_error("get_screen",
                   "PNG export requires stb_image_write.h in vendor/; "
                   "rebuild with -DCEMU_HEADLESS_USE_STB_IMAGE=ON after placing the header");
#endif
        return;
    }

    if (ascii_casecmp(format, "crc") == 0) {
        native_lcd_crc_string(crc_str, sizeof(crc_str));
        printf("{\"ok\":true,\"cmd\":\"get_screen\",\"format\":\"crc\","
               "\"crc\":\"%s\",\"backend\":\"native_core\"}\n",
               crc_str);
        fflush(stdout);
        return;
    }

    if (ascii_casecmp(format, "text") == 0) {
        char text_buf[LCD_TEXT_BUF_SIZE];
        char escaped_text[LCD_TEXT_BUF_SIZE * 2];
        native_capture_lcd();
        native_lcd_extract_text(text_buf, sizeof(text_buf));
        native_lcd_crc_string(crc_str, sizeof(crc_str));
        json_escape_string(text_buf, escaped_text, (int)sizeof(escaped_text));
        printf("{\"ok\":true,\"cmd\":\"get_screen\",\"format\":\"text\","
               "\"text\":\"%s\",\"crc\":\"%s\",\"backend\":\"native_core\"}\n",
               escaped_text, crc_str);
        fflush(stdout);
        return;
    }

    emit_error("get_screen", "unknown format '%s'; supported: 'crc', 'png', 'text'", format);
}

static void native_cmd_snapshot(const char *json) {
    char path[MAX_PATH_LENGTH];
    char escaped_path[MAX_PATH_LENGTH * 2];
    char crc_str[16];

    if (!json_get_string(json, "path", path, sizeof(path))) {
        emit_error("snapshot", "missing required field: path");
        return;
    }
    if (!g_session.rom_loaded) {
        emit_error("snapshot", "no ROM loaded; call load_rom first");
        return;
    }

    if (has_extension_ci(path, ".png")) {
#if CEMU_HEADLESS_PNG_EXPORT
        native_capture_lcd();
        if (!stbi_write_png(path, LCD_WIDTH, LCD_HEIGHT, 4,
                            g_native_lcd_buf, LCD_WIDTH * 4)) {
            emit_error("snapshot", "stbi_write_png failed writing to '%s'", path);
            return;
        }
        native_lcd_crc_string(crc_str, sizeof(crc_str));
        json_escape_string(path, escaped_path, (int)sizeof(escaped_path));
        printf("{\"ok\":true,\"cmd\":\"snapshot\",\"type\":\"png\","
               "\"path\":\"%s\",\"crc\":\"%s\",\"backend\":\"native_core\"}\n",
               escaped_path, crc_str);
        fflush(stdout);
#else
        emit_error("snapshot",
                   "PNG snapshots require stb_image_write.h in vendor/; "
                   "rebuild with -DCEMU_HEADLESS_USE_STB_IMAGE=ON after placing the header");
#endif
        return;
    }

    if (has_extension_ci(path, ".json")) {
        char snap_err[MAX_ERROR_LENGTH];
        snap_err[0] = '\0';
        if (!write_state_snapshot(path, snap_err, sizeof(snap_err))) {
            emit_error("snapshot", "failed to write state snapshot: %s",
                       snap_err[0] ? snap_err : "unknown error");
            return;
        }
        native_lcd_crc_string(crc_str, sizeof(crc_str));
        json_escape_string(path, escaped_path, (int)sizeof(escaped_path));
        printf("{\"ok\":true,\"cmd\":\"snapshot\",\"type\":\"state\","
               "\"path\":\"%s\",\"crc\":\"%s\",\"backend\":\"native_core\"}\n",
               escaped_path, crc_str);
        fflush(stdout);
        return;
    }

    emit_error("snapshot",
               "unsupported snapshot extension for '%s'; use .png or .json",
               path);
}

static void native_cmd_reset(void) {
    if (!g_session.rom_loaded) {
        emit_error("reset", "no ROM loaded; call load_rom first");
        return;
    }
    emu_reset();
    emu_run(NATIVE_BOOT_TICKS);
    printf("{\"ok\":true,\"cmd\":\"reset\",\"backend\":\"native_core\"}\n");
    fflush(stdout);
}

#endif /* CEMU_HEADLESS_HAS_CEMU_CORE */

/* ═══════════════════════════════════════════════════════════════════════════
 * Command dispatcher — routes to native or wrapper implementations
 * ═══════════════════════════════════════════════════════════════════════════ */
static void dispatch_command(const char *line) {
    char cmd[64];
    const char *scan = line;

    g_command_failed = 0;

    while (*scan == ' ' || *scan == '\t' || *scan == '\r' || *scan == '\n') {
        scan++;
    }
    if (*scan == '\0' || *scan == '#') {
        return;
    }

    if (!json_get_string(line, "cmd", cmd, sizeof(cmd))) {
        emit_error("unknown",
                   "missing required field: cmd (expected JSON object with \"cmd\" key)");
        return;
    }

#if CEMU_HEADLESS_HAS_CEMU_CORE
    /* In native mode, all commands use the CEmu core directly. */
    if (strcmp(cmd, "load_rom") == 0) {
        native_cmd_load_rom(line);
    } else if (strcmp(cmd, "load_app") == 0) {
        native_cmd_load_app(line);
    } else if (strcmp(cmd, "step") == 0) {
        native_cmd_step(line);
    } else if (strcmp(cmd, "snapshot") == 0) {
        native_cmd_snapshot(line);
    } else if (strcmp(cmd, "get_screen") == 0) {
        native_cmd_get_screen(line);
    } else if (strcmp(cmd, "inject_key") == 0) {
        native_cmd_inject_key(line);
    } else if (strcmp(cmd, "get_state") == 0) {
        cmd_get_state();
    } else if (strcmp(cmd, "reset") == 0) {
        native_cmd_reset();
    } else if (strcmp(cmd, "exit") == 0) {
        cmd_exit();
    } else {
        emit_error(cmd, "unknown command '%s'", cmd);
    }
#else
    /* Wrapper mode — fall through to autotester relay implementations. */
    if (strcmp(cmd, "load_rom") == 0) {
        cmd_load_rom(line);
    } else if (strcmp(cmd, "load_app") == 0) {
        cmd_load_app(line);
    } else if (strcmp(cmd, "step") == 0) {
        cmd_step(line);
    } else if (strcmp(cmd, "snapshot") == 0) {
        cmd_snapshot(line);
    } else if (strcmp(cmd, "get_screen") == 0) {
        cmd_get_screen(line);
    } else if (strcmp(cmd, "inject_key") == 0) {
        cmd_inject_key(line);
    } else if (strcmp(cmd, "get_state") == 0) {
        cmd_get_state();
    } else if (strcmp(cmd, "reset") == 0) {
        cmd_reset();
    } else if (strcmp(cmd, "exit") == 0) {
        cmd_exit();
    } else {
        emit_error(cmd, "unknown command '%s'", cmd);
    }
#endif
}

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "cemu-headless %s\n"
            "\n"
            "Usage:\n"
            "  %s [--version] [--help] [script.json]\n"
            "\n"
            "Modes:\n"
            "  Interactive:\n"
            "    Reads JSON command lines from stdin and writes JSON responses to stdout.\n"
            "\n"
            "  Script:\n"
            "    Provide a file containing one JSON command per line.\n"
            "    Exit code 0 = success, 1 = command failure, 2 = startup error.\n"
            "\n"
            "Backend:\n"
            "  This build runs as an autotester.exe-backed wrapper because cemu-core is\n"
            "  not bundled in the current tree. get_screen(format=crc) replays the current\n"
            "  session into a temporary AutoTester config and returns the observed CRC.\n"
            "\n"
            "Supported commands:\n"
            "  load_rom   {\"cmd\":\"load_rom\",\"path\":\"rom/Rom.rom\",\"autotester_path\":\"C:/.../autotester.exe\"}\n"
            "  load_app   {\"cmd\":\"load_app\",\"path\":\"C:/.../CALCENG.8xp\"}\n"
            "  load_app   {\"cmd\":\"load_app\",\"path\":\"C:/Users/user/Downloads/CalcEngine\"}\n"
            "  step       {\"cmd\":\"step\",\"cycles\":480000}\n"
            "  inject_key {\"cmd\":\"inject_key\",\"key\":\"2\"}\n"
            "  get_screen {\"cmd\":\"get_screen\",\"format\":\"crc\"}\n"
            "  get_screen {\"cmd\":\"get_screen\",\"format\":\"text\"}\n"
            "  snapshot   {\"cmd\":\"snapshot\",\"path\":\"session-state.json\"}\n"
            "  get_state  {\"cmd\":\"get_state\"}\n"
            "  reset      {\"cmd\":\"reset\"}\n"
            "  exit       {\"cmd\":\"exit\"}\n"
            "\n"
            "Current limits in wrapper mode:\n"
            "  - get_screen(format=text) is unavailable in wrapper mode\n"
            "  - snapshot(path=.png) is unavailable\n"
            "  - true emulator RAM state save/restore requires native cemu-core wiring\n",
            CEMU_HEADLESS_VERSION,
            argv0);
}

int main(int argc, char *argv[]) {
    char line_buf[INPUT_BUFFER_SIZE];
    FILE *input_stream = stdin;
    int script_mode = 0;
    int any_error = 0;
    int i;

    memset(&g_session, 0, sizeof(g_session));
    g_last_script_path[0] = '\0';

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("%s\n", CEMU_HEADLESS_VERSION);
            return 0;
        }
        if (argv[i][0] != '-') {
            input_stream = fopen(argv[i], "r");
            if (!input_stream) {
                fprintf(stderr, "cemu-headless: cannot open script file '%s'\n", argv[i]);
                return 2;
            }
            script_mode = 1;
            continue;
        }
        fprintf(stderr, "cemu-headless: unknown option '%s'. Use --help for usage.\n", argv[i]);
        return 2;
    }

    fprintf(stderr, "cemu-headless %s ready (backend=%s)\n",
            CEMU_HEADLESS_VERSION,
#if CEMU_HEADLESS_HAS_CEMU_CORE
            "native_core"
#else
            "autotester_wrapper"
#endif
            );
    fflush(stderr);

#if CEMU_HEADLESS_HAS_CEMU_CORE
    /* Start watchdog thread that prevents emu_run from blocking indefinitely */
    watchdog_global_init();
#endif

    while (!g_should_exit && fgets(line_buf, sizeof(line_buf), input_stream) != NULL) {
        int length = (int)strlen(line_buf);

        while (length > 0 &&
               (line_buf[length - 1] == '\n' || line_buf[length - 1] == '\r')) {
            line_buf[--length] = '\0';
        }

        if (length == INPUT_BUFFER_SIZE - 1) {
            int c;
            emit_error("unknown",
                       "input line exceeds maximum length (%d bytes); line truncated",
                       INPUT_BUFFER_SIZE - 1);
            while ((c = fgetc(input_stream)) != EOF && c != '\n') {
            }
            any_error = 1;
            continue;
        }

        dispatch_command(line_buf);
        if (g_command_failed) {
            any_error = 1;
        }
    }

    if (script_mode && input_stream != stdin) {
        fclose(input_stream);
    }

    return any_error ? 1 : 0;
}
