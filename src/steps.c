/*
 * steps.c — Step recording buffer
 * Static storage (~3.3 KB) for step-by-step display.
 * Includes operation-level checkpoints for pause-at-boundary navigation.
 */
#include "steps.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef CE_DISABLE_STEP_RECORDING

static char lines[MAX_STEPS][MAX_STEP_LEN];
static int  count;
static bool overflow;

/* Checkpoint storage: description + step index where it was inserted */
static char ckpt_desc[MAX_CHECKPOINTS][MAX_CKPT_LEN];
static char ckpt_result[MAX_CHECKPOINTS][MAX_STEP_LEN];
static int  ckpt_step[MAX_CHECKPOINTS]; /* step index at time of checkpoint */
static int  ckpt_count;

void steps_clear(void)
{
    count = 0;
    overflow = false;
    ckpt_count = 0;
}

bool steps_add(const char *line)
{
    if (!line) return false;
    if (count >= MAX_STEPS) {
        if (!overflow) {
            strncpy(lines[MAX_STEPS - 1], "(... more steps omitted)",
                    MAX_STEP_LEN - 1);
            lines[MAX_STEPS - 1][MAX_STEP_LEN - 1] = '\0';
            overflow = true;
        }
        return false;
    }
    strncpy(lines[count], line, MAX_STEP_LEN - 1);
    lines[count][MAX_STEP_LEN - 1] = '\0';
    count++;
    return true;
}

bool steps_addf(const char *fmt, ...)
{
    va_list ap;
    if (!fmt) return false;
    if (count >= MAX_STEPS) {
        if (!overflow) {
            strncpy(lines[MAX_STEPS - 1], "(... more steps omitted)",
                    MAX_STEP_LEN - 1);
            lines[MAX_STEPS - 1][MAX_STEP_LEN - 1] = '\0';
            overflow = true;
        }
        return false;
    }
    va_start(ap, fmt);
    vsnprintf(lines[count], MAX_STEP_LEN, fmt, ap);
    va_end(ap);
    count++;
    return true;
}

int steps_count(void)
{
    return count;
}

const char *steps_get(int index)
{
    return (index >= 0 && index < count) ? lines[index] : "";
}

int steps_save(void)
{
    return count;
}

void steps_restore(int saved_count)
{
    if (saved_count <= MAX_STEPS && saved_count <= count) {
        count = saved_count;
        overflow = false;
        /* Trim checkpoints that point beyond the restored count */
        while (ckpt_count > 0 && ckpt_step[ckpt_count - 1] > count)
            ckpt_count--;
    }
}

bool steps_full(void)
{
    return overflow;
}

/* ── Operation-level checkpoints ─────────────────────────── */

bool steps_add_checkpoint(const char *desc)
{
    if (!desc) return false;
    if (ckpt_count >= MAX_CHECKPOINTS) return false;
    strncpy(ckpt_desc[ckpt_count], desc, MAX_CKPT_LEN - 1);
    ckpt_desc[ckpt_count][MAX_CKPT_LEN - 1] = '\0';
    ckpt_result[ckpt_count][0] = '\0';
    ckpt_step[ckpt_count] = count; /* current step index */
    ckpt_count++;
    return true;
}

bool steps_set_checkpoint_result(const char *result)
{
    if (!result) return false;
    if (ckpt_count <= 0) return false;
    strncpy(ckpt_result[ckpt_count - 1], result, MAX_STEP_LEN - 1);
    ckpt_result[ckpt_count - 1][MAX_STEP_LEN - 1] = '\0';
    return true;
}

bool steps_set_checkpoint_resultf(const char *fmt, ...)
{
    va_list ap;

    if (!fmt) return false;
    if (ckpt_count <= 0) return false;

    va_start(ap, fmt);
    vsnprintf(ckpt_result[ckpt_count - 1], MAX_STEP_LEN, fmt, ap);
    va_end(ap);
    return true;
}

int steps_checkpoint_count(void)
{
    return ckpt_count;
}

const char *steps_get_checkpoint(int index)
{
    return (index >= 0 && index < ckpt_count) ? ckpt_desc[index] : "";
}

int steps_get_checkpoint_step(int index)
{
    return (index >= 0 && index < ckpt_count) ? ckpt_step[index] : 0;
}

int steps_get_checkpoint_end_step(int index)
{
    if (index < 0 || index >= ckpt_count) return 0;
    if (index + 1 < ckpt_count) return ckpt_step[index + 1];
    return count;
}

const char *steps_get_checkpoint_result(int index)
{
    int start;
    int end;

    if (index < 0 || index >= ckpt_count) return "";
    if (ckpt_result[index][0]) return ckpt_result[index];

    start = ckpt_step[index];
    end = steps_get_checkpoint_end_step(index);
    if (end <= start || end <= 0) return "";
    return steps_get(end - 1);
}

#endif
