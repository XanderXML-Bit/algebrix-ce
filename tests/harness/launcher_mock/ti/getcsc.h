/* Desktop stand-in for CEdev <ti/getcsc.h>: the sk_* scan codes (values
 * copied from CEdev v11.2) and os_GetCSC, which the launcher harness
 * scripts.  Used only by tests/harness/launcher_view_harness.c. */
#ifndef LAUNCHER_MOCK_TI_GETCSC_H
#define LAUNCHER_MOCK_TI_GETCSC_H
#include <stdint.h>
typedef uint8_t sk_key_t;
uint8_t os_GetCSC(void);
#define sk_Down             0x01
#define sk_Left             0x02
#define sk_Right            0x03
#define sk_Up               0x04
#define sk_Enter            0x09
#define sk_2nd              0x36
#define sk_Clear            0x0F
#define sk_Alpha            0x30
#define sk_Add              0x0A
#define sk_Sub              0x0B
#define sk_Mul              0x0C
#define sk_Div              0x0D
#define sk_Graph            0x31
#define sk_Trace            0x32
#define sk_Zoom             0x33
#define sk_Window           0x34
#define sk_Yequ             0x35
#define sk_Mode             0x37
#define sk_Del              0x38
#define sk_Store            0x2A
#define sk_Ln               0x2B
#define sk_Log              0x2C
#define sk_Square           0x2D
#define sk_Recip            0x2E
#define sk_Math             0x2F
#define sk_0                0x21
#define sk_1                0x22
#define sk_4                0x23
#define sk_7                0x24
#define sk_2                0x1A
#define sk_5                0x1B
#define sk_8                0x1C
#define sk_3                0x12
#define sk_6                0x13
#define sk_9                0x14
#define sk_Comma            0x25
#define sk_Sin              0x26
#define sk_Apps             0x27
#define sk_GraphVar         0x28
#define sk_DecPnt           0x19
#define sk_LParen           0x1D
#define sk_Cos              0x1E
#define sk_Prgm             0x1F
#define sk_Stat             0x20
#define sk_Chs              0x11
#define sk_RParen           0x15
#define sk_Tan              0x16
#define sk_Vars             0x17
#define sk_Power            0x0E
#define sk_Fenetre          sk_Window
#define sk_Fx               sk_Yequ
#define sk_Suppr            sk_Del
#define sk_TglExact         sk_Recip
#define sk_Trig             sk_Sin
#define sk_Matrice          sk_Apps
#define sk_Resol            sk_Cos
#define sk_Frac             sk_Tan
#define sk_Annul            sk_Clear
#endif
