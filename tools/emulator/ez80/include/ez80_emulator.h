#ifndef EZ80_EMULATOR_H
#define EZ80_EMULATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ez80_emulator;
typedef struct ez80_emulator ez80_emulator_t;

#define EZ80_FLASH_TOTAL_BYTES (4u * 1024u * 1024u)
#define EZ80_FLASH_WINDOW_BYTES (64u * 1024u)
#define EZ80_FLASH_SECTOR_BYTES (64u * 1024u)
#define EZ80_FLASH_SECTOR_COUNT (EZ80_FLASH_TOTAL_BYTES / EZ80_FLASH_SECTOR_BYTES)
#define EZ80_RAM_BYTES (256u * 1024u)
#define EZ80_PORT_BYTES 256u
#define EZ80_PORT_SPACE_BYTES 65536u
#define EZ80_MMIO_BYTES 2048u
#define EZ80_SYSTEM_MMIO_BYTES 0x1000u

#define EZ80_FLASH_BASE 0x000000u
#define EZ80_RAM_BASE 0xD00000u
#define EZ80_LCD_RAM_BASE 0xD40000u
#define EZ80_LCD_RAM_BYTES 0x25800u
#define EZ80_SYSTEM_MMIO_BASE 0xE00000u
#define EZ80_KEYPAD_MMIO_BASE 0xF50000u
#define EZ80_KEYPAD_MMIO_BYTES 0x40u
#define EZ80_PORT_BASE 0xF00000u
#define EZ80_MMIO_BASE 0xE30000u

#define EZ80_KEYPAD_REG_SCAN_MODE_0 0x00u
#define EZ80_KEYPAD_REG_SCAN_CFG_0 0x04u
#define EZ80_KEYPAD_REG_IRQ_STATUS 0x08u
#define EZ80_KEYPAD_REG_IRQ_MASK 0x0Cu
#define EZ80_KEYPAD_REG_ROW0_LO 0x10u
#define EZ80_KEYPAD_REG_ROW_STRIDE 0x02u

#define EZ80_MMIO_REG_KEY_STATUS 0x0000u
#define EZ80_MMIO_REG_LCD_STATUS 0x0001u
#define EZ80_MMIO_REG_LAST_OUT_PORT 0x0010u
#define EZ80_MMIO_REG_LAST_OUT_VALUE 0x0011u
#define EZ80_MMIO_REG_LAST_IN_PORT 0x0012u
#define EZ80_MMIO_REG_LAST_IN_VALUE 0x0013u
#define EZ80_MMIO_REG_FLASH_UNLOCK_STATE 0x0014u
#define EZ80_MMIO_REG_FLASH_CERT_WRITE_STATE 0x0015u
#define EZ80_MMIO_REG_LAST_OUT_PORT_HI 0x0016u
#define EZ80_MMIO_REG_LAST_IN_PORT_HI 0x0017u
#define EZ80_MMIO_REG_OS_VERSION_PATCH 0x0020u
#define EZ80_MMIO_REG_OS_VERSION_MINOR 0x0021u
#define EZ80_MMIO_REG_OS_VERSION_MAJOR 0x0022u
#define EZ80_MMIO_REG_NATIVE_UNSIGNED_EXEC_ALLOWED 0x0023u
#define EZ80_MMIO_REG_ARTIFICE_BYPASS_ACTIVE 0x0024u
#define EZ80_MMIO_REG_ARTIFICE_ENTRY_METHOD 0x0025u
#define EZ80_MMIO_REG_DNGRADE_PORT28_BIT3_BLOCKED 0x0026u
#define EZ80_MMIO_REG_DNGRADE_LAST_FAILURE_REASON 0x0027u
#define EZ80_MMIO_REG_DNGRADE_LAST_FAILURE_PORT 0x0028u
#define EZ80_MMIO_REG_ARTIFICE_LAST_KEY 0x0029u
#define EZ80_MMIO_REG_OS_VAR_COUNT 0x0030u
#define EZ80_MMIO_REG_OS_LAST_BCALL_LO 0x0031u
#define EZ80_MMIO_REG_OS_LAST_BCALL_HI 0x0032u
#define EZ80_MMIO_REG_OS_LAST_STATUS 0x0033u
#define EZ80_MMIO_REG_OS_LAST_RESULT_0 0x0034u
#define EZ80_MMIO_REG_OS_LAST_RESULT_1 0x0035u
#define EZ80_MMIO_REG_OS_LAST_RESULT_2 0x0036u
#define EZ80_MMIO_REG_HW_KEYPAD_ROW_MASK 0x0040u
#define EZ80_MMIO_REG_HW_KEYPAD_DATA 0x0041u
#define EZ80_MMIO_REG_HW_LCD_COMMAND 0x0042u
#define EZ80_MMIO_REG_HW_LCD_DATA 0x0043u
#define EZ80_MMIO_REG_HW_LCD_WRITE_INDEX_LO 0x0044u
#define EZ80_MMIO_REG_HW_LCD_WRITE_INDEX_HI 0x0045u
#define EZ80_MMIO_REG_HW_USB_CONTROL 0x0046u
#define EZ80_MMIO_REG_HW_USB_STATUS 0x0047u
#define EZ80_MMIO_REG_HW_TIMER0_LO 0x0048u
#define EZ80_MMIO_REG_HW_TIMER0_HI 0x0049u
#define EZ80_MMIO_REG_HW_TIMER1_LO 0x004Au
#define EZ80_MMIO_REG_HW_TIMER1_HI 0x004Bu
#define EZ80_MMIO_REG_HW_RTC_SECONDS 0x004Cu
#define EZ80_MMIO_REG_HW_RTC_MINUTES 0x004Du
#define EZ80_MMIO_REG_HW_RTC_HOURS 0x004Eu
#define EZ80_MMIO_REG_HW_IRQ_MASK 0x004Fu
#define EZ80_MMIO_REG_HW_IRQ_PENDING 0x0050u
#define EZ80_MMIO_REG_OS_HOME_ROW 0x0051u
#define EZ80_MMIO_REG_OS_HOME_COL 0x0052u
#define EZ80_MMIO_REG_OS_GRAPH_LAST_X 0x0053u
#define EZ80_MMIO_REG_OS_GRAPH_LAST_Y 0x0054u
#define EZ80_MMIO_REG_OS_GRAPH_LAST_COLOR 0x0055u
#define EZ80_MMIO_REG_DNGRADE_LAST_STRATEGY 0x0056u
#define EZ80_MMIO_REG_DNGRADE_LAST_ATTEMPT_STATUS 0x0057u
#define EZ80_MMIO_REG_DNGRADE_LAST_TIMEOUT 0x0058u
#define EZ80_MMIO_REG_DNGRADE_LAST_WATCHDOG 0x0059u
#define EZ80_MMIO_REG_DNGRADE_LAST_CYCLES_0 0x005Au
#define EZ80_MMIO_REG_DNGRADE_LAST_CYCLES_1 0x005Bu
#define EZ80_MMIO_REG_DNGRADE_LAST_CYCLES_2 0x005Cu
#define EZ80_MMIO_REG_DNGRADE_LAST_WRITE_OK 0x005Du
#define EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_NAME_0 0x005Eu
#define EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_NAME_1 0x005Fu
#define EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_NAME_2 0x0060u
#define EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_NAME_3 0x0061u
#define EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_NAME_4 0x0062u
#define EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_NAME_5 0x0063u
#define EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_NAME_6 0x0064u
#define EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_NAME_7 0x0065u
#define EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_NAME_8 0x0066u
#define EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_RAM_0 0x0067u
#define EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_RAM_1 0x0068u
#define EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_RAM_2 0x0069u
#define EZ80_MMIO_REG_OS_LAUNCH_COUNT_0 0x006Au
#define EZ80_MMIO_REG_OS_LAUNCH_COUNT_1 0x006Bu
#define EZ80_MMIO_REG_OS_LAUNCH_COUNT_2 0x006Cu
#define EZ80_MMIO_REG_OS_SWAP_COUNT_0 0x006Du
#define EZ80_MMIO_REG_OS_SWAP_COUNT_1 0x006Eu
#define EZ80_MMIO_REG_OS_SWAP_COUNT_2 0x006Fu
#define EZ80_MMIO_REG_OS_SWAP_FAIL_COUNT_0 0x0070u
#define EZ80_MMIO_REG_OS_SWAP_FAIL_COUNT_1 0x0071u
#define EZ80_MMIO_REG_OS_SWAP_FAIL_COUNT_2 0x0072u
#define EZ80_MMIO_REG_OS_LAST_SWAP_FAILURE 0x0073u
#define EZ80_MMIO_REG_OS_LAST_SWAP_ARCHIVE_CRC_0 0x0074u
#define EZ80_MMIO_REG_OS_LAST_SWAP_ARCHIVE_CRC_1 0x0075u
#define EZ80_MMIO_REG_OS_LAST_SWAP_ARCHIVE_CRC_2 0x0076u
#define EZ80_MMIO_REG_OS_LAST_SWAP_ARCHIVE_CRC_3 0x0077u
#define EZ80_MMIO_REG_OS_LAST_SWAP_RAM_CRC_0 0x0078u
#define EZ80_MMIO_REG_OS_LAST_SWAP_RAM_CRC_1 0x0079u
#define EZ80_MMIO_REG_OS_LAST_SWAP_RAM_CRC_2 0x007Au
#define EZ80_MMIO_REG_OS_LAST_SWAP_RAM_CRC_3 0x007Bu
#define EZ80_MMIO_REG_FLASH_SECTOR_SELECT 0x007Cu
#define EZ80_MMIO_REG_FLASH_SECTOR_FLAGS 0x007Du
#define EZ80_MMIO_REG_FLASH_PROTECT_MODE 0x007Eu
#define EZ80_MMIO_REG_WRITEFLASH_STATUS 0x007Fu
#define EZ80_MMIO_REG_WRITEFLASH_ADDR_0 0x0080u
#define EZ80_MMIO_REG_WRITEFLASH_ADDR_1 0x0081u
#define EZ80_MMIO_REG_WRITEFLASH_ADDR_2 0x0082u
#define EZ80_MMIO_REG_WRITEFLASH_REQUESTED 0x0083u
#define EZ80_MMIO_REG_WRITEFLASH_BEFORE 0x0084u
#define EZ80_MMIO_REG_WRITEFLASH_AFTER 0x0085u
#define EZ80_MMIO_REG_TIMER0_PRESCALER 0x0086u
#define EZ80_MMIO_REG_TIMER1_PRESCALER 0x0087u
#define EZ80_MMIO_REG_TIMER0_RELOAD_LO 0x0088u
#define EZ80_MMIO_REG_TIMER0_RELOAD_HI 0x0089u
#define EZ80_MMIO_REG_TIMER1_RELOAD_LO 0x008Au
#define EZ80_MMIO_REG_TIMER1_RELOAD_HI 0x008Bu
#define EZ80_MMIO_REG_WATCHPOINT_COUNT 0x008Cu
#define EZ80_MMIO_REG_LAST_WATCHPOINT_ADDR_0 0x008Du
#define EZ80_MMIO_REG_LAST_WATCHPOINT_ADDR_1 0x008Eu
#define EZ80_MMIO_REG_LAST_WATCHPOINT_ADDR_2 0x008Fu
#define EZ80_MMIO_REG_LAST_WATCHPOINT_TYPE 0x0090u
#define EZ80_MMIO_REG_EI_PENDING 0x0091u

