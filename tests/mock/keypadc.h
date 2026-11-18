/*
 * keypadc.h — Mock CE keypad header for desktop compilation
 * Provides key constants and shared mock keyboard state.
 */
#ifndef KEYPADC_H
#define KEYPADC_H

#include <stdint.h>

/* kb_Data mock: 8 groups of uint16_t */
extern uint16_t kb_Data[8];
void kb_mock_reset(void);

void kb_Scan(void);

/* Group 1 */
#define kb_2nd     (1 << 0)
#define kb_Del     (1 << 1)
#define kb_Mode    (1 << 2)
#define kb_Yequ    (1 << 3)
#define kb_Window  (1 << 4)
#define kb_Zoom    (1 << 5)
#define kb_Trace   (1 << 6)
#define kb_Graph   (1 << 7)

/* Group 2 */
#define kb_Ln      (1 << 0)
#define kb_Log     (1 << 1)
#define kb_Square  (1 << 2)
#define kb_Recip   (1 << 3)
#define kb_Math    (1 << 4)
#define kb_Alpha   (1 << 5)
#define kb_Sto     (1 << 6)  /* mock-only bit; real CEdev uses 1<<1 */

/* Group 3 */
#define kb_0       (1 << 0)
#define kb_1       (1 << 1)
#define kb_4       (1 << 2)
#define kb_7       (1 << 3)
#define kb_Comma   (1 << 4)
#define kb_Sin     (1 << 5)
#define kb_Apps    (1 << 6)
#define kb_GraphVar (1 << 7)

/* Group 4 */
#define kb_DecPnt  (1 << 0)
#define kb_2       (1 << 1)
#define kb_5       (1 << 2)
#define kb_8       (1 << 3)
#define kb_LParen  (1 << 4)
#define kb_Cos     (1 << 5)
#define kb_Prgm    (1 << 6)

/* Group 5 */
#define kb_Chs     (1 << 0)
#define kb_3       (1 << 1)
#define kb_6       (1 << 2)
#define kb_9       (1 << 3)
#define kb_RParen  (1 << 4)
#define kb_Tan     (1 << 5)

/* Group 6 */
#define kb_Enter   (1 << 0)
#define kb_Add     (1 << 1)
#define kb_Sub     (1 << 2)
#define kb_Mul     (1 << 3)
#define kb_Div     (1 << 4)
#define kb_Power   (1 << 5)
#define kb_Clear   (1 << 6)

/* Group 7 */
#define kb_Down    (1 << 0)
#define kb_Left    (1 << 1)
#define kb_Right   (1 << 2)
#define kb_Up      (1 << 3)

#endif /* KEYPADC_H */
