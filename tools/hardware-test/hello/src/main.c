/*
 * HWTEST1 — CalcEngine Hardware Sanity Test 1
 * Minimal bare-bones .8xp: no AppVars, no os_RunPrgm, no CalcEngine code.
 * Purpose: confirm arTIfiCE -> ASM launch -> basic screen/keypad work.
 *
 * Procedure:
 *  1. Flash via TI Connect CE (archived).
 *  2. Launch via arTIfiCE shell — select HWTEST1, press ENTER.
 *  3. Screen should clear and show "HELLO".
 *  4. Press any key -> shows "GOT KEY: <decimal code>".
 *  5. Press any key -> program exits cleanly back to shell.
 *
 * If step 3 fails: arTIfiCE privilege or ASM launch broken.
 * If step 4 fails: keypad read broken (unlikely).
 * If step 5 crashes: return-to-shell broken.
 */

#include <tice.h>
#include <ti/getcsc.h>
#include <ti/screen.h>
#include <stdio.h>

int main(void) {
    char buf[20];
    uint8_t key;

    /* Step 1: clear screen and print hello */
    os_ClrHome();
    os_PutStrFull("HWTEST1: HELLO            ");
    os_SetCursorPos(1, 0);
    os_PutStrFull("                          ");
    os_SetCursorPos(2, 0);
    os_PutStrFull("Press any key...          ");

    /* Wait for first keypress */
    do { key = os_GetCSC(); } while (key == 0);

    /* Step 2: show key code */
    os_ClrHome();
    snprintf(buf, sizeof(buf), "GOT KEY: %u", (unsigned)key);
    os_PutStrFull(buf);
    os_SetCursorPos(1, 0);
    os_PutStrFull("                          ");
    os_SetCursorPos(2, 0);
    os_PutStrFull("Press any key to exit...  ");

    /* Wait for second keypress */
    do { key = os_GetCSC(); } while (key == 0);

    /* Step 3: clean exit */
    os_ClrHome();
    return 0;
}