/* DMA controller MMIO registers */
#define EZ80_MMIO_REG_DMA0_SRC_0 0x00A0u
#define EZ80_MMIO_REG_DMA0_SRC_1 0x00A1u
#define EZ80_MMIO_REG_DMA0_SRC_2 0x00A2u
#define EZ80_MMIO_REG_DMA0_DST_0 0x00A3u
#define EZ80_MMIO_REG_DMA0_DST_1 0x00A4u
#define EZ80_MMIO_REG_DMA0_DST_2 0x00A5u
#define EZ80_MMIO_REG_DMA0_LEN_0 0x00A6u
#define EZ80_MMIO_REG_DMA0_LEN_1 0x00A7u
#define EZ80_MMIO_REG_DMA0_LEN_2 0x00A8u
#define EZ80_MMIO_REG_DMA0_CONTROL 0x00A9u
#define EZ80_MMIO_REG_DMA0_STATUS 0x00AAu
#define EZ80_MMIO_REG_DMA0_TRANSFERRED_0 0x00ABu
#define EZ80_MMIO_REG_DMA0_TRANSFERRED_1 0x00ACu
#define EZ80_MMIO_REG_DMA0_TRANSFERRED_2 0x00ADu
#define EZ80_MMIO_REG_DMA1_SRC_0 0x00B0u
#define EZ80_MMIO_REG_DMA1_SRC_1 0x00B1u
#define EZ80_MMIO_REG_DMA1_SRC_2 0x00B2u
#define EZ80_MMIO_REG_DMA1_DST_0 0x00B3u
#define EZ80_MMIO_REG_DMA1_DST_1 0x00B4u
#define EZ80_MMIO_REG_DMA1_DST_2 0x00B5u
#define EZ80_MMIO_REG_DMA1_LEN_0 0x00B6u
#define EZ80_MMIO_REG_DMA1_LEN_1 0x00B7u
#define EZ80_MMIO_REG_DMA1_LEN_2 0x00B8u
#define EZ80_MMIO_REG_DMA1_CONTROL 0x00B9u
#define EZ80_MMIO_REG_DMA1_STATUS 0x00BAu
#define EZ80_MMIO_REG_DMA1_TRANSFERRED_0 0x00BBu
#define EZ80_MMIO_REG_DMA1_TRANSFERRED_1 0x00BCu
#define EZ80_MMIO_REG_DMA1_TRANSFERRED_2 0x00BDu
#define EZ80_MMIO_REG_DMA2_SRC_0 0x00C0u
#define EZ80_MMIO_REG_DMA2_SRC_1 0x00C1u
#define EZ80_MMIO_REG_DMA2_SRC_2 0x00C2u
#define EZ80_MMIO_REG_DMA2_DST_0 0x00C3u
#define EZ80_MMIO_REG_DMA2_DST_1 0x00C4u
#define EZ80_MMIO_REG_DMA2_DST_2 0x00C5u
#define EZ80_MMIO_REG_DMA2_LEN_0 0x00C6u
#define EZ80_MMIO_REG_DMA2_LEN_1 0x00C7u
#define EZ80_MMIO_REG_DMA2_LEN_2 0x00C8u
#define EZ80_MMIO_REG_DMA2_CONTROL 0x00C9u
#define EZ80_MMIO_REG_DMA2_STATUS 0x00CAu
#define EZ80_MMIO_REG_DMA2_TRANSFERRED_0 0x00CBu
#define EZ80_MMIO_REG_DMA2_TRANSFERRED_1 0x00CCu
#define EZ80_MMIO_REG_DMA2_TRANSFERRED_2 0x00CDu
#define EZ80_MMIO_REG_DMA_TOTAL_TRANSFERS_0 0x00D0u
#define EZ80_MMIO_REG_DMA_TOTAL_TRANSFERS_1 0x00D1u
#define EZ80_MMIO_REG_DMA_TOTAL_TRANSFERS_2 0x00D2u
#define EZ80_MMIO_REG_DMA_TOTAL_TRANSFERS_3 0x00D3u

/* Interrupt nesting MMIO registers */
#define EZ80_MMIO_REG_IRQ_NESTING_DEPTH 0x00D4u
#define EZ80_MMIO_REG_IRQ_TOTAL_RETI 0x00D5u
#define EZ80_MMIO_REG_IRQ_LAST_RETI_SOURCE 0x00D6u
#define EZ80_MMIO_REG_NMI_COUNT 0x00D7u
#define EZ80_MMIO_REG_NMI_ACTIVE 0x00D8u

/* SPI MMIO telemetry registers */
#define EZ80_MMIO_REG_SPI0_CONTROL 0x00E0u
#define EZ80_MMIO_REG_SPI0_STATUS 0x00E1u
#define EZ80_MMIO_REG_SPI0_BAUD 0x00E2u
#define EZ80_MMIO_REG_SPI0_TX_COUNT_LO 0x00E3u
#define EZ80_MMIO_REG_SPI0_TX_COUNT_HI 0x00E4u
#define EZ80_MMIO_REG_SPI0_RX_COUNT_LO 0x00E5u
#define EZ80_MMIO_REG_SPI0_RX_COUNT_HI 0x00E6u
#define EZ80_MMIO_REG_SPI1_CONTROL 0x00E7u
#define EZ80_MMIO_REG_SPI1_STATUS 0x00E8u
#define EZ80_MMIO_REG_SPI1_BAUD 0x00E9u
#define EZ80_MMIO_REG_SPI1_TX_COUNT_LO 0x00EAu
#define EZ80_MMIO_REG_SPI1_TX_COUNT_HI 0x00EBu
#define EZ80_MMIO_REG_SPI1_RX_COUNT_LO 0x00ECu
#define EZ80_MMIO_REG_SPI1_RX_COUNT_HI 0x00EDu

/* GPIO MMIO telemetry registers */
#define EZ80_MMIO_REG_GPIO_A_DATA 0x00F0u
#define EZ80_MMIO_REG_GPIO_A_DIR 0x00F1u
#define EZ80_MMIO_REG_GPIO_B_DATA 0x00F2u
#define EZ80_MMIO_REG_GPIO_B_DIR 0x00F3u
#define EZ80_MMIO_REG_GPIO_C_DATA 0x00F4u
#define EZ80_MMIO_REG_GPIO_C_DIR 0x00F5u
#define EZ80_MMIO_REG_GPIO_D_DATA 0x00F6u
#define EZ80_MMIO_REG_GPIO_D_DIR 0x00F7u
#define EZ80_MMIO_REG_GPIO_E_DATA 0x00F8u
#define EZ80_MMIO_REG_GPIO_E_DIR 0x00F9u
#define EZ80_MMIO_REG_GPIO_F_DATA 0x00FAu
#define EZ80_MMIO_REG_GPIO_F_DIR 0x00FBu
#define EZ80_MMIO_REG_BACKLIGHT_STATE 0x00FCu
#define EZ80_MMIO_REG_BATTERY_LEVEL 0x00FDu
#define EZ80_MMIO_REG_POWER_STATE 0x00FEu

/* Flash erase MMIO telemetry registers */
#define EZ80_MMIO_REG_FLASH_ERASE_SECTOR 0x0200u
#define EZ80_MMIO_REG_FLASH_ERASE_STATUS 0x0201u
#define EZ80_MMIO_REG_FLASH_ERASE_COUNT_LO 0x0202u
#define EZ80_MMIO_REG_FLASH_ERASE_COUNT_HI 0x0203u
#define EZ80_MMIO_REG_USB_RX_COUNT_LO 0x0204u
#define EZ80_MMIO_REG_USB_RX_COUNT_HI 0x0205u
#define EZ80_MMIO_REG_USB_TX_COUNT_LO 0x0206u
#define EZ80_MMIO_REG_USB_TX_COUNT_HI 0x0207u
#define EZ80_MMIO_REG_USB_RX_LEVEL 0x0208u
#define EZ80_MMIO_REG_USB_TX_LEVEL 0x0209u
#define EZ80_MMIO_REG_USB_LAST_RX 0x020Au
#define EZ80_MMIO_REG_USB_LAST_TX 0x020Bu
#define EZ80_MMIO_REG_USB_CONNECTED 0x020Cu

#define EZ80_MMIO_REG_BCALL_VECTOR_LO 0x0100u
#define EZ80_MMIO_REG_BCALL_VECTOR_HI 0x0101u
#define EZ80_MMIO_REG_BCALL_ARG0_0 0x0102u
#define EZ80_MMIO_REG_BCALL_ARG0_1 0x0103u
#define EZ80_MMIO_REG_BCALL_ARG0_2 0x0104u
#define EZ80_MMIO_REG_BCALL_ARG1_0 0x0105u
#define EZ80_MMIO_REG_BCALL_ARG1_1 0x0106u
#define EZ80_MMIO_REG_BCALL_ARG1_2 0x0107u
#define EZ80_MMIO_REG_BCALL_ARG2_0 0x0108u
#define EZ80_MMIO_REG_BCALL_ARG2_1 0x0109u
#define EZ80_MMIO_REG_BCALL_ARG2_2 0x010Au
#define EZ80_MMIO_REG_BCALL_ARG3_0 0x010Bu
#define EZ80_MMIO_REG_BCALL_ARG3_1 0x010Cu
#define EZ80_MMIO_REG_BCALL_ARG3_2 0x010Du
#define EZ80_MMIO_REG_BCALL_EXECUTE 0x010Eu
#define EZ80_MMIO_REG_BCALL_STATUS 0x010Fu
#define EZ80_MMIO_REG_BCALL_RESULT_0 0x0110u
#define EZ80_MMIO_REG_BCALL_RESULT_1 0x0111u
#define EZ80_MMIO_REG_BCALL_RESULT_2 0x0112u

#define EZ80_PORT_KEYPAD_ROW_SELECT 0xD0u
#define EZ80_PORT_KEYPAD_DATA 0xD1u
#define EZ80_PORT_LCD_COMMAND 0xD2u
#define EZ80_PORT_LCD_DATA 0xD3u
#define EZ80_PORT_LCD_STATUS_STUB 0xD4u
#define EZ80_PORT_USB_CONTROL 0xD8u
#define EZ80_PORT_USB_STATUS 0xD9u
#define EZ80_PORT_USB_TX 0xDAu
#define EZ80_PORT_USB_RX 0xDBu
#define EZ80_PORT_TIMER0_CONTROL 0xDCu
#define EZ80_PORT_TIMER0_COUNTER_LO 0xDDu
#define EZ80_PORT_TIMER0_COUNTER_HI 0xDEu
#define EZ80_PORT_TIMER1_CONTROL 0xDFu
#define EZ80_PORT_TIMER1_COUNTER_LO 0xE0u
#define EZ80_PORT_TIMER1_COUNTER_HI 0xE1u
#define EZ80_PORT_RTC_SECONDS 0xE2u
#define EZ80_PORT_RTC_MINUTES 0xE3u
#define EZ80_PORT_RTC_HOURS 0xE4u
#define EZ80_PORT_IRQ_MASK 0xE5u
#define EZ80_PORT_IRQ_PENDING 0xE6u
#define EZ80_PORT_IRQ_ACK 0xE7u

#define EZ80_TIMER_CONTROL_ENABLE 0x01u
#define EZ80_TIMER_CONTROL_IRQ_ENABLE 0x02u
#define EZ80_TIMER_CONTROL_RELOAD 0x04u

#define EZ80_PORT_TIMER0_RELOAD_LO 0xE8u
#define EZ80_PORT_TIMER0_RELOAD_HI 0xE9u
#define EZ80_PORT_TIMER1_RELOAD_LO 0xEAu
#define EZ80_PORT_TIMER1_RELOAD_HI 0xEBu
#define EZ80_PORT_TIMER0_PRESCALER 0xECu
#define EZ80_PORT_TIMER1_PRESCALER 0xEDu

/* RTC alarm ports */
#define EZ80_PORT_RTC_ALARM_SECONDS 0xEEu
#define EZ80_PORT_RTC_ALARM_MINUTES 0xEFu
#define EZ80_PORT_RTC_ALARM_HOURS 0xF0u
#define EZ80_PORT_RTC_ALARM_CONTROL 0xF1u

/* Hardware watchdog ports */
#define EZ80_PORT_WDT_CONTROL 0xF2u
#define EZ80_PORT_WDT_FEED 0xF3u
#define EZ80_PORT_WDT_TIMEOUT_LO 0xF4u
#define EZ80_PORT_WDT_TIMEOUT_HI 0xF5u

#define EZ80_PORT_GROUP_KEYBOARD_CTRL 0x01u
#define EZ80_PORT_GROUP_KEYBOARD_DATA 0x02u
#define EZ80_PORT_GROUP_LCD 0x40u

#define EZ80_PORT_KBD_CTRL_GROUP_SELECT 0x00u
#define EZ80_PORT_KBD_CTRL_IRQ_ON_KEYPRESS 0x01u
#define EZ80_PORT_KBD_CTRL_STATUS 0x02u
#define EZ80_PORT_KBD_DATA_MATRIX 0x00u

#define EZ80_KBD_STATUS_KEY_PRESSED 0x01u
#define EZ80_KBD_STATUS_IRQ_PENDING 0x02u
#define EZ80_KBD_IRQ_ON_KEYPRESS_ENABLE 0x01u

#define EZ80_PORT_LCD_REG_MODE 0x00u
#define EZ80_PORT_LCD_REG_TIMING 0x01u
#define EZ80_PORT_LCD_REG_WINDOW_X0_LO 0x02u
#define EZ80_PORT_LCD_REG_WINDOW_X0_HI 0x03u
#define EZ80_PORT_LCD_REG_WINDOW_Y0_LO 0x04u
#define EZ80_PORT_LCD_REG_WINDOW_Y0_HI 0x05u
#define EZ80_PORT_LCD_REG_WINDOW_X1_LO 0x06u
#define EZ80_PORT_LCD_REG_WINDOW_X1_HI 0x07u
#define EZ80_PORT_LCD_REG_WINDOW_Y1_LO 0x08u
#define EZ80_PORT_LCD_REG_WINDOW_Y1_HI 0x09u
#define EZ80_PORT_LCD_REG_VRAM_BASE_0 0x0Au
#define EZ80_PORT_LCD_REG_VRAM_BASE_1 0x0Bu
#define EZ80_PORT_LCD_REG_VRAM_BASE_2 0x0Cu
#define EZ80_PORT_LCD_REG_STATUS 0x0Du
#define EZ80_PORT_LCD_REG_PIPELINE 0x0Eu
#define EZ80_PORT_LCD_REG_DATA 0x0Fu

#define EZ80_LCD_STATUS_DATA_READY 0x01u
#define EZ80_LCD_STATUS_PIPELINE_BUSY 0x02u
#define EZ80_LCD_STATUS_WINDOW_VALID 0x04u
#define EZ80_LCD_STATUS_CONTROLLER_ENABLED 0x80u

#define EZ80_LCD_PIPELINE_IDLE 0u
#define EZ80_LCD_PIPELINE_WAIT_HIGH 1u
#define EZ80_LCD_PIPELINE_PIXEL_COMMITTED 2u

#define EZ80_LCD_CMD_SOFTWARE_RESET 0x01u
#define EZ80_LCD_CMD_COLUMN_ADDRESS_SET 0x2Au
#define EZ80_LCD_CMD_ROW_ADDRESS_SET 0x2Bu
#define EZ80_LCD_CMD_MEMORY_WRITE 0x2Cu
#define EZ80_LCD_CMD_MEMORY_ACCESS_CONTROL 0x36u

#define EZ80_TIMER_PRESCALER_1 0u
#define EZ80_TIMER_PRESCALER_4 1u
#define EZ80_TIMER_PRESCALER_16 2u
#define EZ80_TIMER_PRESCALER_64 3u

/* RTC alarm constants */
#define EZ80_RTC_ALARM_CONTROL_ENABLE 0x01u
#define EZ80_RTC_ALARM_CONTROL_IRQ_ENABLE 0x02u
#define EZ80_RTC_ALARM_STATUS_FIRED 0x01u

/* RTC alarm IRQ source */
#define EZ80_IRQ_SOURCE_RTC_ALARM 0x100u
#define EZ80_IRQ_VECTOR_OFFSET_RTC_ALARM 0x10u

/* Hardware watchdog constants */
#define EZ80_WDT_CONTROL_ENABLE 0x01u
#define EZ80_WDT_CONTROL_IRQ_ENABLE 0x02u
#define EZ80_WDT_CONTROL_RESET_ON_TIMEOUT 0x04u
#define EZ80_WDT_FEED_MAGIC 0xA5u
#define EZ80_WDT_DEFAULT_TIMEOUT 0xFFFFu

/* Power management constants */
#define EZ80_PM_MODE_ACTIVE 0u
#define EZ80_PM_MODE_IDLE 1u     /* HALT-like: CPU stopped, peripherals running */
#define EZ80_PM_MODE_STANDBY 2u  /* SLP: CPU + most peripherals stopped */
#define EZ80_PM_MODE_SLEEP 3u    /* Deep sleep: only RTC + watchdog running */

/* Power management wake sources */
#define EZ80_PM_WAKE_NONE 0u
#define EZ80_PM_WAKE_KEYPAD 0x01u
#define EZ80_PM_WAKE_TIMER 0x02u
#define EZ80_PM_WAKE_USB 0x04u
#define EZ80_PM_WAKE_RTC_ALARM 0x08u
#define EZ80_PM_WAKE_NMI 0x10u
#define EZ80_PM_WAKE_WATCHDOG 0x20u

#define EZ80_USB_FIFO_SIZE 64u
#define EZ80_USB_CONTROL_ENABLE 0x01u
#define EZ80_USB_CONTROL_RX_IRQ_ENABLE 0x02u
#define EZ80_USB_CONTROL_TX_IRQ_ENABLE 0x04u
#define EZ80_USB_CONTROL_LOOPBACK 0x08u
#define EZ80_USB_CONTROL_RESET 0x80u
#define EZ80_USB_STATUS_RX_READY 0x01u
#define EZ80_USB_STATUS_TX_READY 0x02u
#define EZ80_USB_STATUS_CONNECTED 0x04u
#define EZ80_USB_STATUS_OVERRUN 0x08u
#define EZ80_USB_STATUS_IRQ_PENDING 0x10u
#define EZ80_USB_STATUS_LOOPBACK 0x20u

#define EZ80_IRQ_SOURCE_KEYPAD 0x01u
#define EZ80_IRQ_SOURCE_TIMER0 0x02u
#define EZ80_IRQ_SOURCE_TIMER1 0x04u
#define EZ80_IRQ_SOURCE_USB 0x08u

/* IM 2 interrupt vector offsets for each source (eZ80 uses 2-byte vectors) */
#define EZ80_IRQ_VECTOR_OFFSET_KEYPAD 0x00u
#define EZ80_IRQ_VECTOR_OFFSET_TIMER0 0x02u
#define EZ80_IRQ_VECTOR_OFFSET_TIMER1 0x04u
#define EZ80_IRQ_VECTOR_OFFSET_USB 0x06u
#define EZ80_IRQ_VECTOR_OFFSET_DMA 0x08u
#define EZ80_IRQ_VECTOR_OFFSET_SPI 0x0Au
#define EZ80_IRQ_VECTOR_OFFSET_I2C 0x0Cu
#define EZ80_IRQ_VECTOR_OFFSET_UART 0x0Eu

/* Interrupt dispatch cycle costs */
#define EZ80_IRQ_DISPATCH_CYCLES_IM1 13u
#define EZ80_IRQ_DISPATCH_CYCLES_IM2 19u
#define EZ80_NMI_DISPATCH_CYCLES 11u

/* NMI vector address */
#define EZ80_NMI_VECTOR_ADDR 0x0066u

/* DMA constants */
#define EZ80_DMA_CHANNEL_COUNT 3u
#define EZ80_DMA_MAX_TRANSFER_BYTES 0x100000u /* 1MB max per transfer */

/* DMA control bits */
#define EZ80_DMA_CONTROL_ENABLE 0x01u
#define EZ80_DMA_CONTROL_START 0x02u
#define EZ80_DMA_CONTROL_SRC_INC 0x04u /* auto-increment source */
#define EZ80_DMA_CONTROL_DST_INC 0x08u /* auto-increment destination */
#define EZ80_DMA_CONTROL_IRQ_ON_DONE 0x10u /* raise IRQ when transfer completes */
#define EZ80_DMA_CONTROL_WORD_MODE 0x20u /* 16-bit word transfer mode */
#define EZ80_DMA_CONTROL_CONTINUOUS 0x40u /* continuous/auto-restart mode */

/* DMA status bits */
#define EZ80_DMA_STATUS_IDLE 0x00u
#define EZ80_DMA_STATUS_ACTIVE 0x01u
#define EZ80_DMA_STATUS_DONE 0x02u
#define EZ80_DMA_STATUS_ERROR 0x04u

/* DMA interrupt source */
#define EZ80_IRQ_SOURCE_DMA 0x10u

/* Maximum interrupt nesting depth */
#define EZ80_IRQ_MAX_NESTING 8u

/* Maximum breakpoints and watchpoints */
#define EZ80_MAX_BREAKPOINTS 16u
#define EZ80_MAX_WATCHPOINTS 16u
#define EZ80_MAX_PORT_HANDLERS 16u

/* SPI controller constants */
#define EZ80_SPI_CHANNEL_COUNT 2u
#define EZ80_SPI_FIFO_SIZE 16u

/* SPI port addresses (eZ80 on-chip SPI) */
#define EZ80_PORT_SPI0_BASE 0x0080u
#define EZ80_PORT_SPI0_CONTROL 0x0080u
#define EZ80_PORT_SPI0_STATUS 0x0081u
#define EZ80_PORT_SPI0_DATA 0x0082u
#define EZ80_PORT_SPI0_BAUD 0x0083u
#define EZ80_PORT_SPI1_BASE 0x0090u
#define EZ80_PORT_SPI1_CONTROL 0x0090u
#define EZ80_PORT_SPI1_STATUS 0x0091u
#define EZ80_PORT_SPI1_DATA 0x0092u
#define EZ80_PORT_SPI1_BAUD 0x0093u

/* SPI control bits */
#define EZ80_SPI_CONTROL_ENABLE 0x01u
#define EZ80_SPI_CONTROL_MASTER 0x02u
#define EZ80_SPI_CONTROL_CPOL 0x04u
#define EZ80_SPI_CONTROL_CPHA 0x08u
#define EZ80_SPI_CONTROL_IRQ_ENABLE 0x10u
#define EZ80_SPI_CONTROL_LSB_FIRST 0x20u
#define EZ80_SPI_CONTROL_CS_ACTIVE 0x40u
#define EZ80_SPI_CONTROL_RESET 0x80u

/* SPI status bits */
#define EZ80_SPI_STATUS_TX_EMPTY 0x01u
#define EZ80_SPI_STATUS_RX_READY 0x02u
#define EZ80_SPI_STATUS_BUSY 0x04u
#define EZ80_SPI_STATUS_OVERRUN 0x08u
#define EZ80_SPI_STATUS_IRQ_PENDING 0x10u
#define EZ80_SPI_STATUS_CS_ACTIVE 0x20u
#define EZ80_SPI_STATUS_UNDERRUN 0x40u

/* SPI IRQ source */
#define EZ80_IRQ_SOURCE_SPI 0x20u

/* GPIO controller constants */
#define EZ80_GPIO_PORT_COUNT 6u
#define EZ80_GPIO_PORT_STRIDE 0x10u
#define EZ80_GPIO_REG_DATA 0x00u
#define EZ80_GPIO_REG_DIR 0x01u
#define EZ80_GPIO_REG_ALT0 0x02u
#define EZ80_GPIO_REG_ALT1 0x03u
#define EZ80_GPIO_REG_PULLUP 0x04u
#define EZ80_GPIO_REG_PULLDOWN 0x05u
#define EZ80_GPIO_REG_IRQ_RISING 0x06u
#define EZ80_GPIO_REG_IRQ_FALLING 0x07u
#define EZ80_GPIO_REG_IRQ_PENDING 0x08u

/* GPIO port addresses (16-bit, above legacy 8-bit range to avoid conflicts) */
#define EZ80_PORT_GPIO_A_DATA 0x3000u
#define EZ80_PORT_GPIO_A_DIR 0x3001u
#define EZ80_PORT_GPIO_A_ALT0 0x3002u
#define EZ80_PORT_GPIO_A_ALT1 0x3003u
#define EZ80_PORT_GPIO_B_DATA 0x3010u
#define EZ80_PORT_GPIO_B_DIR 0x3011u
#define EZ80_PORT_GPIO_B_ALT0 0x3012u
#define EZ80_PORT_GPIO_B_ALT1 0x3013u
#define EZ80_PORT_GPIO_C_DATA 0x3020u
#define EZ80_PORT_GPIO_C_DIR 0x3021u
#define EZ80_PORT_GPIO_C_ALT0 0x3022u
#define EZ80_PORT_GPIO_C_ALT1 0x3023u
#define EZ80_PORT_GPIO_D_DATA 0x3030u
#define EZ80_PORT_GPIO_D_DIR 0x3031u
#define EZ80_PORT_GPIO_D_ALT0 0x3032u
#define EZ80_PORT_GPIO_D_ALT1 0x3033u
#define EZ80_PORT_GPIO_E_DATA 0x3040u
#define EZ80_PORT_GPIO_E_DIR 0x3041u
#define EZ80_PORT_GPIO_E_ALT0 0x3042u
#define EZ80_PORT_GPIO_E_ALT1 0x3043u
#define EZ80_PORT_GPIO_F_DATA 0x3050u
#define EZ80_PORT_GPIO_F_DIR 0x3051u
#define EZ80_PORT_GPIO_F_ALT0 0x3052u
#define EZ80_PORT_GPIO_F_ALT1 0x3053u

/* TI-84 CE GPIO pin assignments */
#define EZ80_GPIO_BACKLIGHT_PORT 3u  /* GPIO D */
#define EZ80_GPIO_BACKLIGHT_BIT 0x20u /* bit 5 */
#define EZ80_GPIO_POWER_PORT 4u      /* GPIO E */
#define EZ80_GPIO_POWER_BIT 0x01u    /* bit 0 = power button */
#define EZ80_GPIO_CHARGING_BIT 0x02u /* bit 1 = charging status */
#define EZ80_GPIO_USB_PORT 5u        /* GPIO F */
#define EZ80_GPIO_USB_DETECT_BIT 0x01u

/* Flash erase constants */
#define EZ80_FLASH_ERASE_STATUS_NONE 0u
#define EZ80_FLASH_ERASE_STATUS_SUCCESS 1u
#define EZ80_FLASH_ERASE_STATUS_LOCKED 2u
#define EZ80_FLASH_ERASE_STATUS_PROTECTED 3u
#define EZ80_FLASH_ERASE_STATUS_OS_LOCKED 4u

/* I2C controller constants */
#define EZ80_I2C_CHANNEL_COUNT 2u
#define EZ80_I2C_BUFFER_SIZE 32u

/* I2C port addresses (16-bit, above legacy 8-bit range to avoid conflicts) */
#define EZ80_PORT_I2C0_BASE 0x5000u
#define EZ80_PORT_I2C0_SAR 0x5000u   /* slave address register */
#define EZ80_PORT_I2C0_XSAR 0x5001u  /* extended slave address */
#define EZ80_PORT_I2C0_DR 0x5002u    /* data register */
#define EZ80_PORT_I2C0_CTL 0x5003u   /* control register */
#define EZ80_PORT_I2C0_SR 0x5004u    /* status register */
#define EZ80_PORT_I2C0_CCR 0x5005u   /* clock control register */
#define EZ80_PORT_I2C0_SRR 0x5006u   /* software reset register */
#define EZ80_PORT_I2C1_BASE 0x5010u
#define EZ80_PORT_I2C1_SAR 0x5010u
#define EZ80_PORT_I2C1_XSAR 0x5011u
#define EZ80_PORT_I2C1_DR 0x5012u
#define EZ80_PORT_I2C1_CTL 0x5013u
#define EZ80_PORT_I2C1_SR 0x5014u
#define EZ80_PORT_I2C1_CCR 0x5015u
#define EZ80_PORT_I2C1_SRR 0x5016u

/* I2C control bits */
#define EZ80_I2C_CTL_IEN 0x80u    /* interrupt enable */
#define EZ80_I2C_CTL_ENAB 0x40u   /* I2C enable */
#define EZ80_I2C_CTL_STA 0x20u    /* start condition */
#define EZ80_I2C_CTL_STP 0x10u    /* stop condition */
#define EZ80_I2C_CTL_IFLG 0x08u   /* interrupt flag */
#define EZ80_I2C_CTL_AAK 0x04u    /* assert acknowledge */

/* I2C status codes (subset of real eZ80 I2C status) */
#define EZ80_I2C_SR_IDLE 0xF8u
#define EZ80_I2C_SR_START_SENT 0x08u
#define EZ80_I2C_SR_REP_START_SENT 0x10u
#define EZ80_I2C_SR_SLA_W_ACK 0x18u
#define EZ80_I2C_SR_SLA_W_NACK 0x20u
#define EZ80_I2C_SR_DATA_W_ACK 0x28u
#define EZ80_I2C_SR_DATA_W_NACK 0x30u
#define EZ80_I2C_SR_ARB_LOST 0x38u
#define EZ80_I2C_SR_SLA_R_ACK 0x40u
#define EZ80_I2C_SR_SLA_R_NACK 0x48u
#define EZ80_I2C_SR_DATA_R_ACK 0x50u
#define EZ80_I2C_SR_DATA_R_NACK 0x58u
#define EZ80_I2C_SR_BUS_ERROR 0x00u

/* I2C IRQ source */
#define EZ80_IRQ_SOURCE_I2C 0x40u

/* I2C MMIO telemetry registers */
#define EZ80_MMIO_REG_I2C0_CTL 0x0210u
#define EZ80_MMIO_REG_I2C0_SR 0x0211u
#define EZ80_MMIO_REG_I2C0_CCR 0x0212u
#define EZ80_MMIO_REG_I2C0_TX_COUNT_LO 0x0213u
#define EZ80_MMIO_REG_I2C0_TX_COUNT_HI 0x0214u
#define EZ80_MMIO_REG_I2C0_RX_COUNT_LO 0x0215u
#define EZ80_MMIO_REG_I2C0_RX_COUNT_HI 0x0216u
#define EZ80_MMIO_REG_I2C1_CTL 0x0217u
#define EZ80_MMIO_REG_I2C1_SR 0x0218u
#define EZ80_MMIO_REG_I2C1_CCR 0x0219u
#define EZ80_MMIO_REG_I2C1_TX_COUNT_LO 0x021Au
#define EZ80_MMIO_REG_I2C1_TX_COUNT_HI 0x021Bu
#define EZ80_MMIO_REG_I2C1_RX_COUNT_LO 0x021Cu
#define EZ80_MMIO_REG_I2C1_RX_COUNT_HI 0x021Du
#define EZ80_MMIO_REG_I2C0_SAR 0x0232u
#define EZ80_MMIO_REG_I2C0_XSAR 0x0233u
#define EZ80_MMIO_REG_I2C0_DATA 0x0234u
#define EZ80_MMIO_REG_I2C1_SAR 0x0235u
#define EZ80_MMIO_REG_I2C1_XSAR 0x0236u
#define EZ80_MMIO_REG_I2C1_DATA 0x0237u

/* UART controller constants */
#define EZ80_UART_CHANNEL_COUNT 2u
#define EZ80_UART_FIFO_SIZE 16u

/* UART port addresses (16-bit, above legacy 8-bit range to avoid conflicts) */
#define EZ80_PORT_UART0_BASE 0x5020u
#define EZ80_PORT_UART0_THR 0x5020u  /* transmit holding register (write) */
#define EZ80_PORT_UART0_RBR 0x5020u  /* receive buffer register (read) */
#define EZ80_PORT_UART0_BRG_L 0x5020u /* baud rate generator low (DLAB=1) */
#define EZ80_PORT_UART0_IER 0x5021u  /* interrupt enable register */
#define EZ80_PORT_UART0_BRG_H 0x5021u /* baud rate generator high (DLAB=1) */
#define EZ80_PORT_UART0_IIR 0x5022u  /* interrupt identification (read) */
#define EZ80_PORT_UART0_FCTL 0x5022u /* FIFO control (write) */
#define EZ80_PORT_UART0_LCTL 0x5023u /* line control register */
#define EZ80_PORT_UART0_MCR 0x5024u  /* modem control register */
#define EZ80_PORT_UART0_LSR 0x5025u  /* line status register */
#define EZ80_PORT_UART0_MSR 0x5026u  /* modem status register */
#define EZ80_PORT_UART0_SPR 0x5027u  /* scratchpad register */
#define EZ80_PORT_UART1_BASE 0x5030u
#define EZ80_PORT_UART1_THR 0x5030u
#define EZ80_PORT_UART1_RBR 0x5030u
#define EZ80_PORT_UART1_BRG_L 0x5030u
#define EZ80_PORT_UART1_IER 0x5031u
#define EZ80_PORT_UART1_BRG_H 0x5031u
#define EZ80_PORT_UART1_IIR 0x5032u
#define EZ80_PORT_UART1_FCTL 0x5032u
#define EZ80_PORT_UART1_LCTL 0x5033u
#define EZ80_PORT_UART1_MCR 0x5034u
#define EZ80_PORT_UART1_LSR 0x5035u
#define EZ80_PORT_UART1_MSR 0x5036u
#define EZ80_PORT_UART1_SPR 0x5037u

/* UART line control bits */
#define EZ80_UART_LCTL_DLAB 0x80u    /* divisor latch access bit */
#define EZ80_UART_LCTL_SB 0x40u     /* set break */
#define EZ80_UART_LCTL_FPE 0x20u    /* force parity error */
#define EZ80_UART_LCTL_EPS 0x10u    /* even parity select */
#define EZ80_UART_LCTL_PEN 0x08u    /* parity enable */
#define EZ80_UART_LCTL_2STOP 0x04u  /* 2 stop bits */
#define EZ80_UART_LCTL_CHAR_MASK 0x03u /* character size: 0=5, 1=6, 2=7, 3=8 */

/* UART line status bits */
#define EZ80_UART_LSR_ERR 0x80u     /* error in RX FIFO */
#define EZ80_UART_LSR_TEMT 0x40u    /* transmitter empty */
#define EZ80_UART_LSR_THRE 0x20u    /* TX holding register empty */
#define EZ80_UART_LSR_BI 0x10u      /* break interrupt */
#define EZ80_UART_LSR_FE 0x08u      /* framing error */
#define EZ80_UART_LSR_PE 0x04u      /* parity error */
#define EZ80_UART_LSR_OE 0x02u      /* overrun error */
#define EZ80_UART_LSR_DR 0x01u      /* data ready */

/* UART interrupt enable bits */
#define EZ80_UART_IER_RECV 0x01u    /* received data available */
#define EZ80_UART_IER_THRE 0x02u    /* TX holding register empty */
#define EZ80_UART_IER_LSR 0x04u     /* receiver line status */
#define EZ80_UART_IER_MSR 0x08u     /* modem status */

/* UART FIFO control bits */
#define EZ80_UART_FCTL_FIFOEN 0x01u  /* FIFO enable */
#define EZ80_UART_FCTL_CLRRx 0x02u  /* clear receive FIFO */
#define EZ80_UART_FCTL_CLRTx 0x04u  /* clear transmit FIFO */
#define EZ80_UART_FCTL_TRIG_MASK 0xC0u /* receive trigger level */

/* UART IRQ source */
#define EZ80_IRQ_SOURCE_UART 0x80u

/* UART MMIO telemetry registers */
#define EZ80_MMIO_REG_UART0_LCTL 0x0220u
#define EZ80_MMIO_REG_UART0_LSR 0x0221u
#define EZ80_MMIO_REG_UART0_IER 0x0222u
#define EZ80_MMIO_REG_UART0_BRG_LO 0x0223u
#define EZ80_MMIO_REG_UART0_BRG_HI 0x0224u
#define EZ80_MMIO_REG_UART0_TX_COUNT_LO 0x0225u
#define EZ80_MMIO_REG_UART0_TX_COUNT_HI 0x0226u
#define EZ80_MMIO_REG_UART0_RX_COUNT_LO 0x0227u
#define EZ80_MMIO_REG_UART0_RX_COUNT_HI 0x0228u
#define EZ80_MMIO_REG_UART1_LCTL 0x0229u
#define EZ80_MMIO_REG_UART1_LSR 0x022Au
#define EZ80_MMIO_REG_UART1_IER 0x022Bu
#define EZ80_MMIO_REG_UART1_BRG_LO 0x022Cu
#define EZ80_MMIO_REG_UART1_BRG_HI 0x022Du
#define EZ80_MMIO_REG_UART1_TX_COUNT_LO 0x022Eu
#define EZ80_MMIO_REG_UART1_TX_COUNT_HI 0x022Fu
#define EZ80_MMIO_REG_UART1_RX_COUNT_LO 0x0230u
#define EZ80_MMIO_REG_UART1_RX_COUNT_HI 0x0231u
#define EZ80_MMIO_REG_UART0_DATA 0x0238u
#define EZ80_MMIO_REG_UART0_FCTL 0x0239u
#define EZ80_MMIO_REG_UART0_MCR 0x023Au
#define EZ80_MMIO_REG_UART0_SPR 0x023Bu
#define EZ80_MMIO_REG_UART1_DATA 0x023Cu
#define EZ80_MMIO_REG_UART1_FCTL 0x023Du
#define EZ80_MMIO_REG_UART1_MCR 0x023Eu
#define EZ80_MMIO_REG_UART1_SPR 0x023Fu

/* Execution trace constants */
#define EZ80_TRACE_BUFFER_SIZE 64u

/* Execution trace MMIO telemetry registers */
#define EZ80_MMIO_REG_TRACE_ENABLED 0x0240u
#define EZ80_MMIO_REG_TRACE_COUNT_LO 0x0241u
#define EZ80_MMIO_REG_TRACE_COUNT_HI 0x0242u
#define EZ80_MMIO_REG_TRACE_HEAD 0x0243u

/* RTC Alarm MMIO registers */
#define EZ80_MMIO_REG_RTC_ALARM_SECONDS 0x0250u
#define EZ80_MMIO_REG_RTC_ALARM_MINUTES 0x0251u
#define EZ80_MMIO_REG_RTC_ALARM_HOURS 0x0252u
#define EZ80_MMIO_REG_RTC_ALARM_ENABLED 0x0253u
#define EZ80_MMIO_REG_RTC_ALARM_FIRED 0x0254u
#define EZ80_MMIO_REG_RTC_ALARM_FIRE_COUNT 0x0255u

/* Hardware watchdog timer MMIO registers */
#define EZ80_MMIO_REG_WDT_CONTROL 0x0260u
#define EZ80_MMIO_REG_WDT_COUNTER_LO 0x0261u
#define EZ80_MMIO_REG_WDT_COUNTER_HI 0x0262u
#define EZ80_MMIO_REG_WDT_TIMEOUT_LO 0x0263u
#define EZ80_MMIO_REG_WDT_TIMEOUT_HI 0x0264u
#define EZ80_MMIO_REG_WDT_RESET_COUNT 0x0265u
#define EZ80_MMIO_REG_WDT_LAST_RESET_CYCLE_LO 0x0266u
#define EZ80_MMIO_REG_WDT_LAST_RESET_CYCLE_HI 0x0267u

/* Power management MMIO registers */
#define EZ80_MMIO_REG_PM_MODE 0x0270u
#define EZ80_MMIO_REG_PM_WAKE_SOURCE 0x0271u
#define EZ80_MMIO_REG_PM_SLEEP_COUNT 0x0272u
#define EZ80_MMIO_REG_PM_TOTAL_SLEEP_CYCLES_LO 0x0273u
#define EZ80_MMIO_REG_PM_TOTAL_SLEEP_CYCLES_HI 0x0274u

/* Watchpoint types */
#define EZ80_WATCHPOINT_READ  0x01u
#define EZ80_WATCHPOINT_WRITE 0x02u
#define EZ80_WATCHPOINT_READWRITE 0x03u

#define EZ80_OS_VAR_NAME_MAX 8u
#define EZ80_OS_MAX_VARS 64u
#define EZ80_HOME_ROWS 10u
#define EZ80_HOME_COLS 26u
#define EZ80_SCREEN_ASCII_ROWS 24u
#define EZ80_SCREEN_ASCII_COLS 40u
#define EZ80_GRAPH_WIDTH 320u
#define EZ80_GRAPH_HEIGHT 240u

#define EZ80_BCALL_HOME_CLEAR 0x4000u
#define EZ80_BCALL_HOME_PUTC 0x4001u
#define EZ80_BCALL_GRAPH_CLEAR 0x4010u
#define EZ80_BCALL_GRAPH_PLOT 0x4011u
#define EZ80_BCALL_GRAPH_RENDER_TO_LCD 0x4012u
#define EZ80_BCALL_VAR_PUT 0x4020u
#define EZ80_BCALL_VAR_GET 0x4021u
#define EZ80_BCALL_VAR_DELETE 0x4022u
#define EZ80_BCALL_VAR_EXISTS 0x4023u
#define EZ80_BCALL_VAR_GET_INFO 0x4024u
#define EZ80_BCALL_PROGRAM_ARCHIVE_TO_RAM 0x4030u
#define EZ80_BCALL_PROGRAM_RETURN_TO_ARCHIVE 0x4031u
#define EZ80_BCALL_PROGRAM_LAUNCH 0x4032u
#define EZ80_BCALL_PROGRAM_FINISH 0x4033u

#define EZ80_OS_VAR_REAL 0u
#define EZ80_OS_VAR_COMPLEX 1u
#define EZ80_OS_VAR_MATRIX 2u
#define EZ80_OS_VAR_LIST 3u
#define EZ80_OS_VAR_STRING 4u
#define EZ80_OS_VAR_PROGRAM 5u

#define EZ80_ARTIFICE_ENTRY_NONE 0u
#define EZ80_ARTIFICE_ENTRY_KEY_SEQUENCE 1u
#define EZ80_ARTIFICE_ENTRY_MEMORY_PATCH 2u

#define EZ80_DNGRADE_FAILURE_NONE 0u
#define EZ80_DNGRADE_FAILURE_PORT28_BIT3_BLOCKED 1u
#define EZ80_DNGRADE_FAILURE_OS58_PORT28_BIT3_REQUIRED 2u
#define EZ80_DNGRADE_FAILURE_FLASH_NOT_UNLOCKED 3u
#define EZ80_DNGRADE_FAILURE_TIMEOUT 4u
#define EZ80_DNGRADE_FAILURE_WATCHDOG_RESET 5u
#define EZ80_DNGRADE_FAILURE_INVALID_STRATEGY 6u
#define EZ80_DNGRADE_FAILURE_FLASH_SECTOR_PROTECTED 7u
#define EZ80_DNGRADE_FAILURE_FLASH_REQUIRES_ERASE 8u

#define EZ80_DNGRADE_STRATEGY_NONE 0u
#define EZ80_DNGRADE_STRATEGY_DIRECT_FLASH_WRITE 1u
#define EZ80_DNGRADE_STRATEGY_PORT28_UNLOCK_THEN_WRITE 2u
#define EZ80_DNGRADE_STRATEGY_CERT_UNLOCK_THEN_WRITE 3u

#define EZ80_DNGRADE_ATTEMPT_STATUS_NONE 0u
#define EZ80_DNGRADE_ATTEMPT_STATUS_SUCCESS 1u
#define EZ80_DNGRADE_ATTEMPT_STATUS_FAILURE 2u

#define EZ80_FLASH_PROTECT_MODE_PERMISSIVE 0u
#define EZ80_FLASH_PROTECT_MODE_LOCKED 1u

#define EZ80_FLASH_SECTOR_FLAG_PROTECTED 0x01u
#define EZ80_FLASH_SECTOR_FLAG_OS_LOCKED 0x02u

#define EZ80_WRITEFLASH_STATUS_NONE 0u
#define EZ80_WRITEFLASH_STATUS_SUCCESS 1u
#define EZ80_WRITEFLASH_STATUS_FLASH_LOCKED 2u
#define EZ80_WRITEFLASH_STATUS_SECTOR_PROTECTED 3u
#define EZ80_WRITEFLASH_STATUS_REQUIRES_ERASE 4u

#define EZ80_OS_SWAP_FAILURE_NONE 0u
#define EZ80_OS_SWAP_FAILURE_PROGRAM_NOT_FOUND 1u
#define EZ80_OS_SWAP_FAILURE_RAM_CAPACITY 2u
#define EZ80_OS_SWAP_FAILURE_ARCHIVE_RANGE 3u
#define EZ80_OS_SWAP_FAILURE_INTEGRITY 4u
#define EZ80_OS_SWAP_FAILURE_INVALID_STATE 5u

#define EZ80_OS_VAR_INFO_BYTES 13u

#define EZ80_ARTIFICE_PATCH_ADDR (EZ80_RAM_BASE + 0x5A58u)
#define EZ80_ARTIFICE_PATCH_MAGIC 0xCEu

typedef enum {
    EZ80_OK = 0,
    EZ80_ERR_INVALID_ARG,
    EZ80_ERR_NOT_INITIALIZED,
    EZ80_ERR_NOT_IMPLEMENTED,
    EZ80_ERR_ROM_FORMAT,
    EZ80_ERR_OUT_OF_BOUNDS
} ez80_status_t;

typedef ez80_status_t (*ez80_port_read_handler_t)(ez80_emulator_t *emu,
                                                   uint16_t port,
                                                   uint8_t *out_value,
                                                   void *user_data);

typedef ez80_status_t (*ez80_port_write_handler_t)(ez80_emulator_t *emu,
                                                    uint16_t port,
                                                    uint8_t value,
                                                    void *user_data);

typedef struct {
    bool enabled;
    uint16_t start_port;
    uint16_t end_port;
    ez80_port_read_handler_t read_handler;
    ez80_port_write_handler_t write_handler;
    void *user_data;
} ez80_port_handler_slot_t;

typedef struct {
    uint32_t pc;      /* 24-bit in ADL mode, stored in low 24 bits */
    uint32_t sp;      /* 24-bit stack pointer in ADL mode */
    uint32_t ix;
    uint32_t iy;
    uint32_t af;
    uint32_t bc;
    uint32_t de;
    uint32_t hl;
    uint32_t af_alt;
    uint32_t bc_alt;
    uint32_t de_alt;
    uint32_t hl_alt;
    uint8_t i_reg;
    uint16_t i_reg16;
    uint8_t r_reg;
    uint8_t mbase_reg;
    uint8_t interrupt_mode; /* 0,1,2 */
    bool iff1;
    bool iff2;
    bool adl_mode;
    bool adl_encoding;  /* true = decode with 24-bit immediates/addresses */
    bool adl_mode_override_active;
    bool adl_mode_override;
    bool adl_encoding_override_active;
    bool adl_encoding_override;
    bool mixed_mode;
    bool halted;
    bool ei_pending;    /* EI delays interrupt enable by one instruction */
} ez80_cpu_state_t;

typedef struct {
    uint32_t cycles_executed;
    bool hit_breakpoint;
    bool hit_halt;
    bool hit_watchpoint;
    uint32_t watchpoint_addr;
    uint8_t watchpoint_type; /* EZ80_WATCHPOINT_READ or _WRITE */
} ez80_run_result_t;

typedef struct {
    uint32_t addr;
    uint32_t length;
    uint8_t type; /* EZ80_WATCHPOINT_READ, _WRITE, or _READWRITE */
} ez80_watchpoint_t;

typedef struct {
    uint8_t bytes[6];
    uint8_t length;
    uint8_t opcode;
    uint8_t cycles;
    char mnemonic[32];
    char operands[64];
} ez80_instruction_t;

typedef struct {
    uint8_t flash_window[EZ80_FLASH_WINDOW_BYTES];
    uint8_t ram[EZ80_RAM_BYTES];
    uint8_t ports[EZ80_PORT_BYTES];
    uint8_t port_space[EZ80_PORT_SPACE_BYTES];
    uint8_t system_mmio[EZ80_SYSTEM_MMIO_BYTES];
    uint8_t mmio[EZ80_MMIO_BYTES];
    uint32_t flash_bytes_loaded;
} ez80_memory_t;

typedef struct {
    uint8_t os_major;
    uint8_t os_minor;
    uint8_t os_patch;
    bool os_version_detected;
    bool native_unsigned_execution_allowed;
    bool artifice_bypass_active;
    uint8_t artifice_entry_method;
    bool dngrade_port28_bit3_blocked;
    uint8_t dngrade_last_failure;
    uint8_t dngrade_last_port;
    uint8_t dngrade_last_strategy;
    uint8_t dngrade_last_attempt_status;
    bool dngrade_last_timeout;
    bool dngrade_last_watchdog;
    bool dngrade_last_write_ok;
    uint32_t dngrade_last_cycles;
    uint8_t port28_unlock_read_latch;
    uint8_t artifice_key_window[4];
    uint8_t artifice_key_window_count;
    uint8_t flash_protect_mode;
    uint8_t flash_sector_select;
    uint8_t flash_sector_protected[EZ80_FLASH_SECTOR_COUNT];
    uint8_t writeflash_last_status;
    uint32_t writeflash_last_addr;
    uint8_t writeflash_last_requested;
    uint8_t writeflash_last_before;
    uint8_t writeflash_last_after;
} ez80_platform_state_t;

typedef struct {
    bool in_use;
    uint8_t type;
    char name[EZ80_OS_VAR_NAME_MAX + 1u];
    bool archived;
    bool loaded_in_ram;
    uint32_t size_bytes;
    uint32_t archive_addr;
    uint32_t ram_addr;
    uint32_t archive_crc32;
    uint32_t ram_crc32;
} ez80_os_var_slot_t;

typedef struct {
    bool exists;
    uint8_t type;
    char name[EZ80_OS_VAR_NAME_MAX + 1u];
    bool archived;
    bool loaded_in_ram;
    uint32_t size_bytes;
    uint32_t archive_addr;
    uint32_t ram_addr;
} ez80_os_var_info_t;

typedef struct {
    uint8_t strategy;
    uint8_t attempt_status;
    uint8_t failure_reason;
    uint8_t failure_port;
    uint32_t target_flash_addr;
    uint8_t requested_value;
    uint8_t observed_before;
    uint8_t observed_after;
    uint32_t cycles_executed;
    bool write_attempted;
    bool write_succeeded;
    bool timed_out;
    bool watchdog_reset;
} ez80_dngrade_attempt_result_t;

typedef struct {
    ez80_os_var_slot_t vars[EZ80_OS_MAX_VARS];
    uint32_t ram_pool_next;
    uint32_t archive_pool_next;
    uint8_t home_row;
    uint8_t home_col;
    uint16_t last_bcall_vector;
    uint8_t last_bcall_status;
    uint32_t last_bcall_result;
    uint16_t graph_last_x;
    uint16_t graph_last_y;
    uint8_t graph_last_color;
    char active_program_name[EZ80_OS_VAR_NAME_MAX + 1u];
    uint32_t active_program_ram_addr;
    uint32_t launch_count;
    uint32_t swap_count;
    uint32_t swap_fail_count;
    uint8_t last_swap_failure;
    uint32_t last_swap_archive_crc32;
    uint32_t last_swap_ram_crc32;
} ez80_tios_state_t;

typedef struct {
    uint32_t src_addr;
    uint32_t dst_addr;
    uint32_t length;
    uint8_t control;
    uint8_t status;
    uint32_t bytes_transferred;
} ez80_dma_channel_t;

typedef struct {
    uint8_t control;
    uint8_t status;
    uint8_t baud_divisor;
    uint8_t tx_fifo[EZ80_SPI_FIFO_SIZE];
    uint8_t rx_fifo[EZ80_SPI_FIFO_SIZE];
    uint8_t rx_source_fifo[EZ80_SPI_FIFO_SIZE];
    uint8_t tx_head;
    uint8_t tx_count;
    uint8_t rx_head;
    uint8_t rx_count;
    uint8_t rx_source_head;
    uint8_t rx_source_count;
    uint32_t total_tx_bytes;
    uint32_t total_rx_bytes;
    uint8_t last_mode;
    uint8_t last_tx_byte;
    uint8_t last_rx_byte;
    uint8_t active_burst_len;
    uint8_t last_burst_len;
    uint32_t transfer_complete_count;
    uint32_t cs_assert_count;
    uint32_t cs_deassert_count;
    uint32_t last_transfer_cycle;
    uint32_t last_cs_assert_cycle;
    uint32_t last_cs_deassert_cycle;
} ez80_spi_channel_t;

typedef struct {
    uint8_t data;
    uint8_t direction;  /* 0=input, 1=output per bit */
    uint8_t alt0;
    uint8_t alt1;
    uint8_t output_latch;
    uint8_t input_latch;
    uint8_t input_mask;
    uint8_t pull_up;
    uint8_t pull_down;
    uint8_t irq_rising;
    uint8_t irq_falling;
    uint8_t irq_pending;
    uint8_t last_sampled;
} ez80_gpio_port_t;

typedef struct {
    uint8_t control;
    uint8_t status;
    uint8_t clock_control;
    uint8_t slave_addr;
    uint8_t xslave_addr;
    uint8_t tx_buf[EZ80_I2C_BUFFER_SIZE];
    uint8_t rx_buf[EZ80_I2C_BUFFER_SIZE];
    uint8_t tx_head;
    uint8_t tx_count;
    uint8_t rx_head;
    uint8_t rx_count;
    uint32_t total_tx_bytes;
    uint32_t total_rx_bytes;
    bool start_pending;
    bool stop_pending;
} ez80_i2c_channel_t;

typedef struct {
    uint8_t lctl;        /* line control register */
    uint8_t lsr;         /* line status register */
    uint8_t ier;         /* interrupt enable register */
    uint8_t iir;         /* interrupt identification */
    uint8_t mcr;         /* modem control register */
    uint8_t msr;         /* modem status register */
    uint8_t spr;         /* scratchpad register */
    uint8_t fctl;        /* FIFO control */
    uint16_t brg;        /* baud rate generator divisor */
    uint8_t tx_fifo[EZ80_UART_FIFO_SIZE];
    uint8_t rx_fifo[EZ80_UART_FIFO_SIZE];
    uint8_t tx_head;
    uint8_t tx_count;
    uint8_t rx_head;
    uint8_t rx_count;
    uint32_t total_tx_bytes;
    uint32_t total_rx_bytes;
} ez80_uart_channel_t;

typedef struct {
    uint32_t pc;
    uint8_t opcode;
    uint8_t length;
    uint8_t bytes[6];
} ez80_trace_entry_t;

typedef struct {
    bool homescreen_active;
    uint8_t cursor_row;
    uint8_t cursor_col;
    uint8_t visible_line_count;
    uint8_t step_line_count;
    char input_line[EZ80_HOME_COLS + 1u];
    char visible_lines[EZ80_HOME_ROWS][EZ80_HOME_COLS + 1u];
    char step_lines[EZ80_HOME_ROWS][EZ80_HOME_COLS + 1u];
    char ascii[(EZ80_SCREEN_ASCII_ROWS * (EZ80_SCREEN_ASCII_COLS + 1u)) + 1u];
} ez80_screen_text_state_t;

typedef struct {
    uint8_t keypad_row_select;
    uint8_t keypad_rows[8];
    uint8_t keypad_regs[EZ80_KEYPAD_MMIO_BYTES];
    uint8_t kbd_group_select;
    bool kbd_irq_on_keypress;
    uint8_t kbd_last_state;
    uint8_t lcd_last_command;
    uint8_t lcd_last_data;
    uint32_t lcd_write_index;
    bool lcd_pending_low_byte;
    uint8_t lcd_low_byte;
    uint8_t lcd_command_param_count;
    uint8_t lcd_command_param_expected;
    uint8_t lcd_command_params[4];
    uint8_t lcd_mode;
    uint8_t lcd_timing;
    uint16_t lcd_window_x0;
    uint16_t lcd_window_y0;
    uint16_t lcd_window_x1;
    uint16_t lcd_window_y1;
    uint32_t lcd_vram_base;
    uint8_t lcd_status;
    uint8_t lcd_pipeline_state;
    uint8_t usb_control;
    uint8_t usb_status;
    uint8_t usb_rx_data;
    uint8_t usb_last_tx_data;
    uint8_t usb_tx_fifo[EZ80_USB_FIFO_SIZE];
    uint8_t usb_rx_fifo[EZ80_USB_FIFO_SIZE];
    uint8_t usb_tx_head;
    uint8_t usb_tx_count;
    uint8_t usb_rx_head;
    uint8_t usb_rx_count;
    uint32_t usb_total_tx_bytes;
    uint32_t usb_total_rx_bytes;
    bool usb_connected;
    uint16_t timer0_counter;
    uint16_t timer1_counter;
    uint8_t timer0_control;
    uint8_t timer1_control;
    uint16_t timer0_reload;
    uint16_t timer1_reload;
    uint8_t timer0_prescaler;  /* 0=1x, 1=4x, 2=16x, 3=64x */
    uint8_t timer1_prescaler;
    uint32_t timer0_prescaler_accum;
    uint32_t timer1_prescaler_accum;
    uint32_t rtc_cycle_accumulator;
    uint32_t rtc_seconds_of_day;
    uint8_t irq_mask;
    uint8_t irq_pending;
    uint32_t total_interrupts_serviced;
    uint8_t last_irq_source;
    uint32_t last_irq_vector_addr;
    /* DMA controller */
    ez80_dma_channel_t dma[EZ80_DMA_CHANNEL_COUNT];
    uint32_t dma_total_transfers;
    /* Interrupt nesting */
    uint8_t irq_nesting_depth;
    uint8_t irq_nesting_stack[EZ80_IRQ_MAX_NESTING]; /* source of each nested IRQ */
    uint32_t total_reti_count;
    uint8_t last_reti_source;
    /* NMI state */
    bool nmi_pending;
    bool nmi_active;
    uint32_t nmi_count;
    bool nmi_iff1_saved; /* IFF1 value saved on NMI entry */
    uint8_t nmi_nesting_depth;
    bool flash_boot_nmi_armed;
    uint8_t flash_boot_nmi_status;
    bool boot_irq_overlay_active;
    bool boot_irq_halt_release_pending;
    uint8_t boot_irq_status14;
    uint8_t boot_irq_status15;
    uint8_t boot_irq_status16;
    /* SPI controller */
    ez80_spi_channel_t spi[EZ80_SPI_CHANNEL_COUNT];
    /* GPIO controller */
    ez80_gpio_port_t gpio[EZ80_GPIO_PORT_COUNT];
    bool backlight_on;
    uint8_t battery_level; /* 0-100 */
    uint8_t power_state;   /* 0=off, 1=on, 2=standby */
    /* Flash erase */
    uint8_t flash_erase_last_sector;
    uint8_t flash_erase_last_status;
    uint32_t flash_erase_count;
    /* I2C controller */
    ez80_i2c_channel_t i2c[EZ80_I2C_CHANNEL_COUNT];
    /* UART controller */
    ez80_uart_channel_t uart[EZ80_UART_CHANNEL_COUNT];
    /* Execution trace */
    ez80_trace_entry_t trace_buf[EZ80_TRACE_BUFFER_SIZE];
    uint32_t trace_head;
    uint32_t trace_count;
    bool trace_enabled;
    /* RTC alarm */
    uint8_t rtc_alarm_seconds;
    uint8_t rtc_alarm_minutes;
    uint8_t rtc_alarm_hours;
    uint8_t rtc_alarm_control;
    bool rtc_alarm_fired;
    uint32_t rtc_alarm_fire_count;
    /* Hardware watchdog timer */
    uint8_t wdt_control;
    uint16_t wdt_counter;
    uint16_t wdt_timeout;
    uint32_t wdt_reset_count;
    uint32_t wdt_last_reset_cycle;
    uint32_t wdt_prescaler_accum;
    /* Power management */
    uint8_t pm_mode;
    uint8_t pm_wake_source;
    uint32_t pm_sleep_count;
    uint32_t pm_total_sleep_cycles;
    uint32_t total_cycles;
    bool halt_pc_advanced;
} ez80_hardware_state_t;

struct ez80_emulator {
    bool initialized;
    bool rom_loaded;
    char loaded_rom_path[260];
    ez80_cpu_state_t cpu;
    ez80_memory_t memory;
    ez80_platform_state_t platform;
    ez80_tios_state_t os;
    ez80_hardware_state_t hw;
    uint32_t breakpoints[EZ80_MAX_BREAKPOINTS];
    uint32_t breakpoint_count;
    ez80_watchpoint_t watchpoints[EZ80_MAX_WATCHPOINTS];
    uint32_t watchpoint_count;
    bool interrupts_enabled_in_emulator;
    bool watchpoint_triggered;
    uint32_t watchpoint_hit_addr;
    uint8_t watchpoint_hit_type;
    ez80_port_handler_slot_t port_handlers[EZ80_MAX_PORT_HANDLERS];
    bool cpu_instruction_port_access_active;
    uint32_t cpu_instruction_port_access_pc;
};

ez80_status_t ez80_emulator_init(ez80_emulator_t *emu);
ez80_status_t ez80_emulator_reset(ez80_emulator_t *emu);
ez80_status_t ez80_emulator_load_rom(ez80_emulator_t *emu, const char *rom_path);
ez80_status_t ez80_emulator_set_os_version(ez80_emulator_t *emu,
                                           uint8_t major,
                                           uint8_t minor,
                                           uint8_t patch);
ez80_status_t ez80_emulator_inject_key(ez80_emulator_t *emu, uint8_t key_code);
ez80_status_t ez80_emulator_apply_artifice_memory_patch(ez80_emulator_t *emu,
                                                         uint32_t addr,
                                                         uint8_t value);
ez80_status_t ez80_emulator_simulate_dngrade_attempt(ez80_emulator_t *emu,
                                                      uint8_t strategy,
                                                      uint32_t target_flash_addr,
                                                      uint8_t value,
                                                      uint32_t timeout_cycles,
                                                      uint32_t watchdog_cycles,
                                                      ez80_dngrade_attempt_result_t *out_result);
ez80_status_t ez80_emulator_bcall(ez80_emulator_t *emu,
                                  uint16_t vector,
                                  uint32_t arg0,
                                  uint32_t arg1,
                                  uint32_t arg2,
                                  uint32_t arg3,
                                  uint32_t *out_result);
ez80_status_t ez80_emulator_os_var_write(ez80_emulator_t *emu,
                                         const char *name,
                                         uint8_t type,
                                         const uint8_t *data,
                                         size_t size,
                                         bool archived);
ez80_status_t ez80_emulator_os_var_read(const ez80_emulator_t *emu,
                                        const char *name,
                                        uint8_t type,
                                        uint8_t *out,
                                        size_t cap,
                                        size_t *out_size);
ez80_status_t ez80_emulator_os_var_delete(ez80_emulator_t *emu,
                                          const char *name,
                                          uint8_t type);
ez80_status_t ez80_emulator_os_var_get_info(const ez80_emulator_t *emu,
                                            const char *name,
                                            uint8_t type,
                                            ez80_os_var_info_t *out_info);
ez80_status_t ez80_emulator_load_program_file(ez80_emulator_t *emu,
                                              const char *program_name,
                                              const char *program_path,
                                              bool archived);
ez80_status_t ez80_emulator_archive_program_to_ram(ez80_emulator_t *emu,
                                                   const char *program_name);
ez80_status_t ez80_emulator_return_program_to_archive(ez80_emulator_t *emu,
                                                      const char *program_name);
ez80_status_t ez80_emulator_launch_program(ez80_emulator_t *emu,
                                           const char *program_name,
                                           uint32_t *out_ram_addr);
ez80_status_t ez80_emulator_finish_program(ez80_emulator_t *emu,
                                           const char *program_name,
                                           uint32_t *out_archive_addr);
ez80_status_t ez80_emulator_home_get_line(const ez80_emulator_t *emu,
                                          uint8_t row,
                                          char *out,
                                          size_t cap);
ez80_status_t ez80_emulator_get_screen_text_state(const ez80_emulator_t *emu,
                                                  ez80_screen_text_state_t *out_state);
ez80_status_t ez80_emulator_graph_get_pixel(const ez80_emulator_t *emu,
                                            uint16_t x,
                                            uint16_t y,
                                            uint8_t *out);
ez80_status_t ez80_emulator_lcd_get_pixel(const ez80_emulator_t *emu,
                                          uint16_t x,
                                          uint16_t y,
                                          uint16_t *out);
ez80_status_t ez80_emulator_lcd_write_png(const ez80_emulator_t *emu,
                                          const char *path);
ez80_status_t ez80_emulator_step(ez80_emulator_t *emu, ez80_instruction_t *decoded);
ez80_status_t ez80_emulator_run_cycles(ez80_emulator_t *emu,
                                       uint32_t cycle_budget,
                                       ez80_run_result_t *out);
ez80_status_t ez80_emulator_get_state(const ez80_emulator_t *emu, ez80_cpu_state_t *out);
ez80_status_t ez80_memory_read8(const ez80_emulator_t *emu, uint32_t addr, uint8_t *out);
ez80_status_t ez80_memory_write8(ez80_emulator_t *emu, uint32_t addr, uint8_t value);
const char *ez80_status_string(ez80_status_t status);

/* Port I/O handler registration (callback-based dispatch). */
ez80_status_t ez80_emulator_set_port_handler(ez80_emulator_t *emu,
                                             uint8_t slot_index,
                                             uint16_t start_port,
                                             uint16_t end_port,
                                             ez80_port_read_handler_t read_handler,
                                             ez80_port_write_handler_t write_handler,
                                             void *user_data);
ez80_status_t ez80_emulator_clear_port_handlers(ez80_emulator_t *emu);

/* Breakpoint management */
ez80_status_t ez80_emulator_add_breakpoint(ez80_emulator_t *emu, uint32_t addr);
ez80_status_t ez80_emulator_remove_breakpoint(ez80_emulator_t *emu, uint32_t addr);
ez80_status_t ez80_emulator_clear_breakpoints(ez80_emulator_t *emu);

/* Interrupt control */
ez80_status_t ez80_emulator_enable_interrupts(ez80_emulator_t *emu, bool enable);
ez80_status_t ez80_emulator_raise_interrupt(ez80_emulator_t *emu, uint8_t source);

/* Key release (complement of inject_key which presses) */
ez80_status_t ez80_emulator_release_key(ez80_emulator_t *emu, uint8_t key_code);
ez80_status_t ez80_emulator_release_all_keys(ez80_emulator_t *emu);

/* LCD framebuffer access */
ez80_status_t ez80_emulator_lcd_get_framebuffer(const ez80_emulator_t *emu,
                                                 uint16_t *out_buffer,
                                                 size_t buffer_pixels);

/* LCD to ASCII text conversion for CLI output */
ez80_status_t ez80_emulator_lcd_to_ascii(const ez80_emulator_t *emu,
                                          char *out_buffer,
                                          size_t buffer_size,
                                          uint32_t *out_length);

/* Watchpoint management */
ez80_status_t ez80_emulator_add_watchpoint(ez80_emulator_t *emu,
                                            uint32_t addr,
                                            uint32_t length,
                                            uint8_t type);
ez80_status_t ez80_emulator_remove_watchpoint(ez80_emulator_t *emu,
                                               uint32_t addr);
ez80_status_t ez80_emulator_clear_watchpoints(ez80_emulator_t *emu);

/* Timer prescaler/reload configuration */
ez80_status_t ez80_emulator_set_timer_prescaler(ez80_emulator_t *emu,
                                                 uint8_t timer_id,
                                                 uint8_t prescaler);
ez80_status_t ez80_emulator_set_timer_reload(ez80_emulator_t *emu,
                                              uint8_t timer_id,
                                              uint16_t reload_value);

/* DMA controller */
ez80_status_t ez80_emulator_dma_configure(ez80_emulator_t *emu,
                                           uint8_t channel,
                                           uint32_t src_addr,
                                           uint32_t dst_addr,
                                           uint32_t length,
                                           uint8_t control);
ez80_status_t ez80_emulator_dma_start(ez80_emulator_t *emu, uint8_t channel);
ez80_status_t ez80_emulator_dma_abort(ez80_emulator_t *emu, uint8_t channel);
ez80_status_t ez80_emulator_dma_get_status(const ez80_emulator_t *emu,
                                            uint8_t channel,
                                            uint8_t *out_status,
                                            uint32_t *out_bytes_transferred);

/* NMI (non-maskable interrupt) */
ez80_status_t ez80_emulator_raise_nmi(ez80_emulator_t *emu);

/* SPI controller */
ez80_status_t ez80_emulator_spi_configure(ez80_emulator_t *emu,
                                           uint8_t channel,
                                           uint8_t control,
                                           uint8_t baud_divisor);
ez80_status_t ez80_emulator_spi_transfer(ez80_emulator_t *emu,
                                          uint8_t channel,
                                          uint8_t tx_byte,
                                          uint8_t *out_rx_byte);
ez80_status_t ez80_emulator_spi_get_status(const ez80_emulator_t *emu,
                                            uint8_t channel,
                                            uint8_t *out_status);
ez80_status_t ez80_emulator_spi_set_chip_select(ez80_emulator_t *emu,
                                                 uint8_t channel,
                                                 bool active);
ez80_status_t ez80_emulator_spi_load_rx(ez80_emulator_t *emu,
                                         uint8_t channel,
                                         uint8_t value);

/* GPIO controller */
ez80_status_t ez80_emulator_gpio_write(ez80_emulator_t *emu,
                                        uint8_t port_index,
                                        uint8_t value);
ez80_status_t ez80_emulator_gpio_read(const ez80_emulator_t *emu,
                                       uint8_t port_index,
                                       uint8_t *out_value);
ez80_status_t ez80_emulator_gpio_set_direction(ez80_emulator_t *emu,
                                                uint8_t port_index,
                                                uint8_t direction);
ez80_status_t ez80_emulator_gpio_drive_input(ez80_emulator_t *emu,
                                              uint8_t port_index,
                                              uint8_t value,
                                              uint8_t mask);
ez80_status_t ez80_emulator_set_backlight(ez80_emulator_t *emu, bool on);
ez80_status_t ez80_emulator_set_battery_level(ez80_emulator_t *emu,
                                               uint8_t level);

/* USB controller */
ez80_status_t ez80_emulator_usb_set_connected(ez80_emulator_t *emu,
                                               bool connected);
ez80_status_t ez80_emulator_usb_load_rx(ez80_emulator_t *emu,
                                         const uint8_t *data,
                                         uint8_t count);
ez80_status_t ez80_emulator_usb_get_status(const ez80_emulator_t *emu,
                                            uint8_t *out_status);

/* Flash sector erase */
ez80_status_t ez80_emulator_flash_erase_sector(ez80_emulator_t *emu,
                                                uint8_t sector);

/* I2C controller */
ez80_status_t ez80_emulator_i2c_configure(ez80_emulator_t *emu,
                                           uint8_t channel,
                                           uint8_t control,
                                           uint8_t clock_control);
ez80_status_t ez80_emulator_i2c_start(ez80_emulator_t *emu,
                                       uint8_t channel,
                                       uint8_t slave_addr,
                                       bool read);
ez80_status_t ez80_emulator_i2c_write(ez80_emulator_t *emu,
                                       uint8_t channel,
                                       uint8_t data);
ez80_status_t ez80_emulator_i2c_read(ez80_emulator_t *emu,
                                      uint8_t channel,
                                      uint8_t *out_data,
                                      bool ack);
ez80_status_t ez80_emulator_i2c_stop(ez80_emulator_t *emu,
                                      uint8_t channel);
ez80_status_t ez80_emulator_i2c_load_rx(ez80_emulator_t *emu,
                                         uint8_t channel,
                                         const uint8_t *data,
                                         uint8_t count);
ez80_status_t ez80_emulator_i2c_get_status(const ez80_emulator_t *emu,
                                            uint8_t channel,
                                            uint8_t *out_status);

/* UART controller */
ez80_status_t ez80_emulator_uart_configure(ez80_emulator_t *emu,
                                            uint8_t channel,
                                            uint8_t lctl,
                                            uint16_t brg);
ez80_status_t ez80_emulator_uart_write(ez80_emulator_t *emu,
                                        uint8_t channel,
                                        uint8_t data);
ez80_status_t ez80_emulator_uart_read(ez80_emulator_t *emu,
                                       uint8_t channel,
                                       uint8_t *out_data);
ez80_status_t ez80_emulator_uart_load_rx(ez80_emulator_t *emu,
                                          uint8_t channel,
                                          const uint8_t *data,
                                          uint8_t count);
ez80_status_t ez80_emulator_uart_get_lsr(const ez80_emulator_t *emu,
                                          uint8_t channel,
                                          uint8_t *out_lsr);

/* Execution trace */
ez80_status_t ez80_emulator_trace_enable(ez80_emulator_t *emu, bool enable);
ez80_status_t ez80_emulator_trace_clear(ez80_emulator_t *emu);
ez80_status_t ez80_emulator_trace_get_entry(const ez80_emulator_t *emu,
                                             uint32_t index,
                                             ez80_trace_entry_t *out_entry);
ez80_status_t ez80_emulator_trace_get_count(const ez80_emulator_t *emu,
                                             uint32_t *out_count);

/* Memory snapshot/restore */
typedef struct {
    ez80_cpu_state_t cpu;
    uint8_t ram[EZ80_RAM_BYTES];
    uint8_t ports[EZ80_PORT_BYTES];
    uint8_t port_space[EZ80_PORT_SPACE_BYTES];
    uint32_t snapshot_id;
} ez80_snapshot_t;

ez80_status_t ez80_emulator_snapshot_save(const ez80_emulator_t *emu,
                                           ez80_snapshot_t *out_snap);
ez80_status_t ez80_emulator_snapshot_restore(ez80_emulator_t *emu,
                                              const ez80_snapshot_t *snap);

/* RTC alarm */
ez80_status_t ez80_emulator_rtc_alarm_set(ez80_emulator_t *emu,
                                           uint8_t hours,
                                           uint8_t minutes,
                                           uint8_t seconds);
ez80_status_t ez80_emulator_rtc_alarm_enable(ez80_emulator_t *emu, bool enable);
ez80_status_t ez80_emulator_rtc_alarm_get_status(const ez80_emulator_t *emu,
                                                  bool *out_fired);

/* Hardware watchdog timer */
ez80_status_t ez80_emulator_wdt_configure(ez80_emulator_t *emu,
                                           uint8_t control,
                                           uint16_t timeout);
ez80_status_t ez80_emulator_wdt_feed(ez80_emulator_t *emu);
ez80_status_t ez80_emulator_wdt_get_counter(const ez80_emulator_t *emu,
                                             uint16_t *out_counter);

/* Power management */
ez80_status_t ez80_emulator_pm_set_mode(ez80_emulator_t *emu, uint8_t mode);
ez80_status_t ez80_emulator_pm_get_mode(const ez80_emulator_t *emu,
                                         uint8_t *out_mode);

#endif /* EZ80_EMULATOR_H */
