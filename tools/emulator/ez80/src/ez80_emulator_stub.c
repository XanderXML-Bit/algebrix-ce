#include "../include/ez80_emulator.h"
#include "../include/ez80_decode.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t mask24(uint32_t value)
{
    return value & 0x00FFFFFFu;
}

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_u24(const uint8_t *bytes)
{
    return (uint32_t)bytes[0]
         | ((uint32_t)bytes[1] << 8)
         | ((uint32_t)bytes[2] << 16);
}

static bool rom_looks_like_adl_boot_vectors(const uint8_t *bytes, size_t size)
{
    static const uint8_t k_vector_prefix[5] = { 0xF3u, 0xEDu, 0x7Eu, 0x5Bu, 0xC3u };
    size_t vector_index;

    if (!bytes || size < 32u) {
        return false;
    }

    for (vector_index = 0u; vector_index < 4u; vector_index++) {
        size_t offset = vector_index * 8u;
        if (memcmp(&bytes[offset], k_vector_prefix, sizeof(k_vector_prefix)) != 0) {
            return false;
        }
    }

    return true;
}

/* Read a 16-bit or 24-bit immediate from instruction bytes depending on
 * ADL mode.  Returns the value masked to 24 bits. */
static uint32_t read_imm_word(const uint8_t *bytes, bool adl_mode)
{
    if (adl_mode) return mask24(read_u24(bytes));
    return (uint32_t)read_u16(bytes);
}

static bool cpu_effective_adl_mode(const ez80_cpu_state_t *cpu)
{
    if (!cpu) return false;
    if (cpu->adl_mode_override_active) {
        return cpu->adl_mode_override;
    }
    return cpu->adl_mode;
}

static bool cpu_effective_adl_encoding(const ez80_cpu_state_t *cpu)
{
    if (!cpu) return false;
    if (cpu->adl_encoding_override_active) {
        return cpu->adl_encoding_override;
    }
    return cpu->adl_encoding;
}

static bool cpu_effective_pair_adl(const ez80_cpu_state_t *cpu)
{
    if (!cpu) return false;
    if (cpu->adl_mode_override_active || cpu->adl_encoding_override_active) {
        return cpu_effective_adl_mode(cpu);
    }
    return cpu->adl_encoding;
}

static void cpu_clear_instruction_mode_override(ez80_cpu_state_t *cpu)
{
    if (!cpu) return;
    cpu->adl_mode_override_active = false;
    cpu->adl_encoding_override_active = false;
}

static void cpu_commit_instruction_modes(ez80_cpu_state_t *cpu)
{
    if (!cpu) return;
    if (cpu->adl_mode_override_active) {
        cpu->adl_mode = cpu->adl_mode_override;
    }
    if (cpu->adl_encoding_override_active) {
        cpu->adl_encoding = cpu->adl_encoding_override;
    }
}

static bool ez80_suffix_prefix_modes(uint8_t prefix,
                                     bool *out_adl_mode,
                                     bool *out_adl_encoding)
{
    bool adl_mode;
    bool adl_encoding;

    switch (prefix) {
    case 0x40u: /* .SIS */
        adl_mode = false;
        adl_encoding = false;
        break;

    case 0x49u: /* .LIS */
        adl_mode = true;
        adl_encoding = false;
        break;

    case 0x52u: /* .SIL */
        adl_mode = false;
        adl_encoding = true;
        break;

    case 0x5Bu: /* .LIL */
        adl_mode = true;
        adl_encoding = true;
        break;

    default:
        return false;
    }

    if (out_adl_mode) {
        *out_adl_mode = adl_mode;
    }
    if (out_adl_encoding) {
        *out_adl_encoding = adl_encoding;
    }
    return true;
}

static uint8_t reverse_bits8(uint8_t value)
{
    value = (uint8_t)(((value & 0xF0u) >> 4) | ((value & 0x0Fu) << 4));
    value = (uint8_t)(((value & 0xCCu) >> 2) | ((value & 0x33u) << 2));
    value = (uint8_t)(((value & 0xAAu) >> 1) | ((value & 0x55u) << 1));
    return value;
}

#define FLAG_S 0x80u
#define FLAG_Z 0x40u
#define FLAG_H 0x10u
#define FLAG_PV 0x04u
#define FLAG_N 0x02u
#define FLAG_C 0x01u

#define EZ80_FLASH_UNLOCK_PORT 0x28u
#define EZ80_FLASH_PROTECT_PORT 0x06u
#define EZ80_FLASH_SECTOR_INDEX_PORT 0x24u
#define EZ80_FLASH_SECTOR_CONTROL_PORT 0x25u
#define EZ80_FLASH_PROTECT_CONTROL_PORT 0x26u
#define EZ80_FLASH_UNLOCK_MASK 0x01u
#define EZ80_FLASH_UNLOCK_OS58_MASK 0x08u
#define EZ80_FLASH_CERT_WRITE_MASK 0x08u
#define EZ80_FLASH_BOOT_PROTECTED_LAST_PAGE 0x01u
#define EZ80_FLASH_CERT_PAGE 0x3Bu
#define EZ80_FLASH_CERT_BACKUP_PAGE 0x3Fu
#define EZ80_FLASH_SECTOR_CMD_PROTECT 0x01u
#define EZ80_FLASH_SECTOR_CMD_UNPROTECT 0x02u
#define EZ80_FLASH_PROTECT_MODE_LOCKDOWN_MASK 0x80u
#define EZ80_OS_VERSION_FLASH_OFFSET 0x3BF037u

#define EZ80_OS_RAM_POOL_OFFSET 0x20000u
#define EZ80_OS_RAM_POOL_BYTES 0x18000u
#define EZ80_OS_ARCHIVE_POOL_BASE 0x3C0000u
#define EZ80_OS_ARCHIVE_POOL_BYTES 0x040000u
#define EZ80_RTC_CYCLES_PER_SECOND 32768u
#define EZ80_USB_DRAIN_CYCLES_PER_BYTE 64u

#define EZ80_ARTIFICE_KEY_SEQ_LEN 4u
#define EZ80_ARTIFICE_KEY_0 0x41u /* 'A' */
#define EZ80_ARTIFICE_KEY_1 0x52u /* 'R' */
#define EZ80_ARTIFICE_KEY_2 0x54u /* 'T' */
#define EZ80_ARTIFICE_KEY_3 0x49u /* 'I' */

#define EZ80_DNGRADE_CYCLES_PRECHECK 16u
#define EZ80_DNGRADE_CYCLES_PORT_WRITE 24u
#define EZ80_DNGRADE_CYCLES_FLASH_WRITE 128u
#define EZ80_DNGRADE_CYCLES_VERIFY 32u

static uint8_t g_flash_image[EZ80_FLASH_TOTAL_BYTES];
static char g_home_screen[EZ80_HOME_ROWS][EZ80_HOME_COLS];
static uint8_t g_graph_buffer[EZ80_GRAPH_WIDTH * EZ80_GRAPH_HEIGHT];
static uint16_t g_lcd_framebuffer[EZ80_GRAPH_WIDTH * EZ80_GRAPH_HEIGHT];
static void hardware_raise_irq(ez80_emulator_t *emu, uint8_t source);
typedef struct {
    uint16_t vector;
    const char *name;
} ez80_bcall_descriptor_t;

static const ez80_bcall_descriptor_t g_bcall_dispatch_table[] = {
    { EZ80_BCALL_HOME_CLEAR, "HomeClear" },
    { EZ80_BCALL_HOME_PUTC, "HomePutC" },
    { EZ80_BCALL_GRAPH_CLEAR, "GraphClear" },
    { EZ80_BCALL_GRAPH_PLOT, "GraphPlot" },
    { EZ80_BCALL_GRAPH_RENDER_TO_LCD, "GraphRenderToLcd" },
    { EZ80_BCALL_VAR_PUT, "VarPut" },
    { EZ80_BCALL_VAR_GET, "VarGet" },
    { EZ80_BCALL_VAR_DELETE, "VarDelete" },
    { EZ80_BCALL_VAR_EXISTS, "VarExists" },
    { EZ80_BCALL_VAR_GET_INFO, "VarGetInfo" },
    { EZ80_BCALL_PROGRAM_ARCHIVE_TO_RAM, "ProgramArchiveToRam" },
    { EZ80_BCALL_PROGRAM_RETURN_TO_ARCHIVE, "ProgramReturnToArchive" },
    { EZ80_BCALL_PROGRAM_LAUNCH, "ProgramLaunch" },
    { EZ80_BCALL_PROGRAM_FINISH, "ProgramFinish" }
};

static void platform_recompute_state(ez80_emulator_t *emu);
static void platform_sync_mmio(ez80_emulator_t *emu);
static void os_set_defaults(ez80_emulator_t *emu);
static void hardware_set_defaults(ez80_emulator_t *emu);
static void hardware_sync_ports(ez80_emulator_t *emu);
static void hardware_sync_mmio(ez80_emulator_t *emu);
static void os_sync_mmio(ez80_emulator_t *emu);
static void platform_record_dngrade_attempt(ez80_emulator_t *emu,
                                            const ez80_dngrade_attempt_result_t *attempt);
static void port_write(ez80_emulator_t *emu, uint16_t port, uint8_t value);
static uint8_t port_read(ez80_emulator_t *emu, uint16_t port);
static void io_record_last_out(ez80_emulator_t *emu, uint16_t port, uint8_t value);
static void io_record_last_in(ez80_emulator_t *emu, uint16_t port, uint8_t value);
static void io_install_default_handlers(ez80_emulator_t *emu);
static void mmio_set32(ez80_emulator_t *emu, uint16_t offset, uint32_t value);
static void mmio_write_byte(ez80_emulator_t *emu, uint16_t offset, uint8_t value);
static void usb_set_connected_state(ez80_emulator_t *emu, bool connected);
static void gpio_refresh_port_state(ez80_emulator_t *emu, uint8_t port_index);
static void gpio_sync_external_inputs(ez80_emulator_t *emu);
static void os_execute_mmio_bcall(ez80_emulator_t *emu);
static ez80_status_t port_write_legacy_low(ez80_emulator_t *emu, uint8_t port, uint8_t value);
static ez80_status_t io_spi_handler_write(ez80_emulator_t *emu, uint16_t port,
                                          uint8_t value, void *user_data);
static ez80_status_t io_gpio_handler_write(ez80_emulator_t *emu, uint16_t port,
                                           uint8_t value, void *user_data);
static ez80_status_t io_i2c_handler_read(ez80_emulator_t *emu, uint16_t port,
                                          uint8_t *out_value, void *user_data);
static ez80_status_t io_i2c_handler_write(ez80_emulator_t *emu, uint16_t port,
                                           uint8_t value, void *user_data);
static ez80_status_t io_uart_handler_read(ez80_emulator_t *emu, uint16_t port,
                                           uint8_t *out_value, void *user_data);
static ez80_status_t io_uart_handler_write(ez80_emulator_t *emu, uint16_t port,
                                            uint8_t value, void *user_data);
static void notify_reti(ez80_emulator_t *emu);
static void notify_retn(ez80_emulator_t *emu);
static ez80_status_t stack_push_pc(ez80_emulator_t *emu, uint32_t value);
static void dma_execute_channel(ez80_emulator_t *emu, uint8_t ch);
static ez80_status_t spi_perform_transfer(ez80_emulator_t *emu,
                                          ez80_spi_channel_t *spi,
                                          uint8_t tx_byte,
                                          uint8_t *out_rx_byte,
                                          bool queue_rx);
static uint32_t os_crc32_bytes(const uint8_t *data, uint32_t size_bytes);
static void os_set_last_swap_failure(ez80_emulator_t *emu, uint8_t reason);
static void os_record_swap_crc(ez80_emulator_t *emu,
                               uint32_t archive_crc32,
                               uint32_t ram_crc32,
                               bool success);
static uint8_t flash_read_byte(const ez80_emulator_t *emu, uint32_t flash_addr);
static void os_set_active_program(ez80_emulator_t *emu, const ez80_os_var_slot_t *slot);
static void os_clear_active_program(ez80_emulator_t *emu);
static ez80_status_t os_program_launch_internal(ez80_emulator_t *emu,
                                                ez80_os_var_slot_t *slot,
                                                uint32_t *out_ram_addr);
static ez80_status_t os_program_finish_internal(ez80_emulator_t *emu,
                                                ez80_os_var_slot_t *slot,
                                                uint32_t *out_archive_addr);
static uint8_t flash_sector_index(uint32_t flash_addr);
static bool flash_sector_os_locked(const ez80_emulator_t *emu, uint8_t sector);
static void flash_sync_protection_mmio(ez80_emulator_t *emu);
static void flash_reset_protection(ez80_emulator_t *emu);
static void flash_record_writeflash(ez80_emulator_t *emu,
                                    uint8_t status,
                                    uint32_t addr,
                                    uint8_t requested,
                                    uint8_t before,
                                    uint8_t after);
static bool platform_matches_boot_irq_halt_sequence(const ez80_emulator_t *emu, uint8_t value);
static void boot_irq_overlay_arm(ez80_emulator_t *emu);
static void boot_irq_overlay_clear(ez80_emulator_t *emu);
static bool boot_irq_overlay_read(const ez80_emulator_t *emu,
                                  uint16_t port,
                                  uint8_t *out_value);
static bool boot_irq_overlay_write(ez80_emulator_t *emu,
                                   uint16_t port,
                                   uint8_t value);
static bool boot_irq_overlay_release_halt(ez80_emulator_t *emu);
static uint8_t pm_wake_source_for_irq_source(uint8_t irq_source);
static bool pm_mode_allows_timer_advance(const ez80_emulator_t *emu);
static bool pm_mode_allows_usb_advance(const ez80_emulator_t *emu);
static bool pm_mode_allows_wake_source(uint8_t mode, uint8_t wake_source);
static void pm_note_sleep_cycles(ez80_emulator_t *emu, uint32_t cycles);
static void pm_note_wake_event(ez80_emulator_t *emu, uint8_t wake_source, bool resume_cpu);
static void pm_apply_mode_state(ez80_emulator_t *emu, uint8_t mode);
static void rtc_alarm_write_component(ez80_emulator_t *emu,
                                      uint8_t *field,
                                      uint8_t value,
                                      uint8_t modulo);

static bool parity_even8(uint8_t value)
{
    value ^= (uint8_t)(value >> 4);
    value ^= (uint8_t)(value >> 2);
    value ^= (uint8_t)(value >> 1);
    return ((value & 0x01u) == 0u);
}

static bool addr_in_range(uint32_t addr, uint32_t base, uint32_t size)
{
    if (addr < base) return false;
    return (addr - base) < size;
}

static bool lcd_addr_in_range(uint32_t addr)
{
    return addr_in_range(mask24(addr), EZ80_LCD_RAM_BASE, EZ80_LCD_RAM_BYTES);
}

static bool keypad_addr_in_range(uint32_t addr)
{
    return addr_in_range(mask24(addr), EZ80_KEYPAD_MMIO_BASE, EZ80_KEYPAD_MMIO_BYTES);
}

static bool system_mmio_addr_in_range(uint32_t addr)
{
    return addr_in_range(mask24(addr), EZ80_SYSTEM_MMIO_BASE, EZ80_SYSTEM_MMIO_BYTES);
}

static uint8_t lcd_read_byte(uint32_t addr)
{
    uint32_t offset = mask24(addr) - EZ80_LCD_RAM_BASE;
    uint32_t pixel = offset >> 1;
    uint16_t color;
    if (pixel >= (uint32_t)(EZ80_GRAPH_WIDTH * EZ80_GRAPH_HEIGHT)) {
        return 0u;
    }
    color = g_lcd_framebuffer[pixel];
    if ((offset & 0x01u) != 0u) {
        return (uint8_t)((color >> 8) & 0xFFu);
    }
    return (uint8_t)(color & 0xFFu);
}

static void lcd_write_byte(ez80_emulator_t *emu, uint32_t addr, uint8_t value)
{
    uint32_t offset;
    uint32_t pixel;
    uint16_t color;
    if (!emu) return;
    offset = mask24(addr) - EZ80_LCD_RAM_BASE;
    pixel = offset >> 1;
    if (pixel >= (uint32_t)(EZ80_GRAPH_WIDTH * EZ80_GRAPH_HEIGHT)) {
        return;
    }
    color = g_lcd_framebuffer[pixel];
    if ((offset & 0x01u) != 0u) {
        color = (uint16_t)((color & 0x00FFu) | ((uint16_t)value << 8));
    } else {
        color = (uint16_t)((color & 0xFF00u) | (uint16_t)value);
    }
    g_lcd_framebuffer[pixel] = color;
    emu->hw.lcd_status |= EZ80_LCD_STATUS_DATA_READY;
    emu->memory.mmio[EZ80_MMIO_REG_LCD_STATUS] = emu->hw.lcd_status;
}

static void keypad_regs_sync(ez80_emulator_t *emu)
{
    uint8_t row;
    if (!emu) return;

    for (row = 0u; row < 8u; row++) {
        uint16_t row_word = (uint16_t)(0x0300u | (uint16_t)emu->hw.keypad_rows[row]);
        uint8_t offset = (uint8_t)(EZ80_KEYPAD_REG_ROW0_LO + row * EZ80_KEYPAD_REG_ROW_STRIDE);
        emu->hw.keypad_regs[offset] = (uint8_t)(row_word & 0xFFu);
        emu->hw.keypad_regs[offset + 1u] = (uint8_t)((row_word >> 8) & 0xFFu);
    }

    if ((emu->hw.irq_pending & EZ80_IRQ_SOURCE_KEYPAD) != 0u) {
        emu->hw.keypad_regs[EZ80_KEYPAD_REG_IRQ_STATUS] |= 0x01u;
    } else {
        emu->hw.keypad_regs[EZ80_KEYPAD_REG_IRQ_STATUS] &= (uint8_t)~0x01u;
    }
    if ((emu->hw.irq_mask & EZ80_IRQ_SOURCE_KEYPAD) != 0u) {
        emu->hw.keypad_regs[EZ80_KEYPAD_REG_IRQ_MASK] |= 0x01u;
    } else {
        emu->hw.keypad_regs[EZ80_KEYPAD_REG_IRQ_MASK] &= (uint8_t)~0x01u;
    }
}

static uint8_t keypad_read_byte(const ez80_emulator_t *emu, uint32_t addr)
{
    uint32_t offset = mask24(addr) - EZ80_KEYPAD_MMIO_BASE;
    if (!emu || offset >= EZ80_KEYPAD_MMIO_BYTES) return 0u;

    if (offset >= EZ80_KEYPAD_REG_ROW0_LO &&
        offset < (EZ80_KEYPAD_REG_ROW0_LO + 16u)) {
        uint8_t row = (uint8_t)((offset - EZ80_KEYPAD_REG_ROW0_LO) / EZ80_KEYPAD_REG_ROW_STRIDE);
        uint16_t row_word;
        if (row < 8u) {
            row_word = (uint16_t)(0x0300u | (uint16_t)emu->hw.keypad_rows[row]);
            if (((offset - EZ80_KEYPAD_REG_ROW0_LO) % EZ80_KEYPAD_REG_ROW_STRIDE) == 0u) {
                return (uint8_t)(row_word & 0xFFu);
            }
            return (uint8_t)((row_word >> 8) & 0xFFu);
        }
    }

    if (offset == EZ80_KEYPAD_REG_IRQ_STATUS) {
        return (emu->hw.irq_pending & EZ80_IRQ_SOURCE_KEYPAD) ? 0x01u : 0x00u;
    }
    if (offset == EZ80_KEYPAD_REG_IRQ_MASK) {
        return (emu->hw.irq_mask & EZ80_IRQ_SOURCE_KEYPAD) ? 0x01u : 0x00u;
    }

    return emu->hw.keypad_regs[offset];
}

static void keypad_write_byte(ez80_emulator_t *emu, uint32_t addr, uint8_t value)
{
    uint32_t offset;
    if (!emu) return;
    offset = mask24(addr) - EZ80_KEYPAD_MMIO_BASE;
    if (offset >= EZ80_KEYPAD_MMIO_BYTES) return;

    switch (offset) {
    case EZ80_KEYPAD_REG_IRQ_STATUS:
        if ((value & 0x01u) != 0u) {
            emu->hw.irq_pending &= (uint8_t)~EZ80_IRQ_SOURCE_KEYPAD;
        }
        break;

    case EZ80_KEYPAD_REG_IRQ_MASK:
        if ((value & 0x01u) != 0u) {
            emu->hw.irq_mask |= EZ80_IRQ_SOURCE_KEYPAD;
        } else {
            emu->hw.irq_mask &= (uint8_t)~EZ80_IRQ_SOURCE_KEYPAD;
        }
        break;

    default:
        break;
    }

    if (offset >= EZ80_KEYPAD_REG_ROW0_LO &&
        offset < (EZ80_KEYPAD_REG_ROW0_LO + 16u) &&
        ((offset - EZ80_KEYPAD_REG_ROW0_LO) % EZ80_KEYPAD_REG_ROW_STRIDE) == 0u) {
        uint8_t row = (uint8_t)((offset - EZ80_KEYPAD_REG_ROW0_LO) / EZ80_KEYPAD_REG_ROW_STRIDE);
        if (row < 8u) {
            emu->hw.keypad_rows[row] = value;
        }
    }

    emu->hw.keypad_regs[offset] = value;
    keypad_regs_sync(emu);
}

static bool memory_addr_readable(uint32_t addr)
{
    addr = mask24(addr);
    return addr_in_range(addr, EZ80_FLASH_BASE, EZ80_FLASH_TOTAL_BYTES) ||
           addr_in_range(addr, EZ80_RAM_BASE, EZ80_RAM_BYTES) ||
           addr_in_range(addr, EZ80_LCD_RAM_BASE, EZ80_LCD_RAM_BYTES) ||
           addr_in_range(addr, EZ80_SYSTEM_MMIO_BASE, EZ80_SYSTEM_MMIO_BYTES) ||
           addr_in_range(addr, EZ80_KEYPAD_MMIO_BASE, EZ80_KEYPAD_MMIO_BYTES) ||
           addr_in_range(addr, EZ80_PORT_BASE, EZ80_PORT_SPACE_BYTES) ||
           addr_in_range(addr, EZ80_MMIO_BASE, EZ80_MMIO_BYTES);
}

static bool memory_addr_writable(uint32_t addr)
{
    addr = mask24(addr);
    return addr_in_range(addr, EZ80_RAM_BASE, EZ80_RAM_BYTES) ||
           addr_in_range(addr, EZ80_LCD_RAM_BASE, EZ80_LCD_RAM_BYTES) ||
           addr_in_range(addr, EZ80_SYSTEM_MMIO_BASE, EZ80_SYSTEM_MMIO_BYTES) ||
           addr_in_range(addr, EZ80_KEYPAD_MMIO_BASE, EZ80_KEYPAD_MMIO_BYTES) ||
           addr_in_range(addr, EZ80_PORT_BASE, EZ80_PORT_SPACE_BYTES) ||
           addr_in_range(addr, EZ80_MMIO_BASE, EZ80_MMIO_BYTES);
}

static uint32_t z80_mbase_addr(uint8_t mbase, uint16_t addr16)
{
    return mask24(((uint32_t)mbase << 16) | (uint32_t)addr16);
}

static uint32_t map_data_addr_cpu(const ez80_cpu_state_t *cpu, uint32_t raw_addr)
{
    uint32_t addr = mask24(raw_addr);
    if ((addr & 0x00FF0000u) == 0u) {
        if (cpu && !cpu_effective_adl_mode(cpu)) {
            return z80_mbase_addr(cpu->mbase_reg, (uint16_t)addr);
        }
        return mask24(EZ80_RAM_BASE + (addr & 0xFFFFu));
    }
    return addr;
}

static uint32_t map_data_addr16_offset_cpu(const ez80_cpu_state_t *cpu, uint16_t addr16, uint16_t offset)
{
    uint16_t wrapped = (uint16_t)(addr16 + offset);
    if (cpu && !cpu_effective_adl_mode(cpu)) {
        return z80_mbase_addr(cpu->mbase_reg, wrapped);
    }
    return mask24(EZ80_RAM_BASE + (uint32_t)wrapped);
}

static uint32_t map_code_addr16_cpu(const ez80_cpu_state_t *cpu, uint32_t pc, uint16_t imm16)
{
    if (cpu && !cpu_effective_adl_mode(cpu)) {
        return z80_mbase_addr(cpu->mbase_reg, imm16);
    }
    return mask24((pc & 0x00FF0000u) | (uint32_t)imm16);
}

static uint32_t map_multibyte_ptr_offset_cpu(const ez80_cpu_state_t *cpu,
                                             uint32_t raw_addr,
                                             uint16_t offset,
                                             bool multibyte_adl)
{
    if (multibyte_adl) {
        return mask24(map_data_addr_cpu(cpu, raw_addr) + (uint32_t)offset);
    }
    return map_data_addr16_offset_cpu(cpu, (uint16_t)raw_addr, offset);
}

static uint16_t port_addr_from_bc(const ez80_cpu_state_t *cpu)
{
    if (!cpu) return 0u;
    return (uint16_t)(cpu->bc & 0xFFFFu);
}

static uint16_t port_addr_from_de(const ez80_cpu_state_t *cpu)
{
    if (!cpu) return 0u;
    return (uint16_t)(cpu->de & 0xFFFFu);
}

static uint16_t port_addr_from_b_c(uint8_t b, uint8_t c)
{
    return (uint16_t)(((uint16_t)b << 8) | (uint16_t)c);
}

static void port_shadow_write(ez80_emulator_t *emu, uint16_t port, uint8_t value)
{
    if (!emu) return;
    emu->memory.port_space[port] = value;
    if (port < EZ80_PORT_BYTES) {
        emu->memory.ports[port] = value;
    }
}

static uint8_t port_shadow_read(const ez80_emulator_t *emu, uint16_t port)
{
    if (!emu) return 0u;
    if (port < EZ80_PORT_BYTES) {
        return emu->memory.ports[port];
    }
    return emu->memory.port_space[port];
}

static void cpu_reset_runtime_state(ez80_cpu_state_t *cpu)
{
    if (!cpu) return;
    memset(cpu, 0, sizeof(*cpu));
    cpu->adl_mode = true;
    cpu->adl_encoding = false;  /* default: Z80-compat 16-bit immediates */
    cpu->interrupt_mode = 0u;
    cpu->iff1 = false;
    cpu->iff2 = false;
    cpu->sp = mask24(EZ80_RAM_BASE + EZ80_RAM_BYTES - 1u);
}

static void memory_clear_runtime_state(ez80_emulator_t *emu)
{
    if (!emu) return;
    memset(emu->memory.ram, 0, sizeof(emu->memory.ram));
    memset(emu->memory.ports, 0, sizeof(emu->memory.ports));
    memset(emu->memory.port_space, 0, sizeof(emu->memory.port_space));
    memset(emu->memory.mmio, 0, sizeof(emu->memory.mmio));
    emu->memory.mmio[EZ80_MMIO_REG_FLASH_UNLOCK_STATE] = 0u;
    emu->memory.mmio[EZ80_MMIO_REG_FLASH_CERT_WRITE_STATE] = 0u;
    os_set_defaults(emu);
    hardware_set_defaults(emu);
    platform_recompute_state(emu);
    os_sync_mmio(emu);
    hardware_sync_mmio(emu);
}

static void mmio_set(ez80_emulator_t *emu, uint16_t offset, uint8_t value)
{
    if (!emu) return;
    if (offset < EZ80_MMIO_BYTES) {
        emu->memory.mmio[offset] = value;
    }
}

static uint32_t mmio_get24(const ez80_emulator_t *emu, uint16_t offset)
{
    uint32_t b0;
    uint32_t b1;
    uint32_t b2;
    if (!emu || offset + 2u >= EZ80_MMIO_BYTES) return 0u;
    b0 = emu->memory.mmio[offset + 0u];
    b1 = emu->memory.mmio[offset + 1u];
    b2 = emu->memory.mmio[offset + 2u];
    return mask24(b0 | (b1 << 8) | (b2 << 16));
}

static void mmio_set24(ez80_emulator_t *emu, uint16_t offset, uint32_t value)
{
    if (!emu || offset + 2u >= EZ80_MMIO_BYTES) return;
    value = mask24(value);
    emu->memory.mmio[offset + 0u] = (uint8_t)(value & 0xFFu);
    emu->memory.mmio[offset + 1u] = (uint8_t)((value >> 8) & 0xFFu);
    emu->memory.mmio[offset + 2u] = (uint8_t)((value >> 16) & 0xFFu);
}

static void mmio_set32(ez80_emulator_t *emu, uint16_t offset, uint32_t value)
{
    if (!emu || offset + 3u >= EZ80_MMIO_BYTES) return;
    emu->memory.mmio[offset + 0u] = (uint8_t)(value & 0xFFu);
    emu->memory.mmio[offset + 1u] = (uint8_t)((value >> 8) & 0xFFu);
    emu->memory.mmio[offset + 2u] = (uint8_t)((value >> 16) & 0xFFu);
    emu->memory.mmio[offset + 3u] = (uint8_t)((value >> 24) & 0xFFu);
}

static bool mmio_try_write_dma(ez80_emulator_t *emu, uint16_t offset, uint8_t value)
{
    static const uint16_t dma_base[EZ80_DMA_CHANNEL_COUNT] = {
        EZ80_MMIO_REG_DMA0_SRC_0,
        EZ80_MMIO_REG_DMA1_SRC_0,
        EZ80_MMIO_REG_DMA2_SRC_0
    };
    uint8_t ch;

    if (!emu) return false;

    for (ch = 0u; ch < EZ80_DMA_CHANNEL_COUNT; ch++) {
        uint16_t base = dma_base[ch];
        ez80_dma_channel_t *dma;

        if (offset < base || offset > (uint16_t)(base + 10u)) {
            continue;
        }

        emu->memory.mmio[offset] = value;
        dma = &emu->hw.dma[ch];

        if (offset == (uint16_t)(base + 10u)) {
            hardware_sync_mmio(emu);
            return true;
        }

        dma->src_addr = mmio_get24(emu, base);
        dma->dst_addr = mmio_get24(emu, (uint16_t)(base + 3u));
        dma->length = mmio_get24(emu, (uint16_t)(base + 6u));

        if (offset == (uint16_t)(base + 9u)) {
            dma->control = emu->memory.mmio[base + 9u];
            if ((dma->control & EZ80_DMA_CONTROL_ENABLE) == 0u) {
                dma->status = EZ80_DMA_STATUS_IDLE;
                dma->bytes_transferred = 0u;
            }
            if ((dma->control & EZ80_DMA_CONTROL_START) != 0u &&
                (dma->control & EZ80_DMA_CONTROL_ENABLE) != 0u) {
                dma->status = EZ80_DMA_STATUS_IDLE;
                dma->bytes_transferred = 0u;
                dma_execute_channel(emu, ch);
            }
        } else {
            dma->status = EZ80_DMA_STATUS_IDLE;
            dma->bytes_transferred = 0u;
        }

        hardware_sync_mmio(emu);
        return true;
    }

    return false;
}

static bool mmio_try_write_spi(ez80_emulator_t *emu, uint16_t offset, uint8_t value)
{
    uint16_t port;

    if (!emu) return false;

    switch (offset) {
    case EZ80_MMIO_REG_SPI0_CONTROL: port = EZ80_PORT_SPI0_CONTROL; break;
    case EZ80_MMIO_REG_SPI0_BAUD:    port = EZ80_PORT_SPI0_BAUD; break;
    case EZ80_MMIO_REG_SPI1_CONTROL: port = EZ80_PORT_SPI1_CONTROL; break;
    case EZ80_MMIO_REG_SPI1_BAUD:    port = EZ80_PORT_SPI1_BAUD; break;
    default:
        return false;
    }

    (void)io_spi_handler_write(emu, port, value, NULL);
    hardware_sync_mmio(emu);
    return true;
}

static bool mmio_try_write_gpio(ez80_emulator_t *emu, uint16_t offset, uint8_t value)
{
    uint16_t port;

    if (!emu) return false;

    switch (offset) {
    case EZ80_MMIO_REG_GPIO_A_DATA: port = EZ80_PORT_GPIO_A_DATA; break;
    case EZ80_MMIO_REG_GPIO_A_DIR:  port = EZ80_PORT_GPIO_A_DIR; break;
    case EZ80_MMIO_REG_GPIO_B_DATA: port = EZ80_PORT_GPIO_B_DATA; break;
    case EZ80_MMIO_REG_GPIO_B_DIR:  port = EZ80_PORT_GPIO_B_DIR; break;
    case EZ80_MMIO_REG_GPIO_C_DATA: port = EZ80_PORT_GPIO_C_DATA; break;
    case EZ80_MMIO_REG_GPIO_C_DIR:  port = EZ80_PORT_GPIO_C_DIR; break;
    case EZ80_MMIO_REG_GPIO_D_DATA: port = EZ80_PORT_GPIO_D_DATA; break;
    case EZ80_MMIO_REG_GPIO_D_DIR:  port = EZ80_PORT_GPIO_D_DIR; break;
    case EZ80_MMIO_REG_GPIO_E_DATA: port = EZ80_PORT_GPIO_E_DATA; break;
    case EZ80_MMIO_REG_GPIO_E_DIR:  port = EZ80_PORT_GPIO_E_DIR; break;
    case EZ80_MMIO_REG_GPIO_F_DATA: port = EZ80_PORT_GPIO_F_DATA; break;
    case EZ80_MMIO_REG_GPIO_F_DIR:  port = EZ80_PORT_GPIO_F_DIR; break;
    default:
        return false;
    }

    (void)io_gpio_handler_write(emu, port, value, NULL);
    hardware_sync_mmio(emu);
    return true;
}

static bool mmio_try_write_i2c(ez80_emulator_t *emu, uint16_t offset, uint8_t value)
{
    uint16_t port;

    if (!emu) return false;

    switch (offset) {
    case EZ80_MMIO_REG_I2C0_SAR: port = EZ80_PORT_I2C0_SAR; break;
    case EZ80_MMIO_REG_I2C0_XSAR: port = EZ80_PORT_I2C0_XSAR; break;
    case EZ80_MMIO_REG_I2C0_DATA: port = EZ80_PORT_I2C0_DR; break;
    case EZ80_MMIO_REG_I2C0_CTL: port = EZ80_PORT_I2C0_CTL; break;
    case EZ80_MMIO_REG_I2C0_CCR: port = EZ80_PORT_I2C0_CCR; break;
    case EZ80_MMIO_REG_I2C1_SAR: port = EZ80_PORT_I2C1_SAR; break;
    case EZ80_MMIO_REG_I2C1_XSAR: port = EZ80_PORT_I2C1_XSAR; break;
    case EZ80_MMIO_REG_I2C1_DATA: port = EZ80_PORT_I2C1_DR; break;
    case EZ80_MMIO_REG_I2C1_CTL: port = EZ80_PORT_I2C1_CTL; break;
    case EZ80_MMIO_REG_I2C1_CCR: port = EZ80_PORT_I2C1_CCR; break;
    default:
        return false;
    }

    (void)io_i2c_handler_write(emu, port, value, NULL);
    hardware_sync_mmio(emu);
    return true;
}

static bool mmio_try_write_uart(ez80_emulator_t *emu, uint16_t offset, uint8_t value)
{
    ez80_uart_channel_t *uart;
    uint8_t channel;
    uint16_t port = 0u;

    if (!emu) return false;

    switch (offset) {
    case EZ80_MMIO_REG_UART0_DATA:
    case EZ80_MMIO_REG_UART0_FCTL:
    case EZ80_MMIO_REG_UART0_MCR:
    case EZ80_MMIO_REG_UART0_SPR:
    case EZ80_MMIO_REG_UART0_LCTL:
    case EZ80_MMIO_REG_UART0_IER:
    case EZ80_MMIO_REG_UART0_BRG_LO:
    case EZ80_MMIO_REG_UART0_BRG_HI:
        channel = 0u;
        break;
    case EZ80_MMIO_REG_UART1_DATA:
    case EZ80_MMIO_REG_UART1_FCTL:
    case EZ80_MMIO_REG_UART1_MCR:
    case EZ80_MMIO_REG_UART1_SPR:
    case EZ80_MMIO_REG_UART1_LCTL:
    case EZ80_MMIO_REG_UART1_IER:
    case EZ80_MMIO_REG_UART1_BRG_LO:
    case EZ80_MMIO_REG_UART1_BRG_HI:
        channel = 1u;
        break;
    default:
        return false;
    }

    uart = &emu->hw.uart[channel];

    switch (offset) {
    case EZ80_MMIO_REG_UART0_DATA:
        port = EZ80_PORT_UART0_THR;
        break;
    case EZ80_MMIO_REG_UART1_DATA:
        port = EZ80_PORT_UART1_THR;
        break;
    case EZ80_MMIO_REG_UART0_FCTL:
        port = EZ80_PORT_UART0_FCTL;
        break;
    case EZ80_MMIO_REG_UART1_FCTL:
        port = EZ80_PORT_UART1_FCTL;
        break;
    case EZ80_MMIO_REG_UART0_MCR:
        port = EZ80_PORT_UART0_MCR;
        break;
    case EZ80_MMIO_REG_UART1_MCR:
        port = EZ80_PORT_UART1_MCR;
        break;
    case EZ80_MMIO_REG_UART0_SPR:
        port = EZ80_PORT_UART0_SPR;
        break;
    case EZ80_MMIO_REG_UART1_SPR:
        port = EZ80_PORT_UART1_SPR;
        break;
    case EZ80_MMIO_REG_UART0_LCTL:
    case EZ80_MMIO_REG_UART1_LCTL:
        uart->lctl = value;
        break;
    case EZ80_MMIO_REG_UART0_IER:
    case EZ80_MMIO_REG_UART1_IER:
        uart->ier = value;
        break;
    case EZ80_MMIO_REG_UART0_BRG_LO:
    case EZ80_MMIO_REG_UART1_BRG_LO:
        uart->brg = (uint16_t)((uart->brg & 0xFF00u) | (uint16_t)value);
        if (uart->brg == 0u) uart->brg = 1u;
        break;
    case EZ80_MMIO_REG_UART0_BRG_HI:
    case EZ80_MMIO_REG_UART1_BRG_HI:
        uart->brg = (uint16_t)((uart->brg & 0x00FFu) | ((uint16_t)value << 8));
        if (uart->brg == 0u) uart->brg = 1u;
        break;
    default:
        return false;
    }

    if (port != 0u) {
        (void)io_uart_handler_write(emu, port, value, NULL);
    }

    hardware_sync_mmio(emu);
    return true;
}

static bool mmio_try_read_i2c(ez80_emulator_t *emu, uint16_t offset, uint8_t *out)
{
    uint16_t port;
    uint8_t value;

    if (!emu || !out) return false;

    switch (offset) {
    case EZ80_MMIO_REG_I2C0_SAR: port = EZ80_PORT_I2C0_SAR; break;
    case EZ80_MMIO_REG_I2C0_XSAR: port = EZ80_PORT_I2C0_XSAR; break;
    case EZ80_MMIO_REG_I2C0_DATA: port = EZ80_PORT_I2C0_DR; break;
    case EZ80_MMIO_REG_I2C0_CTL: port = EZ80_PORT_I2C0_CTL; break;
    case EZ80_MMIO_REG_I2C0_SR: port = EZ80_PORT_I2C0_SR; break;
    case EZ80_MMIO_REG_I2C0_CCR: port = EZ80_PORT_I2C0_CCR; break;
    case EZ80_MMIO_REG_I2C1_SAR: port = EZ80_PORT_I2C1_SAR; break;
    case EZ80_MMIO_REG_I2C1_XSAR: port = EZ80_PORT_I2C1_XSAR; break;
    case EZ80_MMIO_REG_I2C1_DATA: port = EZ80_PORT_I2C1_DR; break;
    case EZ80_MMIO_REG_I2C1_CTL: port = EZ80_PORT_I2C1_CTL; break;
    case EZ80_MMIO_REG_I2C1_SR: port = EZ80_PORT_I2C1_SR; break;
    case EZ80_MMIO_REG_I2C1_CCR: port = EZ80_PORT_I2C1_CCR; break;
    default:
        return false;
    }

    value = port_read(emu, port);
    hardware_sync_mmio(emu);
    *out = value;
    return true;
}

static bool mmio_try_read_uart(ez80_emulator_t *emu, uint16_t offset, uint8_t *out)
{
    uint16_t port;
    uint8_t value;

    if (!emu || !out) return false;

    switch (offset) {
    case EZ80_MMIO_REG_UART0_DATA: port = EZ80_PORT_UART0_RBR; break;
    case EZ80_MMIO_REG_UART0_LSR: port = EZ80_PORT_UART0_LSR; break;
    case EZ80_MMIO_REG_UART0_IER: port = EZ80_PORT_UART0_IER; break;
    case EZ80_MMIO_REG_UART0_LCTL: port = EZ80_PORT_UART0_LCTL; break;
    case EZ80_MMIO_REG_UART0_MCR: port = EZ80_PORT_UART0_MCR; break;
    case EZ80_MMIO_REG_UART0_SPR: port = EZ80_PORT_UART0_SPR; break;
    case EZ80_MMIO_REG_UART1_DATA: port = EZ80_PORT_UART1_RBR; break;
    case EZ80_MMIO_REG_UART1_LSR: port = EZ80_PORT_UART1_LSR; break;
    case EZ80_MMIO_REG_UART1_IER: port = EZ80_PORT_UART1_IER; break;
    case EZ80_MMIO_REG_UART1_LCTL: port = EZ80_PORT_UART1_LCTL; break;
    case EZ80_MMIO_REG_UART1_MCR: port = EZ80_PORT_UART1_MCR; break;
    case EZ80_MMIO_REG_UART1_SPR: port = EZ80_PORT_UART1_SPR; break;
    default:
        return false;
    }

    value = port_read(emu, port);
    hardware_sync_mmio(emu);
    *out = value;
    return true;
}

static void mmio_write_byte(ez80_emulator_t *emu, uint16_t offset, uint8_t value)
{
    if (!emu || offset >= EZ80_MMIO_BYTES) return;

    if (mmio_try_write_dma(emu, offset, value)) return;
    if (mmio_try_write_spi(emu, offset, value)) return;
    if (mmio_try_write_gpio(emu, offset, value)) return;
    if (mmio_try_write_i2c(emu, offset, value)) return;
    if (mmio_try_write_uart(emu, offset, value)) return;

    emu->memory.mmio[offset] = value;

    switch (offset) {
    case EZ80_MMIO_REG_BCALL_EXECUTE:
        if (value != 0u) {
            os_execute_mmio_bcall(emu);
        }
        return;

    case EZ80_MMIO_REG_HW_KEYPAD_ROW_MASK:
        (void)port_write_legacy_low(emu, EZ80_PORT_KEYPAD_ROW_SELECT, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_LCD_COMMAND:
        (void)port_write_legacy_low(emu, EZ80_PORT_LCD_COMMAND, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_LCD_DATA:
        (void)port_write_legacy_low(emu, EZ80_PORT_LCD_DATA, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_LCD_WRITE_INDEX_LO:
        emu->hw.lcd_write_index = (emu->hw.lcd_write_index & 0xFF00u) | (uint32_t)value;
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_LCD_WRITE_INDEX_HI:
        emu->hw.lcd_write_index = (emu->hw.lcd_write_index & 0x00FFu) | ((uint32_t)value << 8);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_USB_CONTROL:
        (void)port_write_legacy_low(emu, EZ80_PORT_USB_CONTROL, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_USB_CONNECTED:
        usb_set_connected_state(emu, value != 0u);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_TIMER0_LO:
        (void)port_write_legacy_low(emu, EZ80_PORT_TIMER0_COUNTER_LO, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_TIMER0_HI:
        (void)port_write_legacy_low(emu, EZ80_PORT_TIMER0_COUNTER_HI, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_TIMER1_LO:
        (void)port_write_legacy_low(emu, EZ80_PORT_TIMER1_COUNTER_LO, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_TIMER1_HI:
        (void)port_write_legacy_low(emu, EZ80_PORT_TIMER1_COUNTER_HI, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_RTC_SECONDS:
        (void)port_write_legacy_low(emu, EZ80_PORT_RTC_SECONDS, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_RTC_MINUTES:
        (void)port_write_legacy_low(emu, EZ80_PORT_RTC_MINUTES, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_RTC_HOURS:
        (void)port_write_legacy_low(emu, EZ80_PORT_RTC_HOURS, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_IRQ_MASK:
        (void)port_write_legacy_low(emu, EZ80_PORT_IRQ_MASK, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_HW_IRQ_PENDING:
        (void)port_write_legacy_low(emu, EZ80_PORT_IRQ_PENDING, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_TIMER0_PRESCALER:
        (void)port_write_legacy_low(emu, EZ80_PORT_TIMER0_PRESCALER, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_TIMER1_PRESCALER:
        (void)port_write_legacy_low(emu, EZ80_PORT_TIMER1_PRESCALER, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_TIMER0_RELOAD_LO:
        (void)port_write_legacy_low(emu, EZ80_PORT_TIMER0_RELOAD_LO, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_TIMER0_RELOAD_HI:
        (void)port_write_legacy_low(emu, EZ80_PORT_TIMER0_RELOAD_HI, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_TIMER1_RELOAD_LO:
        (void)port_write_legacy_low(emu, EZ80_PORT_TIMER1_RELOAD_LO, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_TIMER1_RELOAD_HI:
        (void)port_write_legacy_low(emu, EZ80_PORT_TIMER1_RELOAD_HI, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_BACKLIGHT_STATE:
        (void)ez80_emulator_set_backlight(emu, value != 0u);
        return;

    case EZ80_MMIO_REG_BATTERY_LEVEL:
        if (value <= 100u) {
            (void)ez80_emulator_set_battery_level(emu, value);
        } else {
            hardware_sync_mmio(emu);
        }
        return;

    case EZ80_MMIO_REG_POWER_STATE:
        emu->hw.power_state = value;
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_TRACE_ENABLED:
        (void)ez80_emulator_trace_enable(emu, value != 0u);
        return;

    case EZ80_MMIO_REG_RTC_ALARM_ENABLED:
        (void)ez80_emulator_rtc_alarm_enable(emu, value != 0u);
        return;

    case EZ80_MMIO_REG_RTC_ALARM_SECONDS:
        rtc_alarm_write_component(emu, &emu->hw.rtc_alarm_seconds, value, 60u);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_RTC_ALARM_MINUTES:
        rtc_alarm_write_component(emu, &emu->hw.rtc_alarm_minutes, value, 60u);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_RTC_ALARM_HOURS:
        rtc_alarm_write_component(emu, &emu->hw.rtc_alarm_hours, value, 24u);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_RTC_ALARM_FIRED:
        if (value == 0u) {
            emu->hw.rtc_alarm_fired = false;
        }
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_WDT_CONTROL:
        if (emu->hw.wdt_timeout == 0u) {
            emu->hw.wdt_timeout = EZ80_WDT_DEFAULT_TIMEOUT;
        }
        (void)ez80_emulator_wdt_configure(emu, value, emu->hw.wdt_timeout);
        return;

    case EZ80_MMIO_REG_WDT_TIMEOUT_LO:
        emu->hw.wdt_timeout = (emu->hw.wdt_timeout & 0xFF00u) | (uint16_t)value;
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_WDT_TIMEOUT_HI:
        emu->hw.wdt_timeout = (emu->hw.wdt_timeout & 0x00FFu) | ((uint16_t)value << 8);
        hardware_sync_mmio(emu);
        return;

    case EZ80_MMIO_REG_FLASH_ERASE_SECTOR:
        if (ez80_emulator_flash_erase_sector(emu, value) != EZ80_OK) {
            hardware_sync_mmio(emu);
        }
        return;

    case EZ80_MMIO_REG_PM_MODE:
        if (value <= EZ80_PM_MODE_SLEEP) {
            (void)ez80_emulator_pm_set_mode(emu, value);
        } else {
            hardware_sync_mmio(emu);
        }
        return;

    case EZ80_MMIO_REG_PM_WAKE_SOURCE:
        if (value == EZ80_PM_WAKE_NONE) {
            emu->hw.pm_wake_source = EZ80_PM_WAKE_NONE;
        }
        hardware_sync_mmio(emu);
        return;

    default:
        return;
    }
}

static bool os_valid_var_type(uint8_t type)
{
    return type <= EZ80_OS_VAR_PROGRAM;
}

static bool os_validate_var_name(const char *name)
{
    size_t i;
    size_t len;
    if (!name) return false;
    len = strlen(name);
    if (len == 0u || len > EZ80_OS_VAR_NAME_MAX) return false;
    for (i = 0u; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum((int)c) || c == '_')) {
            return false;
        }
    }
    return true;
}

static void os_normalize_var_name(const char *name, char out_name[EZ80_OS_VAR_NAME_MAX + 1u])
{
    size_t i;
    if (!out_name) return;
    memset(out_name, 0, EZ80_OS_VAR_NAME_MAX + 1u);
    if (!name) return;
    for (i = 0u; i < EZ80_OS_VAR_NAME_MAX && name[i] != '\0'; i++) {
        out_name[i] = (char)toupper((unsigned char)name[i]);
    }
    out_name[EZ80_OS_VAR_NAME_MAX] = '\0';
}

static int os_find_slot_index(const ez80_emulator_t *emu, const char *normalized_name, uint8_t type)
{
    uint32_t i;
    if (!emu || !normalized_name) return -1;
    for (i = 0u; i < EZ80_OS_MAX_VARS; i++) {
        const ez80_os_var_slot_t *slot = &emu->os.vars[i];
        if (!slot->in_use) continue;
        if (slot->type != type) continue;
        if (strncmp(slot->name, normalized_name, EZ80_OS_VAR_NAME_MAX + 1u) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int os_find_free_slot_index(const ez80_emulator_t *emu)
{
    uint32_t i;
    if (!emu) return -1;
    for (i = 0u; i < EZ80_OS_MAX_VARS; i++) {
        if (!emu->os.vars[i].in_use) {
            return (int)i;
        }
    }
    return -1;
}

static uint8_t os_count_vars(const ez80_emulator_t *emu)
{
    uint32_t i;
    uint8_t count = 0u;
    if (!emu) return 0u;
    for (i = 0u; i < EZ80_OS_MAX_VARS; i++) {
        if (emu->os.vars[i].in_use) {
            count++;
        }
    }
    return count;
}

static uint32_t os_crc32_bytes(const uint8_t *data, uint32_t size_bytes)
{
    uint32_t crc;
    uint32_t i;
    uint8_t bit;

    if (!data) return 0u;
    crc = 0xFFFFFFFFu;
    for (i = 0u; i < size_bytes; i++) {
        crc ^= (uint32_t)data[i];
        for (bit = 0u; bit < 8u; bit++) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void os_set_last_swap_failure(ez80_emulator_t *emu, uint8_t reason)
{
    if (!emu) return;
    emu->os.last_swap_failure = reason;
    if (reason != EZ80_OS_SWAP_FAILURE_NONE) {
        emu->os.swap_fail_count++;
    }
}

static void os_record_swap_crc(ez80_emulator_t *emu,
                               uint32_t archive_crc32,
                               uint32_t ram_crc32,
                               bool success)
{
    if (!emu) return;
    emu->os.last_swap_archive_crc32 = archive_crc32;
    emu->os.last_swap_ram_crc32 = ram_crc32;
    if (success) {
        emu->os.swap_count++;
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_NONE);
    }
}

static void os_set_active_program(ez80_emulator_t *emu, const ez80_os_var_slot_t *slot)
{
    if (!emu || !slot) return;
    strncpy(emu->os.active_program_name, slot->name, EZ80_OS_VAR_NAME_MAX);
    emu->os.active_program_name[EZ80_OS_VAR_NAME_MAX] = '\0';
    emu->os.active_program_ram_addr = slot->ram_addr;
}

static void os_clear_active_program(ez80_emulator_t *emu)
{
    if (!emu) return;
    memset(emu->os.active_program_name, 0, sizeof(emu->os.active_program_name));
    emu->os.active_program_ram_addr = 0u;
}

static void os_clear_buffers(void)
{
    uint32_t i;
    memset(g_graph_buffer, 0, sizeof(g_graph_buffer));
    memset(g_lcd_framebuffer, 0, sizeof(g_lcd_framebuffer));
    for (i = 0u; i < EZ80_HOME_ROWS; i++) {
        memset(g_home_screen[i], ' ', EZ80_HOME_COLS);
    }
}

static bool os_bcall_vector_supported(uint16_t vector)
{
    uint32_t i;
    for (i = 0u; i < (uint32_t)(sizeof(g_bcall_dispatch_table) / sizeof(g_bcall_dispatch_table[0])); i++) {
        if (g_bcall_dispatch_table[i].vector == vector) {
            return true;
        }
    }
    return false;
}

static uint8_t hardware_compute_keypad_data(const ez80_emulator_t *emu)
{
    uint8_t value = 0xFFu;
    uint8_t row;
    if (!emu) return 0xFFu;
    for (row = 0u; row < 8u; row++) {
        if ((emu->hw.keypad_row_select & (uint8_t)(1u << row)) == 0u) {
            value &= emu->hw.keypad_rows[row];
        }
    }
    return value;
}

static bool keyboard_any_key_pressed(const ez80_emulator_t *emu)
{
    uint8_t row;
    if (!emu) return false;
    for (row = 0u; row < 8u; row++) {
        if (emu->hw.keypad_rows[row] != 0xFFu) {
            return true;
        }
    }
    return false;
}

static uint8_t keyboard_compute_group_data(const ez80_emulator_t *emu)
{
    uint8_t row;
    uint8_t value = 0xFFu;
    uint8_t select;
    if (!emu) return 0xFFu;

    select = emu->hw.kbd_group_select;
    for (row = 0u; row < 8u; row++) {
        if ((select & (uint8_t)(1u << row)) == 0u) {
            value &= emu->hw.keypad_rows[row];
        }
    }
    return value;
}

static void lcd_refresh_status(ez80_emulator_t *emu);
static void lcd_pipeline_write_data(ez80_emulator_t *emu, uint8_t value);

static bool lcd_window_is_valid(const ez80_hardware_state_t *hw)
{
    if (!hw) return false;
    return (hw->lcd_window_x0 <= hw->lcd_window_x1) &&
           (hw->lcd_window_y0 <= hw->lcd_window_y1) &&
           (hw->lcd_window_x1 < EZ80_GRAPH_WIDTH) &&
           (hw->lcd_window_y1 < EZ80_GRAPH_HEIGHT);
}

static uint32_t lcd_panel_pixel_count(void)
{
    return (uint32_t)EZ80_GRAPH_WIDTH * (uint32_t)EZ80_GRAPH_HEIGHT;
}

static void lcd_set_full_window(ez80_hardware_state_t *hw)
{
    if (!hw) return;
    hw->lcd_window_x0 = 0u;
    hw->lcd_window_y0 = 0u;
    hw->lcd_window_x1 = (uint16_t)(EZ80_GRAPH_WIDTH - 1u);
    hw->lcd_window_y1 = (uint16_t)(EZ80_GRAPH_HEIGHT - 1u);
}

static void lcd_reset_stream_state(ez80_hardware_state_t *hw)
{
    if (!hw) return;
    hw->lcd_write_index = 0u;
    hw->lcd_pending_low_byte = false;
    hw->lcd_pipeline_state = EZ80_LCD_PIPELINE_IDLE;
}

static void lcd_reset_command_state(ez80_hardware_state_t *hw)
{
    if (!hw) return;
    hw->lcd_command_param_count = 0u;
    hw->lcd_command_param_expected = 0u;
    memset(hw->lcd_command_params, 0, sizeof(hw->lcd_command_params));
}

static uint32_t lcd_stream_pixel_count(const ez80_hardware_state_t *hw)
{
    uint32_t width;
    uint32_t height;

    if (!hw || !lcd_window_is_valid(hw)) {
        return lcd_panel_pixel_count();
    }

    width = (uint32_t)hw->lcd_window_x1 - (uint32_t)hw->lcd_window_x0 + 1u;
    height = (uint32_t)hw->lcd_window_y1 - (uint32_t)hw->lcd_window_y0 + 1u;
    return width * height;
}

static uint32_t lcd_stream_target_pixel(const ez80_hardware_state_t *hw, uint32_t stream_index)
{
    uint32_t ordinal;
    uint32_t width;
    uint32_t x;
    uint32_t y;

    if (!hw || !lcd_window_is_valid(hw)) {
        return stream_index % lcd_panel_pixel_count();
    }

    width = (uint32_t)hw->lcd_window_x1 - (uint32_t)hw->lcd_window_x0 + 1u;
    ordinal = stream_index % lcd_stream_pixel_count(hw);
    x = (uint32_t)hw->lcd_window_x0 + (ordinal % width);
    y = (uint32_t)hw->lcd_window_y0 + (ordinal / width);
    return (y * (uint32_t)EZ80_GRAPH_WIDTH) + x;
}

static void lcd_normalize_write_index(ez80_hardware_state_t *hw)
{
    uint32_t stream_count;

    if (!hw) return;
    stream_count = lcd_stream_pixel_count(hw);
    if (stream_count == 0u) {
        hw->lcd_write_index = 0u;
        return;
    }
    hw->lcd_write_index %= stream_count;
}

static void lcd_begin_command(ez80_hardware_state_t *hw, uint8_t command)
{
    if (!hw) return;

    hw->lcd_last_command = command;
    lcd_reset_stream_state(hw);
    lcd_reset_command_state(hw);

    switch (command) {
    case EZ80_LCD_CMD_COLUMN_ADDRESS_SET:
    case EZ80_LCD_CMD_ROW_ADDRESS_SET:
        hw->lcd_command_param_expected = 4u;
        break;
    case EZ80_LCD_CMD_MEMORY_ACCESS_CONTROL:
        hw->lcd_command_param_expected = 1u;
        break;
    default:
        break;
    }
}

static void lcd_apply_command_data(ez80_emulator_t *emu, uint8_t value)
{
    ez80_hardware_state_t *hw;

    if (!emu) return;
    hw = &emu->hw;
    hw->lcd_last_data = value;

    if (hw->lcd_command_param_expected == 0u) {
        if (hw->lcd_last_command == EZ80_LCD_CMD_MEMORY_WRITE) {
            lcd_pipeline_write_data(emu, value);
        } else {
            hw->lcd_status |= EZ80_LCD_STATUS_DATA_READY;
            lcd_refresh_status(emu);
        }
        return;
    }

    if (hw->lcd_command_param_count < sizeof(hw->lcd_command_params)) {
        hw->lcd_command_params[hw->lcd_command_param_count] = value;
    }
    hw->lcd_command_param_count++;
    if (hw->lcd_command_param_count < hw->lcd_command_param_expected) {
        hw->lcd_status |= EZ80_LCD_STATUS_DATA_READY;
        lcd_refresh_status(emu);
        return;
    }

    switch (hw->lcd_last_command) {
    case EZ80_LCD_CMD_COLUMN_ADDRESS_SET:
        hw->lcd_window_x0 = (uint16_t)(((uint16_t)hw->lcd_command_params[0] << 8) |
                                       (uint16_t)hw->lcd_command_params[1]);
        hw->lcd_window_x1 = (uint16_t)(((uint16_t)hw->lcd_command_params[2] << 8) |
                                       (uint16_t)hw->lcd_command_params[3]);
        break;
    case EZ80_LCD_CMD_ROW_ADDRESS_SET:
        hw->lcd_window_y0 = (uint16_t)(((uint16_t)hw->lcd_command_params[0] << 8) |
                                       (uint16_t)hw->lcd_command_params[1]);
        hw->lcd_window_y1 = (uint16_t)(((uint16_t)hw->lcd_command_params[2] << 8) |
                                       (uint16_t)hw->lcd_command_params[3]);
        break;
    case EZ80_LCD_CMD_MEMORY_ACCESS_CONTROL:
        hw->lcd_timing = hw->lcd_command_params[0];
        break;
    default:
        break;
    }

    lcd_reset_command_state(hw);
    lcd_normalize_write_index(hw);
    hw->lcd_status |= EZ80_LCD_STATUS_DATA_READY;
    lcd_refresh_status(emu);
}

static void lcd_refresh_status(ez80_emulator_t *emu)
{
    uint8_t status;
    if (!emu) return;

    status = emu->hw.lcd_status & (uint8_t)~(EZ80_LCD_STATUS_WINDOW_VALID | EZ80_LCD_STATUS_CONTROLLER_ENABLED);
    if (lcd_window_is_valid(&emu->hw)) {
        status |= EZ80_LCD_STATUS_WINDOW_VALID;
    }
    if ((emu->hw.lcd_mode & 0x01u) != 0u) {
        status |= EZ80_LCD_STATUS_CONTROLLER_ENABLED;
    }
    if (emu->hw.lcd_pending_low_byte) {
        status |= EZ80_LCD_STATUS_PIPELINE_BUSY;
    } else {
        status &= (uint8_t)~EZ80_LCD_STATUS_PIPELINE_BUSY;
    }

    emu->hw.lcd_status = status;
}

static void usb_update_status(ez80_emulator_t *emu)
{
    uint8_t status;
    if (!emu) return;

    status = emu->hw.usb_status & EZ80_USB_STATUS_OVERRUN;
    if (emu->hw.usb_connected) status |= EZ80_USB_STATUS_CONNECTED;
    if ((emu->hw.irq_pending & EZ80_IRQ_SOURCE_USB) != 0u) {
        status |= EZ80_USB_STATUS_IRQ_PENDING;
    }
    if ((emu->hw.usb_control & EZ80_USB_CONTROL_LOOPBACK) != 0u) {
        status |= EZ80_USB_STATUS_LOOPBACK;
    }
    if ((emu->hw.usb_control & EZ80_USB_CONTROL_ENABLE) != 0u) {
        if (emu->hw.usb_rx_count > 0u) status |= EZ80_USB_STATUS_RX_READY;
        if (emu->hw.usb_tx_count < EZ80_USB_FIFO_SIZE) status |= EZ80_USB_STATUS_TX_READY;
    }
    emu->hw.usb_status = status;
    if (emu->hw.usb_rx_count > 0u) {
        emu->hw.usb_rx_data = emu->hw.usb_rx_fifo[emu->hw.usb_rx_head];
    }
}

static uint8_t gpio_known_peripheral_value(const ez80_emulator_t *emu, uint8_t port_index)
{
    uint8_t value = 0u;
    if (!emu || port_index >= EZ80_GPIO_PORT_COUNT) return 0u;

    if (port_index == EZ80_GPIO_BACKLIGHT_PORT && emu->hw.backlight_on) {
        value |= EZ80_GPIO_BACKLIGHT_BIT;
    }
    if (port_index == EZ80_GPIO_POWER_PORT) {
        if (emu->hw.usb_connected && emu->hw.battery_level < 100u) {
            value |= EZ80_GPIO_CHARGING_BIT;
        }
    }
    if (port_index == EZ80_GPIO_USB_PORT && emu->hw.usb_connected) {
        value |= EZ80_GPIO_USB_DETECT_BIT;
    }

    return value;
}

static uint8_t gpio_compute_effective_state(const ez80_emulator_t *emu, uint8_t port_index)
{
    const ez80_gpio_port_t *gpio;
    uint8_t peripheral;
    uint8_t effective;
    uint8_t mask;

    if (!emu || port_index >= EZ80_GPIO_PORT_COUNT) return 0u;

    gpio = &emu->hw.gpio[port_index];
    peripheral = gpio_known_peripheral_value(emu, port_index);
    effective = gpio->data;

    for (mask = 1u; mask != 0u; mask <<= 1) {
        if (((gpio->alt0 | gpio->alt1) & mask) != 0u) {
            if ((peripheral & mask) != 0u) {
                effective |= mask;
            } else {
                effective &= (uint8_t)~mask;
            }
            continue;
        }

        if ((gpio->direction & mask) != 0u) {
            if ((gpio->output_latch & mask) != 0u) {
                effective |= mask;
            } else {
                effective &= (uint8_t)~mask;
            }
            continue;
        }

        if ((gpio->input_mask & mask) != 0u) {
            if ((gpio->input_latch & mask) != 0u) {
                effective |= mask;
            } else {
                effective &= (uint8_t)~mask;
            }
            continue;
        }

        if (((gpio->pull_up & mask) != 0u) && ((gpio->pull_down & mask) == 0u)) {
            effective |= mask;
            continue;
        }
        if (((gpio->pull_down & mask) != 0u) && ((gpio->pull_up & mask) == 0u)) {
            effective &= (uint8_t)~mask;
            continue;
        }

        if ((gpio->output_latch & mask) != 0u) {
            effective |= mask;
        } else {
            effective &= (uint8_t)~mask;
        }
    }

    return effective;
}

static void gpio_refresh_port_state(ez80_emulator_t *emu, uint8_t port_index)
{
    ez80_gpio_port_t *gpio;
    uint8_t before;
    uint8_t after;
    uint8_t rising;
    uint8_t falling;

    if (!emu || port_index >= EZ80_GPIO_PORT_COUNT) return;

    gpio = &emu->hw.gpio[port_index];
    before = gpio->last_sampled;
    after = gpio_compute_effective_state(emu, port_index);
    rising = (uint8_t)((uint8_t)(~before) & after & gpio->irq_rising);
    falling = (uint8_t)(before & (uint8_t)(~after) & gpio->irq_falling);
    gpio->irq_pending |= (uint8_t)(rising | falling);
    gpio->data = after;
    gpio->last_sampled = after;
}

static void gpio_sync_external_inputs(ez80_emulator_t *emu)
{
    if (!emu) return;

    emu->hw.gpio[EZ80_GPIO_POWER_PORT].input_mask |=
        (uint8_t)(EZ80_GPIO_POWER_BIT | EZ80_GPIO_CHARGING_BIT);
    emu->hw.gpio[EZ80_GPIO_POWER_PORT].input_latch &=
        (uint8_t)~(EZ80_GPIO_POWER_BIT | EZ80_GPIO_CHARGING_BIT);
    if (emu->hw.usb_connected && emu->hw.battery_level < 100u) {
        emu->hw.gpio[EZ80_GPIO_POWER_PORT].input_latch |= EZ80_GPIO_CHARGING_BIT;
    }

    emu->hw.gpio[EZ80_GPIO_USB_PORT].input_mask |= EZ80_GPIO_USB_DETECT_BIT;
    if (emu->hw.usb_connected) {
        emu->hw.gpio[EZ80_GPIO_USB_PORT].input_latch |= EZ80_GPIO_USB_DETECT_BIT;
    } else {
        emu->hw.gpio[EZ80_GPIO_USB_PORT].input_latch &= (uint8_t)~EZ80_GPIO_USB_DETECT_BIT;
    }

    gpio_refresh_port_state(emu, EZ80_GPIO_BACKLIGHT_PORT);
    gpio_refresh_port_state(emu, EZ80_GPIO_POWER_PORT);
    gpio_refresh_port_state(emu, EZ80_GPIO_USB_PORT);
}

static void usb_set_connected_state(ez80_emulator_t *emu, bool connected)
{
    if (!emu) return;
    emu->hw.usb_connected = connected;
    gpio_sync_external_inputs(emu);
    usb_update_status(emu);
}

static void usb_reset_controller(ez80_emulator_t *emu)
{
    if (!emu) return;
    emu->hw.usb_tx_head = 0u;
    emu->hw.usb_tx_count = 0u;
    emu->hw.usb_rx_head = 0u;
    emu->hw.usb_rx_count = 0u;
    emu->hw.usb_rx_data = 0u;
    emu->hw.usb_last_tx_data = 0u;
    emu->hw.irq_pending &= (uint8_t)~EZ80_IRQ_SOURCE_USB;
    emu->hw.usb_status &= (uint8_t)~EZ80_USB_STATUS_OVERRUN;
    usb_update_status(emu);
}

static void usb_raise_irq_if_enabled(ez80_emulator_t *emu, uint8_t enable_mask)
{
    if (!emu) return;
    if ((emu->hw.usb_control & EZ80_USB_CONTROL_ENABLE) == 0u) return;
    if (!emu->hw.usb_connected) return;
    if ((emu->hw.usb_control & enable_mask) == 0u) return;
    hardware_raise_irq(emu, EZ80_IRQ_SOURCE_USB);
}

static void usb_push_rx_byte(ez80_emulator_t *emu, uint8_t value, bool irq_event)
{
    uint8_t idx;
    if (!emu) return;
    emu->hw.usb_rx_data = value;
    if ((emu->hw.usb_control & EZ80_USB_CONTROL_ENABLE) == 0u || !emu->hw.usb_connected) {
        usb_update_status(emu);
        return;
    }
    if (emu->hw.usb_rx_count < EZ80_USB_FIFO_SIZE) {
        idx = (uint8_t)((emu->hw.usb_rx_head + emu->hw.usb_rx_count) % EZ80_USB_FIFO_SIZE);
        emu->hw.usb_rx_fifo[idx] = value;
        emu->hw.usb_rx_count++;
        emu->hw.usb_total_rx_bytes++;
        if (irq_event) {
            usb_raise_irq_if_enabled(emu, EZ80_USB_CONTROL_RX_IRQ_ENABLE);
        }
    } else {
        emu->hw.usb_status |= EZ80_USB_STATUS_OVERRUN;
    }
    usb_update_status(emu);
}

static void usb_queue_tx_byte(ez80_emulator_t *emu, uint8_t value)
{
    uint8_t idx;
    if (!emu) return;
    emu->hw.usb_last_tx_data = value;
    if ((emu->hw.usb_control & EZ80_USB_CONTROL_ENABLE) == 0u || !emu->hw.usb_connected) {
        usb_update_status(emu);
        return;
    }
    if (emu->hw.usb_tx_count < EZ80_USB_FIFO_SIZE) {
        idx = (uint8_t)((emu->hw.usb_tx_head + emu->hw.usb_tx_count) % EZ80_USB_FIFO_SIZE);
        emu->hw.usb_tx_fifo[idx] = value;
        emu->hw.usb_tx_count++;
        emu->hw.usb_total_tx_bytes++;
        if ((emu->hw.usb_control & EZ80_USB_CONTROL_LOOPBACK) != 0u) {
            usb_push_rx_byte(emu, value, true);
        }
    } else {
        emu->hw.usb_status |= EZ80_USB_STATUS_OVERRUN;
    }
    usb_update_status(emu);
}

static uint8_t usb_pop_rx_byte(ez80_emulator_t *emu)
{
    uint8_t value;
    if (!emu) return 0u;
    if (emu->hw.usb_rx_count == 0u) {
        usb_update_status(emu);
        return emu->hw.usb_rx_data;
    }
    value = emu->hw.usb_rx_fifo[emu->hw.usb_rx_head];
    emu->hw.usb_rx_head = (uint8_t)((emu->hw.usb_rx_head + 1u) % EZ80_USB_FIFO_SIZE);
    emu->hw.usb_rx_count--;
    emu->hw.usb_rx_data = value;
    usb_update_status(emu);
    return value;
}

static void usb_advance(ez80_emulator_t *emu, uint32_t cycles)
{
    uint32_t bytes_to_drain;
    if (!emu || cycles == 0u) return;
    if ((emu->hw.usb_control & EZ80_USB_CONTROL_ENABLE) == 0u) {
        usb_update_status(emu);
        return;
    }
    if (!emu->hw.usb_connected || emu->hw.usb_tx_count == 0u) {
        usb_update_status(emu);
        return;
    }

    bytes_to_drain = cycles / EZ80_USB_DRAIN_CYCLES_PER_BYTE;
    if (bytes_to_drain == 0u) bytes_to_drain = 1u;
    if (bytes_to_drain > emu->hw.usb_tx_count) {
        bytes_to_drain = emu->hw.usb_tx_count;
    }

    emu->hw.usb_tx_head = (uint8_t)((emu->hw.usb_tx_head + bytes_to_drain) % EZ80_USB_FIFO_SIZE);
    emu->hw.usb_tx_count = (uint8_t)(emu->hw.usb_tx_count - bytes_to_drain);
    if (emu->hw.usb_tx_count == 0u) {
        usb_raise_irq_if_enabled(emu, EZ80_USB_CONTROL_TX_IRQ_ENABLE);
    }
    usb_update_status(emu);
}

static void hardware_sync_ports(ez80_emulator_t *emu)
{
    uint32_t rtc_seconds;
    uint32_t timer0;
    uint32_t timer1;
    uint8_t keypad_data;
    if (!emu) return;

    lcd_refresh_status(emu);
    keypad_regs_sync(emu);
    usb_update_status(emu);
    keypad_data = hardware_compute_keypad_data(emu);
    port_shadow_write(emu, EZ80_PORT_KEYPAD_ROW_SELECT, emu->hw.keypad_row_select);
    port_shadow_write(emu, EZ80_PORT_KEYPAD_DATA, keypad_data);
    port_shadow_write(emu, EZ80_PORT_LCD_COMMAND, emu->hw.lcd_last_command);
    port_shadow_write(emu, EZ80_PORT_LCD_DATA, emu->hw.lcd_last_data);
    port_shadow_write(emu, EZ80_PORT_LCD_STATUS_STUB, emu->hw.lcd_status);
    port_shadow_write(emu, EZ80_PORT_USB_CONTROL, emu->hw.usb_control);
    port_shadow_write(emu, EZ80_PORT_USB_STATUS, emu->hw.usb_status);
    port_shadow_write(emu, EZ80_PORT_USB_TX, emu->hw.usb_last_tx_data);
    port_shadow_write(emu, EZ80_PORT_USB_RX, emu->hw.usb_rx_data);
    timer0 = emu->hw.timer0_counter;
    timer1 = emu->hw.timer1_counter;
    port_shadow_write(emu, EZ80_PORT_TIMER0_CONTROL, emu->hw.timer0_control);
    port_shadow_write(emu, EZ80_PORT_TIMER0_COUNTER_LO, (uint8_t)(timer0 & 0xFFu));
    port_shadow_write(emu, EZ80_PORT_TIMER0_COUNTER_HI, (uint8_t)((timer0 >> 8) & 0xFFu));
    port_shadow_write(emu, EZ80_PORT_TIMER1_CONTROL, emu->hw.timer1_control);
    port_shadow_write(emu, EZ80_PORT_TIMER1_COUNTER_LO, (uint8_t)(timer1 & 0xFFu));
    port_shadow_write(emu, EZ80_PORT_TIMER1_COUNTER_HI, (uint8_t)((timer1 >> 8) & 0xFFu));
    rtc_seconds = emu->hw.rtc_seconds_of_day % (24u * 60u * 60u);
    port_shadow_write(emu, EZ80_PORT_RTC_SECONDS, (uint8_t)(rtc_seconds % 60u));
    port_shadow_write(emu, EZ80_PORT_RTC_MINUTES, (uint8_t)((rtc_seconds / 60u) % 60u));
    port_shadow_write(emu, EZ80_PORT_RTC_HOURS, (uint8_t)((rtc_seconds / 3600u) % 24u));
    port_shadow_write(emu, EZ80_PORT_RTC_ALARM_SECONDS, emu->hw.rtc_alarm_seconds);
    port_shadow_write(emu, EZ80_PORT_RTC_ALARM_MINUTES, emu->hw.rtc_alarm_minutes);
    port_shadow_write(emu, EZ80_PORT_RTC_ALARM_HOURS, emu->hw.rtc_alarm_hours);
    port_shadow_write(emu, EZ80_PORT_RTC_ALARM_CONTROL, emu->hw.rtc_alarm_control);
    port_shadow_write(emu, EZ80_PORT_IRQ_MASK, emu->hw.irq_mask);
    port_shadow_write(emu, EZ80_PORT_IRQ_PENDING, emu->hw.irq_pending);
    port_shadow_write(emu, EZ80_PORT_TIMER0_RELOAD_LO, (uint8_t)(emu->hw.timer0_reload & 0xFFu));
    port_shadow_write(emu, EZ80_PORT_TIMER0_RELOAD_HI, (uint8_t)((emu->hw.timer0_reload >> 8) & 0xFFu));
    port_shadow_write(emu, EZ80_PORT_TIMER1_RELOAD_LO, (uint8_t)(emu->hw.timer1_reload & 0xFFu));
    port_shadow_write(emu, EZ80_PORT_TIMER1_RELOAD_HI, (uint8_t)((emu->hw.timer1_reload >> 8) & 0xFFu));
    port_shadow_write(emu, EZ80_PORT_TIMER0_PRESCALER, emu->hw.timer0_prescaler);
    port_shadow_write(emu, EZ80_PORT_TIMER1_PRESCALER, emu->hw.timer1_prescaler);
    port_shadow_write(emu, EZ80_PORT_WDT_CONTROL, emu->hw.wdt_control);
    port_shadow_write(emu, EZ80_PORT_WDT_FEED, 0u);
    port_shadow_write(emu, EZ80_PORT_WDT_TIMEOUT_LO, (uint8_t)(emu->hw.wdt_timeout & 0xFFu));
    port_shadow_write(emu, EZ80_PORT_WDT_TIMEOUT_HI, (uint8_t)((emu->hw.wdt_timeout >> 8) & 0xFFu));
}

static void os_sync_mmio(ez80_emulator_t *emu)
{
    uint8_t i;
    if (!emu) return;
    mmio_set(emu, EZ80_MMIO_REG_OS_VAR_COUNT, os_count_vars(emu));
    mmio_set(emu, EZ80_MMIO_REG_OS_LAST_BCALL_LO, (uint8_t)(emu->os.last_bcall_vector & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_OS_LAST_BCALL_HI, (uint8_t)((emu->os.last_bcall_vector >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_OS_LAST_STATUS, emu->os.last_bcall_status);
    mmio_set24(emu, EZ80_MMIO_REG_OS_LAST_RESULT_0, emu->os.last_bcall_result);
    mmio_set(emu, EZ80_MMIO_REG_OS_HOME_ROW, emu->os.home_row);
    mmio_set(emu, EZ80_MMIO_REG_OS_HOME_COL, emu->os.home_col);
    mmio_set(emu, EZ80_MMIO_REG_OS_GRAPH_LAST_X, (uint8_t)(emu->os.graph_last_x & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_OS_GRAPH_LAST_Y, (uint8_t)(emu->os.graph_last_y & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_OS_GRAPH_LAST_COLOR, emu->os.graph_last_color);
    for (i = 0u; i < (EZ80_OS_VAR_NAME_MAX + 1u); i++) {
        mmio_set(emu,
                 (uint16_t)(EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_NAME_0 + i),
                 (uint8_t)emu->os.active_program_name[i]);
    }
    mmio_set24(emu, EZ80_MMIO_REG_OS_ACTIVE_PROGRAM_RAM_0, emu->os.active_program_ram_addr);
    mmio_set24(emu, EZ80_MMIO_REG_OS_LAUNCH_COUNT_0, emu->os.launch_count);
    mmio_set24(emu, EZ80_MMIO_REG_OS_SWAP_COUNT_0, emu->os.swap_count);
    mmio_set24(emu, EZ80_MMIO_REG_OS_SWAP_FAIL_COUNT_0, emu->os.swap_fail_count);
    mmio_set(emu, EZ80_MMIO_REG_OS_LAST_SWAP_FAILURE, emu->os.last_swap_failure);
    mmio_set32(emu, EZ80_MMIO_REG_OS_LAST_SWAP_ARCHIVE_CRC_0, emu->os.last_swap_archive_crc32);
    mmio_set32(emu, EZ80_MMIO_REG_OS_LAST_SWAP_RAM_CRC_0, emu->os.last_swap_ram_crc32);
}

static void hardware_sync_mmio(ez80_emulator_t *emu)
{
    if (!emu) return;
    mmio_set(emu, EZ80_MMIO_REG_HW_KEYPAD_ROW_MASK, emu->hw.keypad_row_select);
    mmio_set(emu, EZ80_MMIO_REG_HW_KEYPAD_DATA, emu->memory.ports[EZ80_PORT_KEYPAD_DATA]);
    mmio_set(emu, EZ80_MMIO_REG_HW_LCD_COMMAND, emu->hw.lcd_last_command);
    mmio_set(emu, EZ80_MMIO_REG_HW_LCD_DATA, emu->hw.lcd_last_data);
    mmio_set(emu, EZ80_MMIO_REG_HW_LCD_WRITE_INDEX_LO, (uint8_t)(emu->hw.lcd_write_index & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_HW_LCD_WRITE_INDEX_HI, (uint8_t)((emu->hw.lcd_write_index >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_HW_USB_CONTROL, emu->hw.usb_control);
    mmio_set(emu, EZ80_MMIO_REG_HW_USB_STATUS, emu->hw.usb_status);
    mmio_set(emu, EZ80_MMIO_REG_USB_RX_COUNT_LO, (uint8_t)(emu->hw.usb_total_rx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_USB_RX_COUNT_HI, (uint8_t)((emu->hw.usb_total_rx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_USB_TX_COUNT_LO, (uint8_t)(emu->hw.usb_total_tx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_USB_TX_COUNT_HI, (uint8_t)((emu->hw.usb_total_tx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_USB_RX_LEVEL, emu->hw.usb_rx_count);
    mmio_set(emu, EZ80_MMIO_REG_USB_TX_LEVEL, emu->hw.usb_tx_count);
    mmio_set(emu, EZ80_MMIO_REG_USB_LAST_RX, emu->hw.usb_rx_data);
    mmio_set(emu, EZ80_MMIO_REG_USB_LAST_TX, emu->hw.usb_last_tx_data);
    mmio_set(emu, EZ80_MMIO_REG_USB_CONNECTED, emu->hw.usb_connected ? 1u : 0u);
    mmio_set(emu, EZ80_MMIO_REG_HW_TIMER0_LO, emu->memory.ports[EZ80_PORT_TIMER0_COUNTER_LO]);
    mmio_set(emu, EZ80_MMIO_REG_HW_TIMER0_HI, emu->memory.ports[EZ80_PORT_TIMER0_COUNTER_HI]);
    mmio_set(emu, EZ80_MMIO_REG_HW_TIMER1_LO, emu->memory.ports[EZ80_PORT_TIMER1_COUNTER_LO]);
    mmio_set(emu, EZ80_MMIO_REG_HW_TIMER1_HI, emu->memory.ports[EZ80_PORT_TIMER1_COUNTER_HI]);
    mmio_set(emu, EZ80_MMIO_REG_HW_RTC_SECONDS, emu->memory.ports[EZ80_PORT_RTC_SECONDS]);
    mmio_set(emu, EZ80_MMIO_REG_HW_RTC_MINUTES, emu->memory.ports[EZ80_PORT_RTC_MINUTES]);
    mmio_set(emu, EZ80_MMIO_REG_HW_RTC_HOURS, emu->memory.ports[EZ80_PORT_RTC_HOURS]);
    mmio_set(emu, EZ80_MMIO_REG_HW_IRQ_MASK, emu->hw.irq_mask);
    mmio_set(emu, EZ80_MMIO_REG_HW_IRQ_PENDING, emu->hw.irq_pending);
    mmio_set(emu, EZ80_MMIO_REG_TIMER0_PRESCALER, emu->hw.timer0_prescaler);
    mmio_set(emu, EZ80_MMIO_REG_TIMER1_PRESCALER, emu->hw.timer1_prescaler);
    mmio_set(emu, EZ80_MMIO_REG_TIMER0_RELOAD_LO, (uint8_t)(emu->hw.timer0_reload & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_TIMER0_RELOAD_HI, (uint8_t)((emu->hw.timer0_reload >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_TIMER1_RELOAD_LO, (uint8_t)(emu->hw.timer1_reload & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_TIMER1_RELOAD_HI, (uint8_t)((emu->hw.timer1_reload >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_EI_PENDING, emu->cpu.ei_pending ? 1u : 0u);
    mmio_set(emu, EZ80_MMIO_REG_WATCHPOINT_COUNT, (uint8_t)emu->watchpoint_count);
    /* DMA MMIO sync */
    {
        uint8_t ch;
        static const uint16_t dma_base[EZ80_DMA_CHANNEL_COUNT] = {
            EZ80_MMIO_REG_DMA0_SRC_0,
            EZ80_MMIO_REG_DMA1_SRC_0,
            EZ80_MMIO_REG_DMA2_SRC_0
        };
        static const uint16_t dma_transferred_base[EZ80_DMA_CHANNEL_COUNT] = {
            EZ80_MMIO_REG_DMA0_TRANSFERRED_0,
            EZ80_MMIO_REG_DMA1_TRANSFERRED_0,
            EZ80_MMIO_REG_DMA2_TRANSFERRED_0
        };
        for (ch = 0u; ch < EZ80_DMA_CHANNEL_COUNT; ch++) {
            uint16_t base = dma_base[ch];
            mmio_set24(emu, base, emu->hw.dma[ch].src_addr);
            mmio_set24(emu, (uint16_t)(base + 3u), emu->hw.dma[ch].dst_addr);
            mmio_set24(emu, (uint16_t)(base + 6u), emu->hw.dma[ch].length);
            mmio_set(emu, (uint16_t)(base + 9u), emu->hw.dma[ch].control);
            mmio_set(emu, (uint16_t)(base + 10u), emu->hw.dma[ch].status);
            mmio_set24(emu, dma_transferred_base[ch], emu->hw.dma[ch].bytes_transferred);
        }
        mmio_set32(emu, EZ80_MMIO_REG_DMA_TOTAL_TRANSFERS_0, emu->hw.dma_total_transfers);
    }
    /* Interrupt nesting MMIO sync */
    mmio_set(emu, EZ80_MMIO_REG_IRQ_NESTING_DEPTH, emu->hw.irq_nesting_depth);
    mmio_set(emu, EZ80_MMIO_REG_IRQ_TOTAL_RETI, (uint8_t)(emu->hw.total_reti_count & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_IRQ_LAST_RETI_SOURCE, emu->hw.last_reti_source);
    mmio_set(emu, EZ80_MMIO_REG_NMI_COUNT, (uint8_t)(emu->hw.nmi_count & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_NMI_ACTIVE, emu->hw.nmi_active ? 1u : 0u);
    /* SPI MMIO sync */
    mmio_set(emu, EZ80_MMIO_REG_SPI0_CONTROL, emu->hw.spi[0].control);
    mmio_set(emu, EZ80_MMIO_REG_SPI0_STATUS, emu->hw.spi[0].status);
    mmio_set(emu, EZ80_MMIO_REG_SPI0_BAUD, emu->hw.spi[0].baud_divisor);
    mmio_set(emu, EZ80_MMIO_REG_SPI0_TX_COUNT_LO, (uint8_t)(emu->hw.spi[0].total_tx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_SPI0_TX_COUNT_HI, (uint8_t)((emu->hw.spi[0].total_tx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_SPI0_RX_COUNT_LO, (uint8_t)(emu->hw.spi[0].total_rx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_SPI0_RX_COUNT_HI, (uint8_t)((emu->hw.spi[0].total_rx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_SPI1_CONTROL, emu->hw.spi[1].control);
    mmio_set(emu, EZ80_MMIO_REG_SPI1_STATUS, emu->hw.spi[1].status);
    mmio_set(emu, EZ80_MMIO_REG_SPI1_BAUD, emu->hw.spi[1].baud_divisor);
    mmio_set(emu, EZ80_MMIO_REG_SPI1_TX_COUNT_LO, (uint8_t)(emu->hw.spi[1].total_tx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_SPI1_TX_COUNT_HI, (uint8_t)((emu->hw.spi[1].total_tx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_SPI1_RX_COUNT_LO, (uint8_t)(emu->hw.spi[1].total_rx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_SPI1_RX_COUNT_HI, (uint8_t)((emu->hw.spi[1].total_rx_bytes >> 8) & 0xFFu));
    /* GPIO MMIO sync */
    mmio_set(emu, EZ80_MMIO_REG_GPIO_A_DATA, emu->hw.gpio[0].data);
    mmio_set(emu, EZ80_MMIO_REG_GPIO_A_DIR, emu->hw.gpio[0].direction);
    mmio_set(emu, EZ80_MMIO_REG_GPIO_B_DATA, emu->hw.gpio[1].data);
    mmio_set(emu, EZ80_MMIO_REG_GPIO_B_DIR, emu->hw.gpio[1].direction);
    mmio_set(emu, EZ80_MMIO_REG_GPIO_C_DATA, emu->hw.gpio[2].data);
    mmio_set(emu, EZ80_MMIO_REG_GPIO_C_DIR, emu->hw.gpio[2].direction);
    mmio_set(emu, EZ80_MMIO_REG_GPIO_D_DATA, emu->hw.gpio[3].data);
    mmio_set(emu, EZ80_MMIO_REG_GPIO_D_DIR, emu->hw.gpio[3].direction);
    mmio_set(emu, EZ80_MMIO_REG_GPIO_E_DATA, emu->hw.gpio[4].data);
    mmio_set(emu, EZ80_MMIO_REG_GPIO_E_DIR, emu->hw.gpio[4].direction);
    mmio_set(emu, EZ80_MMIO_REG_GPIO_F_DATA, emu->hw.gpio[5].data);
    mmio_set(emu, EZ80_MMIO_REG_GPIO_F_DIR, emu->hw.gpio[5].direction);
    mmio_set(emu, EZ80_MMIO_REG_BACKLIGHT_STATE, emu->hw.backlight_on ? 1u : 0u);
    mmio_set(emu, EZ80_MMIO_REG_BATTERY_LEVEL, emu->hw.battery_level);
    mmio_set(emu, EZ80_MMIO_REG_POWER_STATE, emu->hw.power_state);
    /* RTC alarm MMIO sync */
    mmio_set(emu, EZ80_MMIO_REG_RTC_ALARM_SECONDS, emu->hw.rtc_alarm_seconds);
    mmio_set(emu, EZ80_MMIO_REG_RTC_ALARM_MINUTES, emu->hw.rtc_alarm_minutes);
    mmio_set(emu, EZ80_MMIO_REG_RTC_ALARM_HOURS, emu->hw.rtc_alarm_hours);
    mmio_set(emu, EZ80_MMIO_REG_RTC_ALARM_ENABLED, (emu->hw.rtc_alarm_control & EZ80_RTC_ALARM_CONTROL_ENABLE) ? 1u : 0u);
    mmio_set(emu, EZ80_MMIO_REG_RTC_ALARM_FIRED, emu->hw.rtc_alarm_fired ? 1u : 0u);
    mmio_set(emu, EZ80_MMIO_REG_RTC_ALARM_FIRE_COUNT, (uint8_t)(emu->hw.rtc_alarm_fire_count & 0xFFu));
    /* Watchdog MMIO sync */
    mmio_set(emu, EZ80_MMIO_REG_WDT_CONTROL, emu->hw.wdt_control);
    mmio_set(emu, EZ80_MMIO_REG_WDT_COUNTER_LO, (uint8_t)(emu->hw.wdt_counter & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_WDT_COUNTER_HI, (uint8_t)((emu->hw.wdt_counter >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_WDT_TIMEOUT_LO, (uint8_t)(emu->hw.wdt_timeout & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_WDT_TIMEOUT_HI, (uint8_t)((emu->hw.wdt_timeout >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_WDT_RESET_COUNT, (uint8_t)(emu->hw.wdt_reset_count & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_WDT_LAST_RESET_CYCLE_LO, (uint8_t)(emu->hw.wdt_last_reset_cycle & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_WDT_LAST_RESET_CYCLE_HI, (uint8_t)((emu->hw.wdt_last_reset_cycle >> 8) & 0xFFu));
    /* Power management MMIO sync */
    mmio_set(emu, EZ80_MMIO_REG_PM_MODE, emu->hw.pm_mode);
    mmio_set(emu, EZ80_MMIO_REG_PM_WAKE_SOURCE, emu->hw.pm_wake_source);
    mmio_set(emu, EZ80_MMIO_REG_PM_SLEEP_COUNT, (uint8_t)(emu->hw.pm_sleep_count & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_PM_TOTAL_SLEEP_CYCLES_LO, (uint8_t)(emu->hw.pm_total_sleep_cycles & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_PM_TOTAL_SLEEP_CYCLES_HI, (uint8_t)((emu->hw.pm_total_sleep_cycles >> 8) & 0xFFu));
    /* Flash erase MMIO sync */
    mmio_set(emu, EZ80_MMIO_REG_FLASH_ERASE_SECTOR, emu->hw.flash_erase_last_sector);
    mmio_set(emu, EZ80_MMIO_REG_FLASH_ERASE_STATUS, emu->hw.flash_erase_last_status);
    mmio_set(emu, EZ80_MMIO_REG_FLASH_ERASE_COUNT_LO, (uint8_t)(emu->hw.flash_erase_count & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_FLASH_ERASE_COUNT_HI, (uint8_t)((emu->hw.flash_erase_count >> 8) & 0xFFu));
    /* I2C MMIO sync */
    mmio_set(emu, EZ80_MMIO_REG_I2C0_CTL, emu->hw.i2c[0].control);
    mmio_set(emu, EZ80_MMIO_REG_I2C0_SR, emu->hw.i2c[0].status);
    mmio_set(emu, EZ80_MMIO_REG_I2C0_CCR, emu->hw.i2c[0].clock_control);
    mmio_set(emu, EZ80_MMIO_REG_I2C0_TX_COUNT_LO, (uint8_t)(emu->hw.i2c[0].total_tx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_I2C0_TX_COUNT_HI, (uint8_t)((emu->hw.i2c[0].total_tx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_I2C0_RX_COUNT_LO, (uint8_t)(emu->hw.i2c[0].total_rx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_I2C0_RX_COUNT_HI, (uint8_t)((emu->hw.i2c[0].total_rx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_I2C0_SAR, emu->hw.i2c[0].slave_addr);
    mmio_set(emu, EZ80_MMIO_REG_I2C0_XSAR, emu->hw.i2c[0].xslave_addr);
    mmio_set(emu, EZ80_MMIO_REG_I2C0_DATA,
             (emu->hw.i2c[0].rx_count > 0u) ? emu->hw.i2c[0].rx_buf[emu->hw.i2c[0].rx_head] : 0xFFu);
    mmio_set(emu, EZ80_MMIO_REG_I2C1_CTL, emu->hw.i2c[1].control);
    mmio_set(emu, EZ80_MMIO_REG_I2C1_SR, emu->hw.i2c[1].status);
    mmio_set(emu, EZ80_MMIO_REG_I2C1_CCR, emu->hw.i2c[1].clock_control);
    mmio_set(emu, EZ80_MMIO_REG_I2C1_TX_COUNT_LO, (uint8_t)(emu->hw.i2c[1].total_tx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_I2C1_TX_COUNT_HI, (uint8_t)((emu->hw.i2c[1].total_tx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_I2C1_RX_COUNT_LO, (uint8_t)(emu->hw.i2c[1].total_rx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_I2C1_RX_COUNT_HI, (uint8_t)((emu->hw.i2c[1].total_rx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_I2C1_SAR, emu->hw.i2c[1].slave_addr);
    mmio_set(emu, EZ80_MMIO_REG_I2C1_XSAR, emu->hw.i2c[1].xslave_addr);
    mmio_set(emu, EZ80_MMIO_REG_I2C1_DATA,
             (emu->hw.i2c[1].rx_count > 0u) ? emu->hw.i2c[1].rx_buf[emu->hw.i2c[1].rx_head] : 0xFFu);
    /* UART MMIO sync */
    mmio_set(emu, EZ80_MMIO_REG_UART0_LCTL, emu->hw.uart[0].lctl);
    mmio_set(emu, EZ80_MMIO_REG_UART0_LSR, emu->hw.uart[0].lsr);
    mmio_set(emu, EZ80_MMIO_REG_UART0_IER, emu->hw.uart[0].ier);
    mmio_set(emu, EZ80_MMIO_REG_UART0_BRG_LO, (uint8_t)(emu->hw.uart[0].brg & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_UART0_BRG_HI, (uint8_t)((emu->hw.uart[0].brg >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_UART0_TX_COUNT_LO, (uint8_t)(emu->hw.uart[0].total_tx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_UART0_TX_COUNT_HI, (uint8_t)((emu->hw.uart[0].total_tx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_UART0_RX_COUNT_LO, (uint8_t)(emu->hw.uart[0].total_rx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_UART0_RX_COUNT_HI, (uint8_t)((emu->hw.uart[0].total_rx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_UART0_DATA,
             (emu->hw.uart[0].rx_count > 0u) ? emu->hw.uart[0].rx_fifo[emu->hw.uart[0].rx_head] : 0u);
    mmio_set(emu, EZ80_MMIO_REG_UART0_FCTL, emu->hw.uart[0].fctl);
    mmio_set(emu, EZ80_MMIO_REG_UART0_MCR, emu->hw.uart[0].mcr);
    mmio_set(emu, EZ80_MMIO_REG_UART0_SPR, emu->hw.uart[0].spr);
    mmio_set(emu, EZ80_MMIO_REG_UART1_LCTL, emu->hw.uart[1].lctl);
    mmio_set(emu, EZ80_MMIO_REG_UART1_LSR, emu->hw.uart[1].lsr);
    mmio_set(emu, EZ80_MMIO_REG_UART1_IER, emu->hw.uart[1].ier);
    mmio_set(emu, EZ80_MMIO_REG_UART1_BRG_LO, (uint8_t)(emu->hw.uart[1].brg & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_UART1_BRG_HI, (uint8_t)((emu->hw.uart[1].brg >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_UART1_TX_COUNT_LO, (uint8_t)(emu->hw.uart[1].total_tx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_UART1_TX_COUNT_HI, (uint8_t)((emu->hw.uart[1].total_tx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_UART1_RX_COUNT_LO, (uint8_t)(emu->hw.uart[1].total_rx_bytes & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_UART1_RX_COUNT_HI, (uint8_t)((emu->hw.uart[1].total_rx_bytes >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_UART1_DATA,
             (emu->hw.uart[1].rx_count > 0u) ? emu->hw.uart[1].rx_fifo[emu->hw.uart[1].rx_head] : 0u);
    mmio_set(emu, EZ80_MMIO_REG_UART1_FCTL, emu->hw.uart[1].fctl);
    mmio_set(emu, EZ80_MMIO_REG_UART1_MCR, emu->hw.uart[1].mcr);
    mmio_set(emu, EZ80_MMIO_REG_UART1_SPR, emu->hw.uart[1].spr);
    /* Trace MMIO sync */
    mmio_set(emu, EZ80_MMIO_REG_TRACE_ENABLED, emu->hw.trace_enabled ? 1u : 0u);
    mmio_set(emu, EZ80_MMIO_REG_TRACE_COUNT_LO, (uint8_t)(emu->hw.trace_count & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_TRACE_COUNT_HI, (uint8_t)((emu->hw.trace_count >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_TRACE_HEAD, (uint8_t)(emu->hw.trace_head & 0xFFu));
}

static void hardware_set_defaults(ez80_emulator_t *emu)
{
    uint8_t row;
    if (!emu) return;
    memset(&emu->hw, 0, sizeof(emu->hw));
    emu->hw.keypad_row_select = 0xFFu;
    emu->hw.kbd_group_select = 0xFFu;
    emu->hw.kbd_irq_on_keypress = true;
    emu->hw.kbd_last_state = 0xFFu;
    for (row = 0u; row < 8u; row++) {
        emu->hw.keypad_rows[row] = 0xFFu;
    }
    emu->hw.keypad_regs[EZ80_KEYPAD_REG_SCAN_MODE_0 + 0u] = 0xC0u;
    emu->hw.keypad_regs[EZ80_KEYPAD_REG_SCAN_MODE_0 + 1u] = 0x03u;
    emu->hw.keypad_regs[EZ80_KEYPAD_REG_SCAN_MODE_0 + 2u] = 0x00u;
    emu->hw.keypad_regs[EZ80_KEYPAD_REG_SCAN_MODE_0 + 3u] = 0x0Fu;
    emu->hw.keypad_regs[EZ80_KEYPAD_REG_SCAN_CFG_0 + 0u] = 0x08u;
    emu->hw.keypad_regs[EZ80_KEYPAD_REG_SCAN_CFG_0 + 1u] = 0x08u;
    emu->hw.lcd_mode = 0x00u;
    emu->hw.lcd_timing = 0x00u;
    emu->hw.lcd_window_x0 = 0u;
    emu->hw.lcd_window_y0 = 0u;
    emu->hw.lcd_window_x1 = (uint16_t)(EZ80_GRAPH_WIDTH - 1u);
    emu->hw.lcd_window_y1 = (uint16_t)(EZ80_GRAPH_HEIGHT - 1u);
    emu->hw.lcd_vram_base = EZ80_LCD_RAM_BASE;
    emu->hw.lcd_pipeline_state = EZ80_LCD_PIPELINE_IDLE;
    emu->hw.lcd_status = 0u;
    emu->hw.usb_control = 0u;
    emu->hw.usb_status = 0u;
    emu->hw.rtc_seconds_of_day = 12u * 60u * 60u;
    emu->hw.rtc_alarm_seconds = 0u;
    emu->hw.rtc_alarm_minutes = 0u;
    emu->hw.rtc_alarm_hours = 0u;
    emu->hw.rtc_alarm_control = 0u;
    emu->hw.rtc_alarm_fired = false;
    emu->hw.rtc_alarm_fire_count = 0u;
    emu->hw.wdt_control = 0u;
    emu->hw.wdt_counter = 0u;
    emu->hw.wdt_timeout = EZ80_WDT_DEFAULT_TIMEOUT;
    emu->hw.wdt_reset_count = 0u;
    emu->hw.wdt_last_reset_cycle = 0u;
    emu->hw.wdt_prescaler_accum = 0u;
    emu->hw.pm_mode = EZ80_PM_MODE_ACTIVE;
    emu->hw.pm_wake_source = EZ80_PM_WAKE_NONE;
    emu->hw.pm_sleep_count = 0u;
    emu->hw.pm_total_sleep_cycles = 0u;
    emu->hw.total_cycles = 0u;
    /* SPI defaults: disabled, TX empty */
    {
        uint8_t ch;
        for (ch = 0u; ch < EZ80_SPI_CHANNEL_COUNT; ch++) {
            emu->hw.spi[ch].status = EZ80_SPI_STATUS_TX_EMPTY;
            emu->hw.spi[ch].baud_divisor = 0x02u; /* default baud divisor */
        }
    }
    /* I2C defaults: idle */
    {
        uint8_t ch;
        for (ch = 0u; ch < EZ80_I2C_CHANNEL_COUNT; ch++) {
            emu->hw.i2c[ch].status = EZ80_I2C_SR_IDLE;
        }
    }
    /* UART defaults: TX empty */
    {
        uint8_t ch;
        for (ch = 0u; ch < EZ80_UART_CHANNEL_COUNT; ch++) {
            emu->hw.uart[ch].lsr = EZ80_UART_LSR_TEMT | EZ80_UART_LSR_THRE;
            emu->hw.uart[ch].brg = 1u; /* avoid div-by-zero */
            emu->hw.uart[ch].lctl = 0x03u; /* 8-bit, no parity, 1 stop */
        }
    }
    /* GPIO defaults: all input, power on, battery full */
    emu->hw.backlight_on = true;
    emu->hw.battery_level = 100u;
    emu->hw.power_state = 1u; /* on */
    /* Set default GPIO pin states for TI-84 CE */
    emu->hw.gpio[EZ80_GPIO_BACKLIGHT_PORT].output_latch = EZ80_GPIO_BACKLIGHT_BIT;
    emu->hw.gpio[EZ80_GPIO_BACKLIGHT_PORT].data = EZ80_GPIO_BACKLIGHT_BIT; /* backlight on */
    emu->hw.gpio[EZ80_GPIO_BACKLIGHT_PORT].direction = EZ80_GPIO_BACKLIGHT_BIT; /* output */
    emu->hw.gpio[EZ80_GPIO_BACKLIGHT_PORT].last_sampled = EZ80_GPIO_BACKLIGHT_BIT;
    emu->hw.gpio[EZ80_GPIO_POWER_PORT].data = 0u; /* no power button pressed, not charging */
    emu->hw.gpio[EZ80_GPIO_POWER_PORT].last_sampled = 0u;
    emu->hw.gpio[EZ80_GPIO_USB_PORT].last_sampled = 0u;
    usb_set_connected_state(emu, true);
    gpio_sync_external_inputs(emu);
    io_install_default_handlers(emu);
    keypad_regs_sync(emu);
    emu->hw.kbd_last_state = keyboard_compute_group_data(emu);
    emu->hw.flash_boot_nmi_status = 0x01u;
    mmio_set(emu, EZ80_MMIO_REG_KEY_STATUS, emu->hw.kbd_last_state);
    lcd_refresh_status(emu);
    hardware_sync_ports(emu);
    hardware_sync_mmio(emu);
}

static void os_set_defaults(ez80_emulator_t *emu)
{
    if (!emu) return;
    memset(&emu->os, 0, sizeof(emu->os));
    emu->os.ram_pool_next = 0u;
    emu->os.archive_pool_next = 0u;
    emu->os.home_row = 0u;
    emu->os.home_col = 0u;
    emu->os.last_bcall_status = (uint8_t)EZ80_OK;
    emu->os.last_swap_failure = EZ80_OS_SWAP_FAILURE_NONE;
    os_clear_buffers();
    os_sync_mmio(emu);
}

static bool version_at_least(uint8_t major,
                             uint8_t minor,
                             uint8_t threshold_major,
                             uint8_t threshold_minor)
{
    if (major > threshold_major) return true;
    if (major < threshold_major) return false;
    return minor >= threshold_minor;
}

static bool platform_os_is_locked(const ez80_emulator_t *emu)
{
    if (!emu) return false;
    return version_at_least(emu->platform.os_major, emu->platform.os_minor, 5u, 5u);
}

static bool platform_os_is_58_or_newer(const ez80_emulator_t *emu)
{
    if (!emu) return false;
    return version_at_least(emu->platform.os_major, emu->platform.os_minor, 5u, 8u);
}

static uint8_t flash_sector_index(uint32_t flash_addr)
{
    return (uint8_t)((mask24(flash_addr) / EZ80_FLASH_SECTOR_BYTES) % EZ80_FLASH_SECTOR_COUNT);
}

static bool flash_sector_os_locked(const ez80_emulator_t *emu, uint8_t sector)
{
    if (sector <= EZ80_FLASH_BOOT_PROTECTED_LAST_PAGE) {
        return true;
    }
    if (platform_os_is_58_or_newer(emu) &&
        (sector == EZ80_FLASH_CERT_PAGE || sector == EZ80_FLASH_CERT_BACKUP_PAGE)) {
        return true;
    }
    return false;
}

static void flash_record_writeflash(ez80_emulator_t *emu,
                                    uint8_t status,
                                    uint32_t addr,
                                    uint8_t requested,
                                    uint8_t before,
                                    uint8_t after)
{
    if (!emu) return;
    emu->platform.writeflash_last_status = status;
    emu->platform.writeflash_last_addr = mask24(addr);
    emu->platform.writeflash_last_requested = requested;
    emu->platform.writeflash_last_before = before;
    emu->platform.writeflash_last_after = after;
    mmio_set(emu, EZ80_MMIO_REG_WRITEFLASH_STATUS, status);
    mmio_set24(emu, EZ80_MMIO_REG_WRITEFLASH_ADDR_0, addr);
    mmio_set(emu, EZ80_MMIO_REG_WRITEFLASH_REQUESTED, requested);
    mmio_set(emu, EZ80_MMIO_REG_WRITEFLASH_BEFORE, before);
    mmio_set(emu, EZ80_MMIO_REG_WRITEFLASH_AFTER, after);
}

static void flash_sync_protection_mmio(ez80_emulator_t *emu)
{
    uint8_t sector;
    uint8_t flags;

    if (!emu) return;
    sector = (uint8_t)(emu->platform.flash_sector_select % EZ80_FLASH_SECTOR_COUNT);
    flags = 0u;
    if (emu->platform.flash_sector_protected[sector]) {
        flags |= EZ80_FLASH_SECTOR_FLAG_PROTECTED;
    }
    if (flash_sector_os_locked(emu, sector)) {
        flags |= EZ80_FLASH_SECTOR_FLAG_OS_LOCKED;
    }
    mmio_set(emu, EZ80_MMIO_REG_FLASH_SECTOR_SELECT, sector);
    mmio_set(emu, EZ80_MMIO_REG_FLASH_SECTOR_FLAGS, flags);
    mmio_set(emu, EZ80_MMIO_REG_FLASH_PROTECT_MODE, emu->platform.flash_protect_mode);
}

static void flash_reset_protection(ez80_emulator_t *emu)
{
    size_t i;

    if (!emu) return;
    memset(emu->platform.flash_sector_protected, 0, sizeof(emu->platform.flash_sector_protected));
    for (i = 0u; i < EZ80_FLASH_SECTOR_COUNT; i++) {
        if (i <= EZ80_FLASH_BOOT_PROTECTED_LAST_PAGE) {
            emu->platform.flash_sector_protected[i] = 1u;
        }
    }
    if (platform_os_is_58_or_newer(emu)) {
        emu->platform.flash_protect_mode = EZ80_FLASH_PROTECT_MODE_LOCKED;
        emu->platform.flash_sector_protected[EZ80_FLASH_CERT_PAGE] = 1u;
        emu->platform.flash_sector_protected[EZ80_FLASH_CERT_BACKUP_PAGE] = 1u;
    } else {
        emu->platform.flash_protect_mode = EZ80_FLASH_PROTECT_MODE_PERMISSIVE;
    }
    emu->platform.flash_sector_select = 0u;
    flash_sync_protection_mmio(emu);
}

static void platform_set_dngrade_failure(ez80_emulator_t *emu, uint8_t reason, uint8_t port)
{
    if (!emu) return;
    emu->platform.dngrade_last_failure = reason;
    emu->platform.dngrade_last_port = port;
    mmio_set(emu, EZ80_MMIO_REG_DNGRADE_LAST_FAILURE_REASON, reason);
    mmio_set(emu, EZ80_MMIO_REG_DNGRADE_LAST_FAILURE_PORT, port);
}

static void platform_record_dngrade_attempt(ez80_emulator_t *emu,
                                            const ez80_dngrade_attempt_result_t *attempt)
{
    if (!emu || !attempt) return;
    emu->platform.dngrade_last_strategy = attempt->strategy;
    emu->platform.dngrade_last_attempt_status = attempt->attempt_status;
    emu->platform.dngrade_last_timeout = attempt->timed_out;
    emu->platform.dngrade_last_watchdog = attempt->watchdog_reset;
    emu->platform.dngrade_last_write_ok = attempt->write_succeeded;
    emu->platform.dngrade_last_cycles = attempt->cycles_executed;
    platform_set_dngrade_failure(emu, attempt->failure_reason, attempt->failure_port);
    platform_sync_mmio(emu);
}

static void platform_sync_mmio(ez80_emulator_t *emu)
{
    if (!emu) return;
    mmio_set(emu, EZ80_MMIO_REG_OS_VERSION_PATCH, emu->platform.os_patch);
    mmio_set(emu, EZ80_MMIO_REG_OS_VERSION_MINOR, emu->platform.os_minor);
    mmio_set(emu, EZ80_MMIO_REG_OS_VERSION_MAJOR, emu->platform.os_major);
    mmio_set(emu,
             EZ80_MMIO_REG_NATIVE_UNSIGNED_EXEC_ALLOWED,
             emu->platform.native_unsigned_execution_allowed ? 1u : 0u);
    mmio_set(emu,
             EZ80_MMIO_REG_ARTIFICE_BYPASS_ACTIVE,
             emu->platform.artifice_bypass_active ? 1u : 0u);
    mmio_set(emu, EZ80_MMIO_REG_ARTIFICE_ENTRY_METHOD, emu->platform.artifice_entry_method);
    mmio_set(emu,
             EZ80_MMIO_REG_DNGRADE_PORT28_BIT3_BLOCKED,
             emu->platform.dngrade_port28_bit3_blocked ? 1u : 0u);
    mmio_set(emu, EZ80_MMIO_REG_DNGRADE_LAST_FAILURE_REASON, emu->platform.dngrade_last_failure);
    mmio_set(emu, EZ80_MMIO_REG_DNGRADE_LAST_FAILURE_PORT, emu->platform.dngrade_last_port);
    mmio_set(emu, EZ80_MMIO_REG_DNGRADE_LAST_STRATEGY, emu->platform.dngrade_last_strategy);
    mmio_set(emu, EZ80_MMIO_REG_DNGRADE_LAST_ATTEMPT_STATUS, emu->platform.dngrade_last_attempt_status);
    mmio_set(emu,
             EZ80_MMIO_REG_DNGRADE_LAST_TIMEOUT,
             emu->platform.dngrade_last_timeout ? 1u : 0u);
    mmio_set(emu,
             EZ80_MMIO_REG_DNGRADE_LAST_WATCHDOG,
             emu->platform.dngrade_last_watchdog ? 1u : 0u);
    mmio_set(emu,
             EZ80_MMIO_REG_DNGRADE_LAST_WRITE_OK,
             emu->platform.dngrade_last_write_ok ? 1u : 0u);
    mmio_set24(emu, EZ80_MMIO_REG_DNGRADE_LAST_CYCLES_0, emu->platform.dngrade_last_cycles);
    flash_sync_protection_mmio(emu);
    flash_record_writeflash(emu,
                            emu->platform.writeflash_last_status,
                            emu->platform.writeflash_last_addr,
                            emu->platform.writeflash_last_requested,
                            emu->platform.writeflash_last_before,
                            emu->platform.writeflash_last_after);
}

static void platform_recompute_state(ez80_emulator_t *emu)
{
    bool locked;

    if (!emu) return;
    locked = platform_os_is_locked(emu);
    emu->platform.native_unsigned_execution_allowed = (!locked) || emu->platform.artifice_bypass_active;
    emu->platform.dngrade_port28_bit3_blocked = platform_os_is_58_or_newer(emu);
    platform_sync_mmio(emu);
}

static void platform_set_defaults(ez80_emulator_t *emu)
{
    if (!emu) return;
    memset(&emu->platform, 0, sizeof(emu->platform));
    emu->platform.os_major = 5u;
    emu->platform.os_minor = 4u;
    emu->platform.os_patch = 0u;
    emu->platform.os_version_detected = false;
    emu->platform.artifice_bypass_active = false;
    emu->platform.artifice_entry_method = EZ80_ARTIFICE_ENTRY_NONE;
    emu->platform.dngrade_last_failure = EZ80_DNGRADE_FAILURE_NONE;
    emu->platform.dngrade_last_port = 0u;
    emu->platform.dngrade_last_strategy = EZ80_DNGRADE_STRATEGY_NONE;
    emu->platform.dngrade_last_attempt_status = EZ80_DNGRADE_ATTEMPT_STATUS_NONE;
    emu->platform.dngrade_last_timeout = false;
    emu->platform.dngrade_last_watchdog = false;
    emu->platform.dngrade_last_write_ok = false;
    emu->platform.dngrade_last_cycles = 0u;
    emu->platform.writeflash_last_status = EZ80_WRITEFLASH_STATUS_NONE;
    emu->platform.writeflash_last_addr = 0u;
    emu->platform.writeflash_last_requested = 0u;
    emu->platform.writeflash_last_before = 0u;
    emu->platform.writeflash_last_after = 0u;
    flash_reset_protection(emu);
    platform_recompute_state(emu);
    mmio_set(emu, EZ80_MMIO_REG_ARTIFICE_LAST_KEY, 0u);
}

static void platform_detect_os_version_from_loaded_rom(ez80_emulator_t *emu)
{
    uint8_t patch;
    uint8_t minor;
    uint8_t major;
    uint32_t offset = EZ80_OS_VERSION_FLASH_OFFSET;

    if (!emu) return;

    emu->platform.os_version_detected = false;
    emu->platform.artifice_bypass_active = false;
    emu->platform.artifice_entry_method = EZ80_ARTIFICE_ENTRY_NONE;
    emu->platform.artifice_key_window_count = 0u;
    memset(emu->platform.artifice_key_window, 0, sizeof(emu->platform.artifice_key_window));

    if (!emu->rom_loaded || emu->memory.flash_bytes_loaded <= offset + 2u) {
        platform_recompute_state(emu);
        return;
    }

    patch = g_flash_image[offset + 0u];
    minor = g_flash_image[offset + 1u];
    major = g_flash_image[offset + 2u];

    if (major >= 1u && major <= 9u && minor <= 99u) {
        emu->platform.os_patch = patch;
        emu->platform.os_minor = minor;
        emu->platform.os_major = major;
        emu->platform.os_version_detected = true;
    }

    flash_reset_protection(emu);
    platform_recompute_state(emu);
}

static bool platform_matches_port28_unlock_sequence(const ez80_emulator_t *emu)
{
    static const uint8_t expected[] = {
        0xF3u,
        0x18u, 0x00u,
        0xF3u,
        0xEDu, 0x7Eu,
        0xEDu, 0x56u,
        0xEDu, 0x39u, 0x28u,
        0xEDu, 0x38u, 0x28u,
        0xCBu, 0x57u
    };
    uint32_t pc;
    uint32_t start;
    size_t i;

    if (!emu) return false;
    if (!emu->cpu_instruction_port_access_active) return false;
    pc = mask24(emu->cpu_instruction_port_access_pc);
    if (pc < 8u || pc >= 0x020000u) return false;
    start = pc - 8u;
    if (!emu->rom_loaded) return false;
    if ((start + sizeof(expected)) > emu->memory.flash_bytes_loaded) return false;

    for (i = 0u; i < sizeof(expected); i++) {
        if (flash_read_byte(emu, start + (uint32_t)i) != expected[i]) {
            return false;
        }
    }

    return true;
}

static bool platform_should_force_port28_bit3_unlock(const ez80_emulator_t *emu, uint8_t value)
{
    if (!emu) return false;
    if ((value & 0x04u) == 0u) return false;
    return platform_matches_port28_unlock_sequence(emu);
}

static bool platform_should_block_port28_bit3_write(const ez80_emulator_t *emu, uint8_t value)
{
    if (!emu) return false;
    if (!emu->platform.dngrade_port28_bit3_blocked) return false;
    if (platform_should_force_port28_bit3_unlock(emu, value)) return false;
    return (value & EZ80_FLASH_UNLOCK_OS58_MASK) != 0u;
}

static bool platform_matches_flash_boot_zero_status_sequence(const ez80_emulator_t *emu, uint8_t value)
{
    static const uint8_t expected[] = {
        0xF3u,
        0x3Eu, 0x8Cu,
        0xEDu, 0x39u, 0x24u,
        0xFEu, 0x8Cu,
        0xC2u, 0x66u, 0x00u, 0x00u
    };
    uint32_t pc;
    uint32_t start;
    size_t i;

    if (!emu) return false;
    if (value != 0x8Cu) return false;
    if (!emu->cpu_instruction_port_access_active) return false;
    pc = mask24(emu->cpu_instruction_port_access_pc);
    if (pc < 3u || pc >= 0x020000u) return false;
    start = pc - 3u;
    if (!emu->rom_loaded) return false;
    if ((start + sizeof(expected)) > emu->memory.flash_bytes_loaded) return false;

    for (i = 0u; i < sizeof(expected); i++) {
        if (flash_read_byte(emu, start + (uint32_t)i) != expected[i]) {
            return false;
        }
    }

    return true;
}

static bool platform_matches_boot_irq_halt_sequence(const ez80_emulator_t *emu, uint8_t value)
{
    static const uint8_t expected[] = {
        0xF3u,
        0x3Eu, 0x10u,
        0xEDu, 0x39u, 0x00u,
        0x00u,
        0x00u,
        0x76u,
        0x40u,
        0x01u, 0x15u, 0x50u,
        0xEDu, 0x78u
    };
    uint32_t pc;
    uint32_t start;
    size_t i;

    if (!emu) return false;
    if (value != 0x10u) return false;
    if (!emu->cpu_instruction_port_access_active) return false;
    if (!emu->rom_loaded) return false;

    pc = mask24(emu->cpu_instruction_port_access_pc);
    if (pc < 3u || pc >= 0x020000u) return false;

    start = pc - 3u;
    if ((start + sizeof(expected)) > emu->memory.flash_bytes_loaded) return false;

    for (i = 0u; i < sizeof(expected); i++) {
        if (flash_read_byte(emu, start + (uint32_t)i) != expected[i]) {
            return false;
        }
    }

    return true;
}

static void boot_irq_overlay_arm(ez80_emulator_t *emu)
{
    if (!emu) return;

    emu->hw.boot_irq_overlay_active = true;
    emu->hw.boot_irq_halt_release_pending = true;
    emu->hw.boot_irq_status14 = 0u;
    emu->hw.boot_irq_status15 = 0u;
    emu->hw.boot_irq_status16 = 0u;

    port_shadow_write(emu, 0x5014u, emu->hw.boot_irq_status14);
    port_shadow_write(emu, 0x5015u, emu->hw.boot_irq_status15);
    port_shadow_write(emu, 0x5016u, emu->hw.boot_irq_status16);
}

static void boot_irq_overlay_clear(ez80_emulator_t *emu)
{
    if (!emu) return;
    emu->hw.boot_irq_overlay_active = false;
    emu->hw.boot_irq_halt_release_pending = false;
}

static bool boot_irq_overlay_port(uint16_t port)
{
    switch (port) {
    case 0x5006u:
    case 0x5008u:
    case 0x5009u:
    case 0x500Au:
    case 0x5014u:
    case 0x5015u:
    case 0x5016u:
        return true;
    default:
        return false;
    }
}

static bool boot_irq_overlay_read(const ez80_emulator_t *emu,
                                  uint16_t port,
                                  uint8_t *out_value)
{
    if (!emu || !out_value) return false;
    if (!emu->hw.boot_irq_overlay_active) return false;
    if (!boot_irq_overlay_port(port)) return false;

    switch (port) {
    case 0x5014u:
        *out_value = emu->hw.boot_irq_status14;
        return true;
    case 0x5015u:
        *out_value = emu->hw.boot_irq_status15;
        return true;
    case 0x5016u:
        *out_value = emu->hw.boot_irq_status16;
        return true;
    default:
        *out_value = port_shadow_read(emu, port);
        return true;
    }
}

static bool boot_irq_overlay_write(ez80_emulator_t *emu,
                                   uint16_t port,
                                   uint8_t value)
{
    if (!emu) return false;
    if (!emu->hw.boot_irq_overlay_active) return false;
    if (!boot_irq_overlay_port(port)) return false;

    switch (port) {
    case 0x5014u:
        emu->hw.boot_irq_status14 = value;
        break;
    case 0x5015u:
        emu->hw.boot_irq_status15 = value;
        break;
    case 0x5016u:
        emu->hw.boot_irq_status16 = value;
        break;
    default:
        break;
    }

    port_shadow_write(emu, port, value);

    if (port == 0x5006u || port == 0x5008u || port == 0x5009u || port == 0x500Au) {
        boot_irq_overlay_clear(emu);
    }

    return true;
}

static bool boot_irq_overlay_release_halt(ez80_emulator_t *emu)
{
    if (!emu) return false;
    if (!emu->hw.boot_irq_halt_release_pending) return false;
    if (!emu->cpu.halted) return false;

    emu->hw.boot_irq_halt_release_pending = false;
    emu->cpu.halted = false;
    if (!emu->hw.halt_pc_advanced) {
        emu->cpu.pc = mask24(emu->cpu.pc + 1u);
    }
    emu->hw.halt_pc_advanced = false;
    return true;
}

static void platform_activate_artifice(ez80_emulator_t *emu, uint8_t entry_method)
{
    if (!emu) return;
    emu->platform.artifice_bypass_active = true;
    emu->platform.artifice_entry_method = entry_method;
    platform_recompute_state(emu);
}

static bool platform_artifice_sequence_match(const ez80_platform_state_t *platform)
{
    static const uint8_t expected[EZ80_ARTIFICE_KEY_SEQ_LEN] = {
        EZ80_ARTIFICE_KEY_0,
        EZ80_ARTIFICE_KEY_1,
        EZ80_ARTIFICE_KEY_2,
        EZ80_ARTIFICE_KEY_3
    };

    if (!platform) return false;
    if (platform->artifice_key_window_count < EZ80_ARTIFICE_KEY_SEQ_LEN) return false;
    return memcmp(platform->artifice_key_window, expected, EZ80_ARTIFICE_KEY_SEQ_LEN) == 0;
}

static void check_watchpoints(ez80_emulator_t *emu, uint32_t addr, uint8_t access_type)
{
    uint32_t i;
    if (!emu || emu->watchpoint_count == 0u) return;
    addr = mask24(addr);
    for (i = 0u; i < emu->watchpoint_count; i++) {
        if ((emu->watchpoints[i].type & access_type) != 0u &&
            addr >= emu->watchpoints[i].addr &&
            addr < emu->watchpoints[i].addr + emu->watchpoints[i].length) {
            emu->watchpoint_triggered = true;
            emu->watchpoint_hit_addr = addr;
            emu->watchpoint_hit_type = access_type;
            emu->memory.mmio[EZ80_MMIO_REG_LAST_WATCHPOINT_ADDR_0] = (uint8_t)(addr & 0xFFu);
            emu->memory.mmio[EZ80_MMIO_REG_LAST_WATCHPOINT_ADDR_1] = (uint8_t)((addr >> 8) & 0xFFu);
            emu->memory.mmio[EZ80_MMIO_REG_LAST_WATCHPOINT_ADDR_2] = (uint8_t)((addr >> 16) & 0xFFu);
            emu->memory.mmio[EZ80_MMIO_REG_LAST_WATCHPOINT_TYPE] = access_type;
            return;
        }
    }
}

static void hardware_raise_irq(ez80_emulator_t *emu, uint8_t source)
{
    if (!emu) return;
    emu->hw.irq_pending |= source;
    pm_note_wake_event(emu, pm_wake_source_for_irq_source(source), false);
}

static void flash_boot_maybe_raise_nmi(ez80_emulator_t *emu)
{
    uint8_t status;

    if (!emu) return;
    if (!emu->hw.flash_boot_nmi_armed) return;
    if (emu->hw.nmi_pending) return;

    emu->hw.flash_boot_nmi_armed = false;
    status = port_shadow_read(emu, 0x3Du);
    status = (uint8_t)((status & (uint8_t)~0x03u) | (emu->hw.flash_boot_nmi_status & 0x03u));
    port_shadow_write(emu, 0x3Du, status);
    emu->hw.flash_boot_nmi_status = 0x01u;
    (void)ez80_emulator_raise_nmi(emu);
}

static void lcd_pipeline_write_data(ez80_emulator_t *emu, uint8_t value)
{
    uint16_t color;
    uint32_t target_pixel;
    uint32_t target_addr;
    uint32_t stream_count;

    if (!emu) return;
    emu->hw.lcd_last_data = value;

    if (!emu->hw.lcd_pending_low_byte) {
        emu->hw.lcd_low_byte = value;
        emu->hw.lcd_pending_low_byte = true;
        emu->hw.lcd_pipeline_state = EZ80_LCD_PIPELINE_WAIT_HIGH;
        lcd_refresh_status(emu);
        return;
    }

    color = (uint16_t)emu->hw.lcd_low_byte | ((uint16_t)value << 8);
    target_pixel = lcd_stream_target_pixel(&emu->hw, emu->hw.lcd_write_index);
    target_addr = mask24(emu->hw.lcd_vram_base + (target_pixel * 2u));
    if (lcd_addr_in_range(target_addr) && lcd_addr_in_range(mask24(target_addr + 1u))) {
        lcd_write_byte(emu, target_addr, (uint8_t)(color & 0xFFu));
        lcd_write_byte(emu, mask24(target_addr + 1u), (uint8_t)((color >> 8) & 0xFFu));
    } else {
        g_lcd_framebuffer[target_pixel % lcd_panel_pixel_count()] = color;
        emu->hw.lcd_status |= EZ80_LCD_STATUS_DATA_READY;
    }

    stream_count = lcd_stream_pixel_count(&emu->hw);
    emu->hw.lcd_write_index = (emu->hw.lcd_write_index + 1u) % stream_count;
    emu->hw.lcd_pending_low_byte = false;
    emu->hw.lcd_pipeline_state = EZ80_LCD_PIPELINE_PIXEL_COMMITTED;
    emu->hw.lcd_status |= EZ80_LCD_STATUS_DATA_READY;
    lcd_refresh_status(emu);
}

static uint8_t lcd_port_reg_read(const ez80_emulator_t *emu, uint8_t reg)
{
    if (!emu) return 0u;
    switch (reg) {
    case EZ80_PORT_LCD_REG_MODE: return emu->hw.lcd_mode;
    case EZ80_PORT_LCD_REG_TIMING: return emu->hw.lcd_timing;
    case EZ80_PORT_LCD_REG_WINDOW_X0_LO: return (uint8_t)(emu->hw.lcd_window_x0 & 0xFFu);
    case EZ80_PORT_LCD_REG_WINDOW_X0_HI: return (uint8_t)((emu->hw.lcd_window_x0 >> 8) & 0xFFu);
    case EZ80_PORT_LCD_REG_WINDOW_Y0_LO: return (uint8_t)(emu->hw.lcd_window_y0 & 0xFFu);
    case EZ80_PORT_LCD_REG_WINDOW_Y0_HI: return (uint8_t)((emu->hw.lcd_window_y0 >> 8) & 0xFFu);
    case EZ80_PORT_LCD_REG_WINDOW_X1_LO: return (uint8_t)(emu->hw.lcd_window_x1 & 0xFFu);
    case EZ80_PORT_LCD_REG_WINDOW_X1_HI: return (uint8_t)((emu->hw.lcd_window_x1 >> 8) & 0xFFu);
    case EZ80_PORT_LCD_REG_WINDOW_Y1_LO: return (uint8_t)(emu->hw.lcd_window_y1 & 0xFFu);
    case EZ80_PORT_LCD_REG_WINDOW_Y1_HI: return (uint8_t)((emu->hw.lcd_window_y1 >> 8) & 0xFFu);
    case EZ80_PORT_LCD_REG_VRAM_BASE_0: return (uint8_t)(emu->hw.lcd_vram_base & 0xFFu);
    case EZ80_PORT_LCD_REG_VRAM_BASE_1: return (uint8_t)((emu->hw.lcd_vram_base >> 8) & 0xFFu);
    case EZ80_PORT_LCD_REG_VRAM_BASE_2: return (uint8_t)((emu->hw.lcd_vram_base >> 16) & 0xFFu);
    case EZ80_PORT_LCD_REG_STATUS: return emu->hw.lcd_status;
    case EZ80_PORT_LCD_REG_PIPELINE: return emu->hw.lcd_pipeline_state;
    case EZ80_PORT_LCD_REG_DATA: return emu->hw.lcd_last_data;
    default: return 0u;
    }
}

static void lcd_port_reg_write(ez80_emulator_t *emu, uint8_t reg, uint8_t value)
{
    uint32_t base;
    if (!emu) return;
    switch (reg) {
    case EZ80_PORT_LCD_REG_MODE:
        emu->hw.lcd_mode = value;
        if ((value & 0x02u) != 0u) {
            memset(g_lcd_framebuffer, 0, sizeof(g_lcd_framebuffer));
            lcd_set_full_window(&emu->hw);
            lcd_reset_stream_state(&emu->hw);
            lcd_reset_command_state(&emu->hw);
            emu->hw.lcd_status |= EZ80_LCD_STATUS_DATA_READY;
        }
        break;
    case EZ80_PORT_LCD_REG_TIMING:
        emu->hw.lcd_timing = value;
        break;
    case EZ80_PORT_LCD_REG_WINDOW_X0_LO:
        emu->hw.lcd_window_x0 = (uint16_t)((emu->hw.lcd_window_x0 & 0xFF00u) | value);
        break;
    case EZ80_PORT_LCD_REG_WINDOW_X0_HI:
        emu->hw.lcd_window_x0 = (uint16_t)((emu->hw.lcd_window_x0 & 0x00FFu) | ((uint16_t)value << 8));
        break;
    case EZ80_PORT_LCD_REG_WINDOW_Y0_LO:
        emu->hw.lcd_window_y0 = (uint16_t)((emu->hw.lcd_window_y0 & 0xFF00u) | value);
        break;
    case EZ80_PORT_LCD_REG_WINDOW_Y0_HI:
        emu->hw.lcd_window_y0 = (uint16_t)((emu->hw.lcd_window_y0 & 0x00FFu) | ((uint16_t)value << 8));
        break;
    case EZ80_PORT_LCD_REG_WINDOW_X1_LO:
        emu->hw.lcd_window_x1 = (uint16_t)((emu->hw.lcd_window_x1 & 0xFF00u) | value);
        break;
    case EZ80_PORT_LCD_REG_WINDOW_X1_HI:
        emu->hw.lcd_window_x1 = (uint16_t)((emu->hw.lcd_window_x1 & 0x00FFu) | ((uint16_t)value << 8));
        break;
    case EZ80_PORT_LCD_REG_WINDOW_Y1_LO:
        emu->hw.lcd_window_y1 = (uint16_t)((emu->hw.lcd_window_y1 & 0xFF00u) | value);
        break;
    case EZ80_PORT_LCD_REG_WINDOW_Y1_HI:
        emu->hw.lcd_window_y1 = (uint16_t)((emu->hw.lcd_window_y1 & 0x00FFu) | ((uint16_t)value << 8));
        break;
    case EZ80_PORT_LCD_REG_VRAM_BASE_0:
        base = emu->hw.lcd_vram_base & 0xFFFF00u;
        emu->hw.lcd_vram_base = mask24(base | (uint32_t)value);
        break;
    case EZ80_PORT_LCD_REG_VRAM_BASE_1:
        base = emu->hw.lcd_vram_base & 0xFF00FFu;
        emu->hw.lcd_vram_base = mask24(base | ((uint32_t)value << 8));
        break;
    case EZ80_PORT_LCD_REG_VRAM_BASE_2:
        base = emu->hw.lcd_vram_base & 0x00FFFFu;
        emu->hw.lcd_vram_base = mask24(base | ((uint32_t)value << 16));
        break;
    case EZ80_PORT_LCD_REG_STATUS:
        emu->hw.lcd_status = (uint8_t)(emu->hw.lcd_status & (uint8_t)~value);
        break;
    case EZ80_PORT_LCD_REG_PIPELINE:
        emu->hw.lcd_pipeline_state = (uint8_t)(value & 0x03u);
        if (emu->hw.lcd_pipeline_state == EZ80_LCD_PIPELINE_IDLE) {
            emu->hw.lcd_pending_low_byte = false;
        }
        break;
    case EZ80_PORT_LCD_REG_DATA:
        lcd_pipeline_write_data(emu, value);
        return;
    default:
        break;
    }
    lcd_normalize_write_index(&emu->hw);
    lcd_refresh_status(emu);
}

static ez80_status_t port_write_legacy_low(ez80_emulator_t *emu, uint8_t port, uint8_t value)
{
    bool force_unlock_bit3 = false;
    if (!emu) return EZ80_ERR_INVALID_ARG;

    if (port == 0x00u && platform_matches_boot_irq_halt_sequence(emu, value)) {
        boot_irq_overlay_arm(emu);
    }

    if (port == EZ80_FLASH_UNLOCK_PORT) {
        force_unlock_bit3 = platform_should_force_port28_bit3_unlock(emu, value);
        if (force_unlock_bit3) {
            /* OS 5.8 boot only observes bit3 during the immediate probe readback. */
            emu->platform.port28_unlock_read_latch = 1u;
        } else if (platform_should_block_port28_bit3_write(emu, value)) {
            value &= (uint8_t)(~EZ80_FLASH_UNLOCK_OS58_MASK);
            platform_set_dngrade_failure(emu,
                                         EZ80_DNGRADE_FAILURE_PORT28_BIT3_BLOCKED,
                                         EZ80_FLASH_UNLOCK_PORT);
            emu->platform.port28_unlock_read_latch = 0u;
        } else {
            emu->platform.port28_unlock_read_latch = 0u;
        }
    }

    port_shadow_write(emu, port, value);
    if (port == 0x01u) {
        mmio_set(emu, EZ80_MMIO_REG_KEY_STATUS, value);
    }
    if (port == 0x02u) {
        mmio_set(emu, EZ80_MMIO_REG_LCD_STATUS, value);
    }
    if (port == EZ80_FLASH_UNLOCK_PORT) {
        mmio_set(emu,
                 EZ80_MMIO_REG_FLASH_UNLOCK_STATE,
                 ((value & EZ80_FLASH_UNLOCK_MASK) != 0u) ? 1u : 0u);
    } else if (port == EZ80_FLASH_PROTECT_PORT) {
        mmio_set(emu,
                 EZ80_MMIO_REG_FLASH_CERT_WRITE_STATE,
                 ((value & EZ80_FLASH_CERT_WRITE_MASK) != 0u) ? 1u : 0u);
    } else if (port == EZ80_FLASH_SECTOR_INDEX_PORT) {
        emu->platform.flash_sector_select = (uint8_t)(value % EZ80_FLASH_SECTOR_COUNT);
        if ((value & 0x80u) != 0u) {
            emu->hw.flash_boot_nmi_armed = true;
            if (platform_matches_flash_boot_zero_status_sequence(emu, value)) {
                /* The traced ROM boot loop exits only when this 0x8C command
                 * completes with a zero-status boot NMI at port 0x3D. */
                emu->hw.flash_boot_nmi_status = 0x00u;
            } else {
                emu->hw.flash_boot_nmi_status = 0x01u;
            }
        }
        flash_sync_protection_mmio(emu);
    } else if (port == EZ80_FLASH_SECTOR_CONTROL_PORT) {
        uint8_t sector = (uint8_t)(emu->platform.flash_sector_select % EZ80_FLASH_SECTOR_COUNT);
        if ((value & EZ80_FLASH_SECTOR_CMD_PROTECT) != 0u) {
            emu->platform.flash_sector_protected[sector] = 1u;
        }
        if ((value & EZ80_FLASH_SECTOR_CMD_UNPROTECT) != 0u) {
            if (!flash_sector_os_locked(emu, sector)) {
                emu->platform.flash_sector_protected[sector] = 0u;
            } else {
                platform_set_dngrade_failure(emu,
                                             EZ80_DNGRADE_FAILURE_FLASH_SECTOR_PROTECTED,
                                             EZ80_FLASH_SECTOR_CONTROL_PORT);
            }
        }
        flash_sync_protection_mmio(emu);
    } else if (port == EZ80_FLASH_PROTECT_CONTROL_PORT) {
        if ((value & EZ80_FLASH_PROTECT_MODE_LOCKDOWN_MASK) != 0u) {
            emu->platform.flash_protect_mode = EZ80_FLASH_PROTECT_MODE_LOCKED;
        } else {
            emu->platform.flash_protect_mode = EZ80_FLASH_PROTECT_MODE_PERMISSIVE;
        }
        flash_sync_protection_mmio(emu);
    } else if (port == 0x3Eu) {
        if ((value & 0x03u) != 0u) {
            uint8_t status = port_shadow_read(emu, 0x3Du);
            status &= (uint8_t)~0x03u;
            port_shadow_write(emu, 0x3Du, status);
        }
    } else if (port == EZ80_PORT_KEYPAD_ROW_SELECT) {
        emu->hw.keypad_row_select = value;
        emu->memory.mmio[EZ80_MMIO_REG_KEY_STATUS] = hardware_compute_keypad_data(emu);
    } else if (port == EZ80_PORT_LCD_COMMAND) {
        lcd_begin_command(&emu->hw, value);
        if (value == EZ80_LCD_CMD_SOFTWARE_RESET) {
            memset(g_lcd_framebuffer, 0, sizeof(g_lcd_framebuffer));
            lcd_set_full_window(&emu->hw);
            emu->hw.lcd_status |= EZ80_LCD_STATUS_DATA_READY;
        }
        lcd_refresh_status(emu);
    } else if (port == EZ80_PORT_LCD_DATA) {
        lcd_apply_command_data(emu, value);
    } else if (port == EZ80_PORT_USB_CONTROL) {
        emu->hw.usb_control = (uint8_t)(value & (EZ80_USB_CONTROL_ENABLE |
                                                EZ80_USB_CONTROL_RX_IRQ_ENABLE |
                                                EZ80_USB_CONTROL_TX_IRQ_ENABLE |
                                                EZ80_USB_CONTROL_LOOPBACK));
        if ((value & EZ80_USB_CONTROL_RESET) != 0u) {
            usb_reset_controller(emu);
        } else {
            usb_update_status(emu);
        }
    } else if (port == EZ80_PORT_USB_TX) {
        usb_queue_tx_byte(emu, value);
    } else if (port == EZ80_PORT_USB_RX) {
        usb_push_rx_byte(emu, value, true);
    } else if (port == EZ80_PORT_TIMER0_CONTROL) {
        emu->hw.timer0_control = value;
    } else if (port == EZ80_PORT_TIMER1_CONTROL) {
        emu->hw.timer1_control = value;
    } else if (port == EZ80_PORT_TIMER0_COUNTER_LO) {
        emu->hw.timer0_counter = (uint16_t)((emu->hw.timer0_counter & 0xFF00u) | value);
    } else if (port == EZ80_PORT_TIMER0_COUNTER_HI) {
        emu->hw.timer0_counter = (uint16_t)((emu->hw.timer0_counter & 0x00FFu) | ((uint16_t)value << 8));
    } else if (port == EZ80_PORT_TIMER1_COUNTER_LO) {
        emu->hw.timer1_counter = (uint16_t)((emu->hw.timer1_counter & 0xFF00u) | value);
    } else if (port == EZ80_PORT_TIMER1_COUNTER_HI) {
        emu->hw.timer1_counter = (uint16_t)((emu->hw.timer1_counter & 0x00FFu) | ((uint16_t)value << 8));
    } else if (port == EZ80_PORT_TIMER0_RELOAD_LO) {
        emu->hw.timer0_reload = (uint16_t)((emu->hw.timer0_reload & 0xFF00u) | value);
    } else if (port == EZ80_PORT_TIMER0_RELOAD_HI) {
        emu->hw.timer0_reload = (uint16_t)((emu->hw.timer0_reload & 0x00FFu) | ((uint16_t)value << 8));
    } else if (port == EZ80_PORT_TIMER1_RELOAD_LO) {
        emu->hw.timer1_reload = (uint16_t)((emu->hw.timer1_reload & 0xFF00u) | value);
    } else if (port == EZ80_PORT_TIMER1_RELOAD_HI) {
        emu->hw.timer1_reload = (uint16_t)((emu->hw.timer1_reload & 0x00FFu) | ((uint16_t)value << 8));
    } else if (port == EZ80_PORT_TIMER0_PRESCALER) {
        emu->hw.timer0_prescaler = (uint8_t)(value & 0x03u);
        emu->hw.timer0_prescaler_accum = 0u;
    } else if (port == EZ80_PORT_TIMER1_PRESCALER) {
        emu->hw.timer1_prescaler = (uint8_t)(value & 0x03u);
        emu->hw.timer1_prescaler_accum = 0u;
    } else if (port == EZ80_PORT_RTC_SECONDS) {
        uint32_t minutes = (emu->hw.rtc_seconds_of_day / 60u) % 60u;
        uint32_t hours = (emu->hw.rtc_seconds_of_day / 3600u) % 24u;
        emu->hw.rtc_seconds_of_day = ((hours * 60u + minutes) * 60u) + (uint32_t)(value % 60u);
    } else if (port == EZ80_PORT_RTC_MINUTES) {
        uint32_t seconds = emu->hw.rtc_seconds_of_day % 60u;
        uint32_t hours = (emu->hw.rtc_seconds_of_day / 3600u) % 24u;
        emu->hw.rtc_seconds_of_day = ((hours * 60u + (uint32_t)(value % 60u)) * 60u) + seconds;
    } else if (port == EZ80_PORT_RTC_HOURS) {
        uint32_t seconds = emu->hw.rtc_seconds_of_day % 60u;
        uint32_t minutes = (emu->hw.rtc_seconds_of_day / 60u) % 60u;
        emu->hw.rtc_seconds_of_day = ((((uint32_t)(value % 24u) * 60u) + minutes) * 60u) + seconds;
    } else if (port == EZ80_PORT_RTC_ALARM_SECONDS) {
        rtc_alarm_write_component(emu, &emu->hw.rtc_alarm_seconds, value, 60u);
    } else if (port == EZ80_PORT_RTC_ALARM_MINUTES) {
        rtc_alarm_write_component(emu, &emu->hw.rtc_alarm_minutes, value, 60u);
    } else if (port == EZ80_PORT_RTC_ALARM_HOURS) {
        rtc_alarm_write_component(emu, &emu->hw.rtc_alarm_hours, value, 24u);
    } else if (port == EZ80_PORT_RTC_ALARM_CONTROL) {
        emu->hw.rtc_alarm_control = value;
        if ((value & EZ80_RTC_ALARM_CONTROL_ENABLE) == 0u) {
            emu->hw.rtc_alarm_fired = false;
        }
    } else if (port == EZ80_PORT_WDT_CONTROL) {
        if (emu->hw.wdt_timeout == 0u) {
            emu->hw.wdt_timeout = EZ80_WDT_DEFAULT_TIMEOUT;
        }
        (void)ez80_emulator_wdt_configure(emu, value, emu->hw.wdt_timeout);
    } else if (port == EZ80_PORT_WDT_FEED) {
        if (value == EZ80_WDT_FEED_MAGIC) {
            (void)ez80_emulator_wdt_feed(emu);
        }
    } else if (port == EZ80_PORT_WDT_TIMEOUT_LO) {
        emu->hw.wdt_timeout = (emu->hw.wdt_timeout & 0xFF00u) | (uint16_t)value;
    } else if (port == EZ80_PORT_WDT_TIMEOUT_HI) {
        emu->hw.wdt_timeout = (emu->hw.wdt_timeout & 0x00FFu) | ((uint16_t)value << 8);
    } else if (port == EZ80_PORT_IRQ_MASK) {
        emu->hw.irq_mask = value;
    } else if (port == EZ80_PORT_IRQ_PENDING) {
        emu->hw.irq_pending = value;
    } else if (port == EZ80_PORT_IRQ_ACK) {
        emu->hw.irq_pending &= (uint8_t)(~value);
    }

    return EZ80_OK;
}

static ez80_status_t port_read_legacy_low(ez80_emulator_t *emu, uint8_t port, uint8_t *out_value)
{
    if (!emu || !out_value) return EZ80_ERR_INVALID_ARG;
    if (port == EZ80_PORT_USB_CONTROL) {
        *out_value = emu->hw.usb_control;
        return EZ80_OK;
    }
    if (port == EZ80_PORT_USB_STATUS) {
        usb_update_status(emu);
        *out_value = emu->hw.usb_status;
        return EZ80_OK;
    }
    if (port == EZ80_PORT_USB_TX) {
        *out_value = emu->hw.usb_last_tx_data;
        return EZ80_OK;
    }
    if (port == EZ80_PORT_USB_RX) {
        *out_value = usb_pop_rx_byte(emu);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return EZ80_OK;
    }
    *out_value = port_shadow_read(emu, port);
    if (port == EZ80_FLASH_UNLOCK_PORT && emu->platform.port28_unlock_read_latch != 0u) {
        *out_value |= EZ80_FLASH_UNLOCK_OS58_MASK;
        emu->platform.port28_unlock_read_latch--;
    }
    return EZ80_OK;
}

/* ---- SPI port handlers ---- */
static uint8_t spi_channel_from_port(uint16_t port)
{
    if (port >= EZ80_PORT_SPI1_BASE && port <= (EZ80_PORT_SPI1_BASE + 3u)) return 1u;
    return 0u;
}

static uint8_t spi_reg_from_port(uint16_t port)
{
    if (port >= EZ80_PORT_SPI1_BASE) return (uint8_t)(port - EZ80_PORT_SPI1_BASE);
    return (uint8_t)(port - EZ80_PORT_SPI0_BASE);
}

static void spi_finalize_burst(ez80_spi_channel_t *spi)
{
    if (!spi) return;
    if (spi->active_burst_len > 0u) {
        spi->last_burst_len = spi->active_burst_len;
        spi->active_burst_len = 0u;
    }
}

static void spi_handle_control_transition(ez80_emulator_t *emu,
                                          ez80_spi_channel_t *spi,
                                          uint8_t old_control,
                                          uint8_t new_control)
{
    bool old_cs_active;
    bool new_cs_active;

    if (!emu || !spi) return;

    old_cs_active = (old_control & EZ80_SPI_CONTROL_CS_ACTIVE) != 0u;
    new_cs_active = (new_control & EZ80_SPI_CONTROL_CS_ACTIVE) != 0u;

    if (!old_cs_active && new_cs_active) {
        spi->cs_assert_count++;
        spi->last_cs_assert_cycle = emu->hw.total_cycles;
        spi->active_burst_len = 0u;
    } else if (old_cs_active && !new_cs_active) {
        spi_finalize_burst(spi);
        spi->cs_deassert_count++;
        spi->last_cs_deassert_cycle = emu->hw.total_cycles;
    }
}

static void spi_update_status(ez80_spi_channel_t *spi)
{
    uint8_t st;
    if (!spi) return;
    st = spi->status & (uint8_t)(EZ80_SPI_STATUS_OVERRUN |
                                 EZ80_SPI_STATUS_IRQ_PENDING |
                                 EZ80_SPI_STATUS_UNDERRUN);
    if (spi->tx_count == 0u) st |= EZ80_SPI_STATUS_TX_EMPTY;
    if (spi->rx_count > 0u) st |= EZ80_SPI_STATUS_RX_READY;
    if ((spi->control & EZ80_SPI_CONTROL_CS_ACTIVE) != 0u) st |= EZ80_SPI_STATUS_CS_ACTIVE;
    if (((spi->control & EZ80_SPI_CONTROL_CS_ACTIVE) != 0u) &&
        (spi->active_burst_len > 0u)) {
        st |= EZ80_SPI_STATUS_BUSY;
    }
    spi->status = st;
}

static void spi_reset_channel(ez80_spi_channel_t *spi)
{
    if (!spi) return;
    memset(spi->tx_fifo, 0, sizeof(spi->tx_fifo));
    memset(spi->rx_fifo, 0, sizeof(spi->rx_fifo));
    memset(spi->rx_source_fifo, 0, sizeof(spi->rx_source_fifo));
    spi->tx_head = 0u;
    spi->tx_count = 0u;
    spi->rx_head = 0u;
    spi->rx_count = 0u;
    spi->rx_source_head = 0u;
    spi->rx_source_count = 0u;
    spi->total_tx_bytes = 0u;
    spi->total_rx_bytes = 0u;
    spi->last_mode = 0u;
    spi->last_tx_byte = 0u;
    spi->last_rx_byte = 0u;
    spi->active_burst_len = 0u;
    spi->last_burst_len = 0u;
    spi->transfer_complete_count = 0u;
    spi->cs_assert_count = 0u;
    spi->cs_deassert_count = 0u;
    spi->last_transfer_cycle = 0u;
    spi->last_cs_assert_cycle = 0u;
    spi->last_cs_deassert_cycle = 0u;
    spi->control = 0u;
    spi->status = EZ80_SPI_STATUS_TX_EMPTY;
}

static ez80_status_t spi_queue_result_byte(ez80_spi_channel_t *spi, uint8_t value)
{
    uint8_t idx;
    if (!spi) return EZ80_ERR_INVALID_ARG;
    if (spi->rx_count >= EZ80_SPI_FIFO_SIZE) {
        spi->status |= EZ80_SPI_STATUS_OVERRUN;
        spi_update_status(spi);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }
    idx = (uint8_t)((spi->rx_head + spi->rx_count) % EZ80_SPI_FIFO_SIZE);
    spi->rx_fifo[idx] = value;
    spi->rx_count++;
    return EZ80_OK;
}

static uint8_t spi_consume_source_byte(ez80_spi_channel_t *spi, bool *out_had_source)
{
    uint8_t value = 0xFFu;
    if (!spi) return 0xFFu;
    if (spi->rx_source_count > 0u) {
        value = spi->rx_source_fifo[spi->rx_source_head];
        spi->rx_source_head = (uint8_t)((spi->rx_source_head + 1u) % EZ80_SPI_FIFO_SIZE);
        spi->rx_source_count--;
        if (out_had_source) *out_had_source = true;
        return value;
    }
    if (out_had_source) *out_had_source = false;
    return value;
}

static ez80_status_t spi_perform_transfer(ez80_emulator_t *emu,
                                          ez80_spi_channel_t *spi,
                                          uint8_t tx_byte,
                                          uint8_t *out_rx_byte,
                                          bool queue_rx)
{
    uint8_t rx_raw;
    uint8_t tx_wire;
    uint8_t rx_wire;
    ez80_status_t queue_status = EZ80_OK;

    if (!emu || !spi) return EZ80_ERR_INVALID_ARG;
    if ((spi->control & EZ80_SPI_CONTROL_ENABLE) == 0u) {
        return EZ80_ERR_INVALID_ARG;
    }

    if ((spi->control & EZ80_SPI_CONTROL_CS_ACTIVE) == 0u) {
        spi_finalize_burst(spi);
    }

    tx_wire = ((spi->control & EZ80_SPI_CONTROL_LSB_FIRST) != 0u)
        ? reverse_bits8(tx_byte)
        : tx_byte;
    rx_raw = spi_consume_source_byte(spi, NULL);
    rx_wire = ((spi->control & EZ80_SPI_CONTROL_LSB_FIRST) != 0u)
        ? reverse_bits8(rx_raw)
        : rx_raw;

    spi->total_tx_bytes++;
    spi->total_rx_bytes++;
    spi->transfer_complete_count++;
    spi->last_mode = spi->control & (uint8_t)(EZ80_SPI_CONTROL_CPOL | EZ80_SPI_CONTROL_CPHA);
    spi->last_tx_byte = tx_wire;
    spi->last_rx_byte = rx_wire;
    spi->last_transfer_cycle = emu->hw.total_cycles;
    if ((spi->control & EZ80_SPI_CONTROL_CS_ACTIVE) != 0u &&
        spi->active_burst_len < EZ80_SPI_FIFO_SIZE) {
        spi->active_burst_len++;
    } else if ((spi->control & EZ80_SPI_CONTROL_CS_ACTIVE) == 0u) {
        spi->active_burst_len = 1u;
        spi_finalize_burst(spi);
    }

    if (queue_rx) {
        queue_status = spi_queue_result_byte(spi, rx_wire);
    } else if (out_rx_byte) {
        *out_rx_byte = rx_wire;
    }

    if ((spi->control & EZ80_SPI_CONTROL_IRQ_ENABLE) != 0u) {
        spi->status |= EZ80_SPI_STATUS_IRQ_PENDING;
        hardware_raise_irq(emu, EZ80_IRQ_SOURCE_SPI);
    }

    spi_update_status(spi);
    return queue_status;
}

static ez80_status_t io_spi_handler_write(ez80_emulator_t *emu,
                                           uint16_t port,
                                           uint8_t value,
                                           void *user_data)
{
    uint8_t ch = spi_channel_from_port(port);
    uint8_t reg = spi_reg_from_port(port);
    ez80_spi_channel_t *spi;
    uint8_t old_control;
    (void)user_data;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (ch >= EZ80_SPI_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;
    spi = &emu->hw.spi[ch];
    old_control = spi->control;

    switch (reg) {
    case 0u: /* CONTROL */
        if ((value & EZ80_SPI_CONTROL_RESET) != 0u) {
            spi_handle_control_transition(emu, spi, old_control, 0u);
            spi_reset_channel(spi);
        } else {
            spi->control = value;
            if ((value & EZ80_SPI_CONTROL_IRQ_ENABLE) == 0u) {
                spi->status &= (uint8_t)~EZ80_SPI_STATUS_IRQ_PENDING;
            }
            spi_handle_control_transition(emu, spi, old_control, spi->control);
        }
        spi_update_status(spi);
        break;
    case 2u: /* DATA (TX) */
        return spi_perform_transfer(emu, spi, value, NULL, true);
    case 3u: /* BAUD */
        spi->baud_divisor = value;
        break;
    default:
        break;
    }
    return EZ80_OK;
}

static ez80_status_t io_spi_handler_read(ez80_emulator_t *emu,
                                          uint16_t port,
                                          uint8_t *out_value,
                                          void *user_data)
{
    uint8_t ch = spi_channel_from_port(port);
    uint8_t reg = spi_reg_from_port(port);
    ez80_spi_channel_t *spi;
    (void)user_data;
    if (!emu || !out_value) return EZ80_ERR_INVALID_ARG;
    if (ch >= EZ80_SPI_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;
    spi = &emu->hw.spi[ch];

    switch (reg) {
    case 0u: /* CONTROL */
        *out_value = spi->control;
        break;
    case 1u: /* STATUS */
        spi_update_status(spi);
        *out_value = spi->status;
        spi->status &= (uint8_t)~EZ80_SPI_STATUS_IRQ_PENDING;
        spi_update_status(spi);
        break;
    case 2u: /* DATA (RX) */
        if (spi->rx_count > 0u) {
            *out_value = spi->rx_fifo[spi->rx_head];
            spi->rx_head = (uint8_t)((spi->rx_head + 1u) % EZ80_SPI_FIFO_SIZE);
            spi->rx_count--;
        } else {
            *out_value = 0xFFu;
            spi->status |= EZ80_SPI_STATUS_UNDERRUN;
        }
        spi->status &= (uint8_t)~EZ80_SPI_STATUS_IRQ_PENDING;
        spi_update_status(spi);
        break;
    case 3u: /* BAUD */
        *out_value = spi->baud_divisor;
        break;
    default:
        *out_value = 0u;
        break;
    }
    return EZ80_OK;
}

/* ---- GPIO port handlers ---- */
static uint8_t gpio_port_index_from_port(uint16_t port)
{
    /* GPIO ports at 0x3000,0x3010,0x3020,...,0x3050 -> index 0..5 */
    return (uint8_t)((port - EZ80_PORT_GPIO_A_DATA) >> 4);
}

static uint8_t gpio_reg_offset(uint16_t port)
{
    return (uint8_t)(port & 0x0Fu);
}

static ez80_status_t io_gpio_handler_write(ez80_emulator_t *emu,
                                            uint16_t port,
                                            uint8_t value,
                                            void *user_data)
{
    uint8_t idx = gpio_port_index_from_port(port);
    uint8_t reg = gpio_reg_offset(port);
    (void)user_data;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (idx >= EZ80_GPIO_PORT_COUNT) return EZ80_ERR_INVALID_ARG;

    switch (reg) {
    case EZ80_GPIO_REG_DATA:
        emu->hw.gpio[idx].output_latch = (uint8_t)(
            (emu->hw.gpio[idx].output_latch & (uint8_t)~emu->hw.gpio[idx].direction) |
            (value & emu->hw.gpio[idx].direction)
        );
        if (idx == EZ80_GPIO_BACKLIGHT_PORT &&
            ((emu->hw.gpio[idx].alt0 | emu->hw.gpio[idx].alt1) & EZ80_GPIO_BACKLIGHT_BIT) == 0u &&
            (emu->hw.gpio[idx].direction & EZ80_GPIO_BACKLIGHT_BIT) != 0u) {
            emu->hw.backlight_on = (emu->hw.gpio[idx].output_latch & EZ80_GPIO_BACKLIGHT_BIT) != 0u;
        }
        gpio_refresh_port_state(emu, idx);
        break;
    case EZ80_GPIO_REG_DIR:
        emu->hw.gpio[idx].direction = value;
        gpio_refresh_port_state(emu, idx);
        break;
    case EZ80_GPIO_REG_ALT0:
        emu->hw.gpio[idx].alt0 = value;
        gpio_refresh_port_state(emu, idx);
        break;
    case EZ80_GPIO_REG_ALT1:
        emu->hw.gpio[idx].alt1 = value;
        gpio_refresh_port_state(emu, idx);
        break;
    case EZ80_GPIO_REG_PULLUP:
        emu->hw.gpio[idx].pull_up = value;
        emu->hw.gpio[idx].pull_down &= (uint8_t)~value;
        gpio_refresh_port_state(emu, idx);
        break;
    case EZ80_GPIO_REG_PULLDOWN:
        emu->hw.gpio[idx].pull_down = value;
        emu->hw.gpio[idx].pull_up &= (uint8_t)~value;
        gpio_refresh_port_state(emu, idx);
        break;
    case EZ80_GPIO_REG_IRQ_RISING:
        emu->hw.gpio[idx].irq_rising = value;
        break;
    case EZ80_GPIO_REG_IRQ_FALLING:
        emu->hw.gpio[idx].irq_falling = value;
        break;
    case EZ80_GPIO_REG_IRQ_PENDING:
        emu->hw.gpio[idx].irq_pending &= (uint8_t)~value;
        break;
    }
    return EZ80_OK;
}

static ez80_status_t io_gpio_handler_read(ez80_emulator_t *emu,
                                           uint16_t port,
                                           uint8_t *out_value,
                                           void *user_data)
{
    uint8_t idx = gpio_port_index_from_port(port);
    uint8_t reg = gpio_reg_offset(port);
    (void)user_data;
    if (!emu || !out_value) return EZ80_ERR_INVALID_ARG;
    if (idx >= EZ80_GPIO_PORT_COUNT) return EZ80_ERR_INVALID_ARG;

    switch (reg) {
    case EZ80_GPIO_REG_DATA:
        gpio_refresh_port_state(emu, idx);
        *out_value = emu->hw.gpio[idx].data;
        break;
    case EZ80_GPIO_REG_DIR: *out_value = emu->hw.gpio[idx].direction; break;
    case EZ80_GPIO_REG_ALT0: *out_value = emu->hw.gpio[idx].alt0; break;
    case EZ80_GPIO_REG_ALT1: *out_value = emu->hw.gpio[idx].alt1; break;
    case EZ80_GPIO_REG_PULLUP: *out_value = emu->hw.gpio[idx].pull_up; break;
    case EZ80_GPIO_REG_PULLDOWN: *out_value = emu->hw.gpio[idx].pull_down; break;
    case EZ80_GPIO_REG_IRQ_RISING: *out_value = emu->hw.gpio[idx].irq_rising; break;
    case EZ80_GPIO_REG_IRQ_FALLING: *out_value = emu->hw.gpio[idx].irq_falling; break;
    case EZ80_GPIO_REG_IRQ_PENDING: *out_value = emu->hw.gpio[idx].irq_pending; break;
    default: *out_value = 0u; break;
    }
    return EZ80_OK;
}

static ez80_status_t io_keyboard_handler_write(ez80_emulator_t *emu,
                                               uint16_t port,
                                               uint8_t value,
                                               void *user_data)
{
    uint8_t group = (uint8_t)((port >> 8) & 0xFFu);
    uint8_t reg = (uint8_t)(port & 0xFFu);
    uint8_t before_state;
    (void)user_data;
    if (!emu) return EZ80_ERR_INVALID_ARG;

    before_state = keyboard_compute_group_data(emu);

    if (group == EZ80_PORT_GROUP_KEYBOARD_CTRL) {
        if (reg == EZ80_PORT_KBD_CTRL_GROUP_SELECT) {
            emu->hw.kbd_group_select = value;
        } else if (reg == EZ80_PORT_KBD_CTRL_IRQ_ON_KEYPRESS) {
            emu->hw.kbd_irq_on_keypress = (value & EZ80_KBD_IRQ_ON_KEYPRESS_ENABLE) != 0u;
        }
    } else if (group == EZ80_PORT_GROUP_KEYBOARD_DATA) {
        if (reg < 8u) {
            emu->hw.keypad_rows[reg] = value;
        }
    }

    emu->hw.kbd_last_state = keyboard_compute_group_data(emu);
    emu->memory.mmio[EZ80_MMIO_REG_KEY_STATUS] = emu->hw.kbd_last_state;

    if (emu->hw.kbd_irq_on_keypress &&
        ((before_state & (uint8_t)~emu->hw.kbd_last_state) != 0u)) {
        hardware_raise_irq(emu, EZ80_IRQ_SOURCE_KEYPAD);
    }

    return EZ80_OK;
}

static ez80_status_t io_keyboard_handler_read(ez80_emulator_t *emu,
                                              uint16_t port,
                                              uint8_t *out_value,
                                              void *user_data)
{
    uint8_t group = (uint8_t)((port >> 8) & 0xFFu);
    uint8_t reg = (uint8_t)(port & 0xFFu);
    (void)user_data;
    if (!emu || !out_value) return EZ80_ERR_INVALID_ARG;

    if (group == EZ80_PORT_GROUP_KEYBOARD_CTRL) {
        if (reg == EZ80_PORT_KBD_CTRL_GROUP_SELECT) {
            *out_value = emu->hw.kbd_group_select;
        } else if (reg == EZ80_PORT_KBD_CTRL_IRQ_ON_KEYPRESS) {
            *out_value = emu->hw.kbd_irq_on_keypress ? EZ80_KBD_IRQ_ON_KEYPRESS_ENABLE : 0u;
        } else if (reg == EZ80_PORT_KBD_CTRL_STATUS) {
            uint8_t status = 0u;
            if (keyboard_any_key_pressed(emu)) status |= EZ80_KBD_STATUS_KEY_PRESSED;
            if ((emu->hw.irq_pending & EZ80_IRQ_SOURCE_KEYPAD) != 0u) status |= EZ80_KBD_STATUS_IRQ_PENDING;
            *out_value = status;
        } else {
            *out_value = 0u;
        }
    } else if (group == EZ80_PORT_GROUP_KEYBOARD_DATA) {
        if (reg == EZ80_PORT_KBD_DATA_MATRIX) {
            *out_value = keyboard_compute_group_data(emu);
        } else if (reg < 8u) {
            *out_value = emu->hw.keypad_rows[reg];
        } else {
            *out_value = 0xFFu;
        }
    } else {
        *out_value = 0u;
    }

    emu->hw.kbd_last_state = keyboard_compute_group_data(emu);
    emu->memory.mmio[EZ80_MMIO_REG_KEY_STATUS] = emu->hw.kbd_last_state;
    return EZ80_OK;
}

static ez80_status_t io_lcd_handler_write(ez80_emulator_t *emu,
                                          uint16_t port,
                                          uint8_t value,
                                          void *user_data)
{
    uint8_t reg = (uint8_t)(port & 0xFFu);
    (void)user_data;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    lcd_port_reg_write(emu, reg, value);
    return EZ80_OK;
}

static ez80_status_t io_lcd_handler_read(ez80_emulator_t *emu,
                                         uint16_t port,
                                         uint8_t *out_value,
                                         void *user_data)
{
    uint8_t reg = (uint8_t)(port & 0xFFu);
    (void)user_data;
    if (!emu || !out_value) return EZ80_ERR_INVALID_ARG;
    lcd_refresh_status(emu);
    *out_value = lcd_port_reg_read(emu, reg);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_set_port_handler(ez80_emulator_t *emu,
                                             uint8_t slot_index,
                                             uint16_t start_port,
                                             uint16_t end_port,
                                             ez80_port_read_handler_t read_handler,
                                             ez80_port_write_handler_t write_handler,
                                             void *user_data)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (slot_index >= EZ80_MAX_PORT_HANDLERS) return EZ80_ERR_INVALID_ARG;
    if (start_port > end_port) return EZ80_ERR_INVALID_ARG;
    if (!read_handler && !write_handler) return EZ80_ERR_INVALID_ARG;

    emu->port_handlers[slot_index].enabled = true;
    emu->port_handlers[slot_index].start_port = start_port;
    emu->port_handlers[slot_index].end_port = end_port;
    emu->port_handlers[slot_index].read_handler = read_handler;
    emu->port_handlers[slot_index].write_handler = write_handler;
    emu->port_handlers[slot_index].user_data = user_data;
    return EZ80_OK;
}

ez80_status_t ez80_emulator_clear_port_handlers(ez80_emulator_t *emu)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    memset(emu->port_handlers, 0, sizeof(emu->port_handlers));
    return EZ80_OK;
}

static void io_install_default_handlers(ez80_emulator_t *emu)
{
    if (!emu) return;
    ez80_emulator_clear_port_handlers(emu);
    ez80_emulator_set_port_handler(emu,
                                   0u,
                                   0x0100u,
                                   0x02FFu,
                                   io_keyboard_handler_read,
                                   io_keyboard_handler_write,
                                   NULL);
    ez80_emulator_set_port_handler(emu,
                                   1u,
                                   0x4000u,
                                   0x40FFu,
                                   io_lcd_handler_read,
                                   io_lcd_handler_write,
                                   NULL);
    /* SPI0 + SPI1 ports */
    ez80_emulator_set_port_handler(emu,
                                   2u,
                                   EZ80_PORT_SPI0_BASE,
                                   (uint16_t)(EZ80_PORT_SPI0_BASE + 3u),
                                   io_spi_handler_read,
                                   io_spi_handler_write,
                                   NULL);
    ez80_emulator_set_port_handler(emu,
                                   3u,
                                   EZ80_PORT_SPI1_BASE,
                                   (uint16_t)(EZ80_PORT_SPI1_BASE + 3u),
                                   io_spi_handler_read,
                                   io_spi_handler_write,
                                   NULL);
    /* GPIO ports A-F with extended control registers (0x3000..0x3058) */
    ez80_emulator_set_port_handler(emu,
                                   4u,
                                   EZ80_PORT_GPIO_A_DATA,
                                   (uint16_t)(EZ80_PORT_GPIO_F_DATA + EZ80_GPIO_REG_IRQ_PENDING),
                                   io_gpio_handler_read,
                                   io_gpio_handler_write,
                                   NULL);
    /* I2C0 ports (0x00C8..0x00CE) */
    ez80_emulator_set_port_handler(emu,
                                   5u,
                                   EZ80_PORT_I2C0_BASE,
                                   EZ80_PORT_I2C0_SRR,
                                   io_i2c_handler_read,
                                   io_i2c_handler_write,
                                   NULL);
    /* I2C1 ports (0x00D0..0x00D6) */
    ez80_emulator_set_port_handler(emu,
                                   6u,
                                   EZ80_PORT_I2C1_BASE,
                                   EZ80_PORT_I2C1_SRR,
                                   io_i2c_handler_read,
                                   io_i2c_handler_write,
                                   NULL);
    /* UART0 ports (0x00C0..0x00C7) */
    ez80_emulator_set_port_handler(emu,
                                   7u,
                                   EZ80_PORT_UART0_BASE,
                                   EZ80_PORT_UART0_SPR,
                                   io_uart_handler_read,
                                   io_uart_handler_write,
                                   NULL);
    /* UART1 ports (0x00D8..0x00DF) */
    ez80_emulator_set_port_handler(emu,
                                   8u,
                                   EZ80_PORT_UART1_BASE,
                                   EZ80_PORT_UART1_SPR,
                                   io_uart_handler_read,
                                   io_uart_handler_write,
                                   NULL);
}

static void port_write(ez80_emulator_t *emu, uint16_t port, uint8_t value)
{
    uint8_t i;
    ez80_status_t status;
    if (!emu) return;

    if (boot_irq_overlay_write(emu, port, value)) {
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;
    }

    for (i = 0u; i < EZ80_MAX_PORT_HANDLERS; i++) {
        ez80_port_handler_slot_t *slot = &emu->port_handlers[i];
        if (!slot->enabled) continue;
        if (port < slot->start_port || port > slot->end_port) continue;
        if (!slot->write_handler) continue;
        status = slot->write_handler(emu, port, value, slot->user_data);
        if (status != EZ80_OK) return;
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return;
    }

    if (port <= 0x00FFu) {
        status = port_write_legacy_low(emu, (uint8_t)(port & 0xFFu), value);
        if (status != EZ80_OK) return;
    } else {
        port_shadow_write(emu, port, value);
    }
    hardware_sync_ports(emu);
    hardware_sync_mmio(emu);
}

static uint8_t port_read(ez80_emulator_t *emu, uint16_t port)
{
    uint8_t i;
    uint8_t value = 0u;
    ez80_status_t status;
    if (!emu) return 0u;

    if (boot_irq_overlay_read(emu, port, &value)) {
        return value;
    }

    for (i = 0u; i < EZ80_MAX_PORT_HANDLERS; i++) {
        ez80_port_handler_slot_t *slot = &emu->port_handlers[i];
        if (!slot->enabled) continue;
        if (port < slot->start_port || port > slot->end_port) continue;
        if (!slot->read_handler) continue;
        status = slot->read_handler(emu, port, &value, slot->user_data);
        if (status != EZ80_OK) return 0u;
        return value;
    }

    if (port <= 0x00FFu) {
        status = port_read_legacy_low(emu, (uint8_t)(port & 0xFFu), &value);
        if (status != EZ80_OK) return 0u;
        return value;
    }

    return port_shadow_read(emu, port);
}

static void io_record_last_out(ez80_emulator_t *emu, uint16_t port, uint8_t value)
{
    if (!emu) return;
    mmio_set(emu, EZ80_MMIO_REG_LAST_OUT_PORT, (uint8_t)(port & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_LAST_OUT_PORT_HI, (uint8_t)((port >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_LAST_OUT_VALUE, value);
}

static void io_record_last_in(ez80_emulator_t *emu, uint16_t port, uint8_t value)
{
    if (!emu) return;
    mmio_set(emu, EZ80_MMIO_REG_LAST_IN_PORT, (uint8_t)(port & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_LAST_IN_PORT_HI, (uint8_t)((port >> 8) & 0xFFu));
    mmio_set(emu, EZ80_MMIO_REG_LAST_IN_VALUE, value);
}

static uint32_t timer_prescaler_divisor(uint8_t prescaler)
{
    switch (prescaler & 0x03u) {
    case EZ80_TIMER_PRESCALER_4:  return 4u;
    case EZ80_TIMER_PRESCALER_16: return 16u;
    case EZ80_TIMER_PRESCALER_64: return 64u;
    default:                       return 1u;
    }
}

static uint8_t pm_wake_source_for_irq_source(uint8_t irq_source)
{
    switch (irq_source) {
    case EZ80_IRQ_SOURCE_KEYPAD:
        return EZ80_PM_WAKE_KEYPAD;
    case EZ80_IRQ_SOURCE_TIMER0:
    case EZ80_IRQ_SOURCE_TIMER1:
        return EZ80_PM_WAKE_TIMER;
    case EZ80_IRQ_SOURCE_USB:
        return EZ80_PM_WAKE_USB;
    default:
        return EZ80_PM_WAKE_NONE;
    }
}

static bool pm_mode_allows_timer_advance(const ez80_emulator_t *emu)
{
    if (!emu) return false;
    return emu->hw.pm_mode <= EZ80_PM_MODE_IDLE;
}

static bool pm_mode_allows_usb_advance(const ez80_emulator_t *emu)
{
    if (!emu) return false;
    return emu->hw.pm_mode <= EZ80_PM_MODE_IDLE;
}

static bool pm_mode_allows_wake_source(uint8_t mode, uint8_t wake_source)
{
    switch (mode) {
    case EZ80_PM_MODE_IDLE:
        return wake_source != EZ80_PM_WAKE_NONE;

    case EZ80_PM_MODE_STANDBY:
        return wake_source == EZ80_PM_WAKE_KEYPAD ||
               wake_source == EZ80_PM_WAKE_USB ||
               wake_source == EZ80_PM_WAKE_RTC_ALARM ||
               wake_source == EZ80_PM_WAKE_NMI ||
               wake_source == EZ80_PM_WAKE_WATCHDOG;

    case EZ80_PM_MODE_SLEEP:
        return wake_source == EZ80_PM_WAKE_KEYPAD ||
               wake_source == EZ80_PM_WAKE_RTC_ALARM ||
               wake_source == EZ80_PM_WAKE_NMI ||
               wake_source == EZ80_PM_WAKE_WATCHDOG;

    default:
        return false;
    }
}

static void pm_note_sleep_cycles(ez80_emulator_t *emu, uint32_t cycles)
{
    if (!emu || cycles == 0u) return;
    if (emu->hw.pm_mode < EZ80_PM_MODE_STANDBY) return;

    if (UINT32_MAX - emu->hw.pm_total_sleep_cycles < cycles) {
        emu->hw.pm_total_sleep_cycles = UINT32_MAX;
        return;
    }
    emu->hw.pm_total_sleep_cycles += cycles;
}

static void pm_note_wake_event(ez80_emulator_t *emu, uint8_t wake_source, bool resume_cpu)
{
    if (!emu || wake_source == EZ80_PM_WAKE_NONE) return;
    if (emu->hw.pm_mode == EZ80_PM_MODE_ACTIVE) return;
    if (!pm_mode_allows_wake_source(emu->hw.pm_mode, wake_source)) return;

    emu->hw.pm_mode = EZ80_PM_MODE_ACTIVE;
    emu->hw.pm_wake_source = wake_source;
    emu->hw.power_state = 1u;
    if (resume_cpu) {
        emu->cpu.halted = false;
    }
}

static void pm_apply_mode_state(ez80_emulator_t *emu, uint8_t mode)
{
    bool entering_sleep;
    if (!emu || mode > EZ80_PM_MODE_SLEEP) return;

    entering_sleep = (mode >= EZ80_PM_MODE_STANDBY) &&
                     (emu->hw.pm_mode != mode);

    emu->hw.pm_mode = mode;
    if (mode == EZ80_PM_MODE_ACTIVE) {
        emu->hw.pm_wake_source = EZ80_PM_WAKE_NONE;
        emu->hw.power_state = 1u;
        emu->cpu.halted = false;
        return;
    }

    emu->hw.pm_wake_source = EZ80_PM_WAKE_NONE;
    emu->hw.power_state = (mode >= EZ80_PM_MODE_STANDBY) ? 2u : 1u;
    emu->cpu.halted = true;
    emu->hw.halt_pc_advanced = false;
    if (entering_sleep) {
        emu->hw.pm_sleep_count++;
    }
}

static void rtc_alarm_write_component(ez80_emulator_t *emu,
                                      uint8_t *field,
                                      uint8_t value,
                                      uint8_t modulo)
{
    if (!emu || !field || modulo == 0u) return;
    *field = (uint8_t)(value % modulo);
    emu->hw.rtc_alarm_fired = false;
}

static void timer_advance(uint16_t *counter,
                           uint8_t control,
                           uint16_t reload,
                           uint8_t prescaler_sel,
                           uint32_t *prescaler_accum,
                           uint32_t cycles,
                           ez80_emulator_t *emu,
                           uint8_t irq_source)
{
    uint32_t divisor;
    uint32_t effective_ticks;
    uint64_t total;
    uint64_t before;
    uint64_t remaining;
    uint32_t period;
    uint64_t overflow_count;

    if ((control & EZ80_TIMER_CONTROL_ENABLE) == 0u) return;

    divisor = timer_prescaler_divisor(prescaler_sel);
    *prescaler_accum += cycles;
    effective_ticks = *prescaler_accum / divisor;
    *prescaler_accum %= divisor;

    if (effective_ticks == 0u) return;

    before = *counter;
    total = before + effective_ticks;
    overflow_count = total >> 16;

    if (overflow_count > 0u) {
        if ((control & EZ80_TIMER_CONTROL_IRQ_ENABLE) != 0u) {
            hardware_raise_irq(emu, irq_source);
        }
        if ((control & EZ80_TIMER_CONTROL_RELOAD) != 0u) {
            remaining = effective_ticks - (0x10000ull - before);
            period = (uint32_t)(0x10000u - reload);
            if (period == 0u) {
                period = 1u;
            }
            *counter = (uint16_t)(reload + (uint16_t)(remaining % period));
        } else {
            *counter = (uint16_t)(total & 0xFFFFu);
        }
    } else {
        *counter = (uint16_t)total;
    }
}

static void hardware_advance_cycles(ez80_emulator_t *emu, uint32_t cycles)
{
    uint32_t rtc_increment;
    if (!emu || cycles == 0u) return;

    emu->hw.total_cycles += cycles;
    pm_note_sleep_cycles(emu, cycles);

    if (pm_mode_allows_timer_advance(emu) &&
        (emu->hw.timer0_control & EZ80_TIMER_CONTROL_ENABLE) != 0u) {
        timer_advance(&emu->hw.timer0_counter,
                      emu->hw.timer0_control,
                      emu->hw.timer0_reload,
                      emu->hw.timer0_prescaler,
                      &emu->hw.timer0_prescaler_accum,
                      cycles, emu, EZ80_IRQ_SOURCE_TIMER0);
    }

    if (pm_mode_allows_timer_advance(emu) &&
        (emu->hw.timer1_control & EZ80_TIMER_CONTROL_ENABLE) != 0u) {
        timer_advance(&emu->hw.timer1_counter,
                      emu->hw.timer1_control,
                      emu->hw.timer1_reload,
                      emu->hw.timer1_prescaler,
                      &emu->hw.timer1_prescaler_accum,
                      cycles, emu, EZ80_IRQ_SOURCE_TIMER1);
    }

    emu->hw.rtc_cycle_accumulator += cycles;
    rtc_increment = emu->hw.rtc_cycle_accumulator / EZ80_RTC_CYCLES_PER_SECOND;
    emu->hw.rtc_cycle_accumulator %= EZ80_RTC_CYCLES_PER_SECOND;
    if (rtc_increment > 0u) {
        emu->hw.rtc_seconds_of_day = (emu->hw.rtc_seconds_of_day + rtc_increment) % (24u * 60u * 60u);
    }

    /* RTC alarm check */
    if (rtc_increment > 0u &&
        (emu->hw.rtc_alarm_control & EZ80_RTC_ALARM_CONTROL_ENABLE) != 0u) {
        uint32_t alarm_sod = (uint32_t)emu->hw.rtc_alarm_hours * 3600u +
                             (uint32_t)emu->hw.rtc_alarm_minutes * 60u +
                             (uint32_t)emu->hw.rtc_alarm_seconds;
        if (emu->hw.rtc_seconds_of_day == alarm_sod && !emu->hw.rtc_alarm_fired) {
            emu->hw.rtc_alarm_fired = true;
            emu->hw.rtc_alarm_fire_count++;
            pm_note_wake_event(emu, EZ80_PM_WAKE_RTC_ALARM, true);
        }
    }

    /* Hardware watchdog timer */
    if ((emu->hw.wdt_control & EZ80_WDT_CONTROL_ENABLE) != 0u) {
        emu->hw.wdt_prescaler_accum += cycles;
        {
            uint32_t wdt_ticks = emu->hw.wdt_prescaler_accum / 4u;
            emu->hw.wdt_prescaler_accum %= 4u;
            if (wdt_ticks > 0u) {
                uint16_t timeout = (emu->hw.wdt_timeout == 0u)
                    ? EZ80_WDT_DEFAULT_TIMEOUT
                    : emu->hw.wdt_timeout;
                uint32_t new_val = (uint32_t)emu->hw.wdt_counter + wdt_ticks;
                if (new_val >= (uint32_t)timeout) {
                    emu->hw.wdt_reset_count++;
                    emu->hw.wdt_last_reset_cycle = cycles;
                    if ((emu->hw.wdt_control & EZ80_WDT_CONTROL_RESET_ON_TIMEOUT) != 0u) {
                        cpu_reset_runtime_state(&emu->cpu);
                    }
                    emu->hw.wdt_counter = 0u;
                    pm_note_wake_event(emu, EZ80_PM_WAKE_WATCHDOG, true);
                } else {
                    emu->hw.wdt_counter = (uint16_t)new_val;
                }
            }
        }
    }

    if (pm_mode_allows_usb_advance(emu)) {
        usb_advance(emu, cycles);
    }
    hardware_sync_ports(emu);
    hardware_sync_mmio(emu);
}

static bool hardware_advances_while_halted(const ez80_emulator_t *emu)
{
    if (!emu) return false;
    if (emu->hw.pm_mode != EZ80_PM_MODE_ACTIVE) return true;
    if (pm_mode_allows_timer_advance(emu) &&
        (emu->hw.timer0_control & EZ80_TIMER_CONTROL_ENABLE) != 0u) return true;
    if (pm_mode_allows_timer_advance(emu) &&
        (emu->hw.timer1_control & EZ80_TIMER_CONTROL_ENABLE) != 0u) return true;
    if ((emu->hw.wdt_control & EZ80_WDT_CONTROL_ENABLE) != 0u) return true;
    if ((emu->hw.rtc_alarm_control & EZ80_RTC_ALARM_CONTROL_ENABLE) != 0u) return true;
    if (pm_mode_allows_usb_advance(emu) &&
        (emu->hw.usb_control & EZ80_USB_CONTROL_ENABLE) != 0u &&
        emu->hw.usb_tx_count > 0u) return true;
    return false;
}

static uint8_t flash_read_byte(const ez80_emulator_t *emu, uint32_t flash_addr)
{
    if (!emu) return 0u;
    flash_addr = mask24(flash_addr);
    if (flash_addr >= EZ80_FLASH_TOTAL_BYTES) return 0u;
    if (flash_addr < EZ80_FLASH_WINDOW_BYTES) {
        return emu->memory.flash_window[flash_addr];
    }
    if (flash_addr >= emu->memory.flash_bytes_loaded) return 0u;
    return g_flash_image[flash_addr];
}

static void flash_erase_range(ez80_emulator_t *emu, uint32_t base_addr, uint32_t length)
{
    uint32_t end_addr;
    uint32_t i;

    if (!emu || length == 0u) return;

    base_addr = mask24(base_addr);
    if (base_addr >= EZ80_FLASH_TOTAL_BYTES) return;

    end_addr = base_addr + length;
    if (end_addr < base_addr || end_addr > EZ80_FLASH_TOTAL_BYTES) {
        end_addr = EZ80_FLASH_TOTAL_BYTES;
    }

    for (i = base_addr; i < end_addr; i++) {
        g_flash_image[i] = 0xFFu;
        if (i < EZ80_FLASH_WINDOW_BYTES) {
            emu->memory.flash_window[i] = 0xFFu;
        }
    }

    if (end_addr > emu->memory.flash_bytes_loaded) {
        emu->memory.flash_bytes_loaded = end_addr;
    }
}

static bool flash_write_allowed(ez80_emulator_t *emu,
                                uint32_t flash_addr,
                                uint8_t requested_value,
                                bool enforce_writeflash_semantics,
                                uint8_t *out_failure_reason,
                                uint8_t *out_failure_port,
                                uint8_t *out_write_status)
{
    uint8_t page;
    uint8_t sector;
    uint8_t unlock;
    uint8_t protect;
    uint8_t before;

#define SET_FLASH_FAILURE(reason, port, status)                                   \
    do {                                                                           \
        if (out_failure_reason) *out_failure_reason = (reason);                   \
        if (out_failure_port) *out_failure_port = (port);                         \
        if (out_write_status) *out_write_status = (status);                       \
    } while (0)

    if (!emu) return false;
    flash_addr = mask24(flash_addr);
    if (out_failure_reason) *out_failure_reason = EZ80_DNGRADE_FAILURE_NONE;
    if (out_failure_port) *out_failure_port = 0u;
    if (out_write_status) *out_write_status = EZ80_WRITEFLASH_STATUS_NONE;
    if (flash_addr >= EZ80_FLASH_TOTAL_BYTES) return false;

    unlock = emu->memory.ports[EZ80_FLASH_UNLOCK_PORT];
    if ((unlock & EZ80_FLASH_UNLOCK_MASK) == 0u) {
        SET_FLASH_FAILURE(EZ80_DNGRADE_FAILURE_FLASH_NOT_UNLOCKED,
                          EZ80_FLASH_UNLOCK_PORT,
                          EZ80_WRITEFLASH_STATUS_FLASH_LOCKED);
        return false;
    }

    if (platform_os_is_58_or_newer(emu) && ((unlock & EZ80_FLASH_UNLOCK_OS58_MASK) == 0u)) {
        SET_FLASH_FAILURE(EZ80_DNGRADE_FAILURE_OS58_PORT28_BIT3_REQUIRED,
                          EZ80_FLASH_UNLOCK_PORT,
                          EZ80_WRITEFLASH_STATUS_FLASH_LOCKED);
        return false;
    }

    page = (uint8_t)(flash_addr >> 16);
    sector = flash_sector_index(flash_addr);

    if (flash_sector_os_locked(emu, sector)) {
        SET_FLASH_FAILURE(EZ80_DNGRADE_FAILURE_FLASH_SECTOR_PROTECTED,
                          EZ80_FLASH_SECTOR_CONTROL_PORT,
                          EZ80_WRITEFLASH_STATUS_SECTOR_PROTECTED);
        return false;
    }

    if (emu->platform.flash_sector_protected[sector]) {
        SET_FLASH_FAILURE(EZ80_DNGRADE_FAILURE_FLASH_SECTOR_PROTECTED,
                          EZ80_FLASH_SECTOR_CONTROL_PORT,
                          EZ80_WRITEFLASH_STATUS_SECTOR_PROTECTED);
        return false;
    }

    if (page == EZ80_FLASH_CERT_PAGE) {
        protect = emu->memory.ports[EZ80_FLASH_PROTECT_PORT];
        if ((protect & EZ80_FLASH_CERT_WRITE_MASK) == 0u) {
            SET_FLASH_FAILURE(EZ80_DNGRADE_FAILURE_FLASH_SECTOR_PROTECTED,
                              EZ80_FLASH_PROTECT_PORT,
                              EZ80_WRITEFLASH_STATUS_SECTOR_PROTECTED);
            return false;
        }
    }

    if (enforce_writeflash_semantics) {
        before = flash_read_byte(emu, flash_addr);
        if ((((uint8_t)~before) & requested_value) != 0u) {
            SET_FLASH_FAILURE(EZ80_DNGRADE_FAILURE_FLASH_REQUIRES_ERASE,
                              EZ80_FLASH_SECTOR_CONTROL_PORT,
                              EZ80_WRITEFLASH_STATUS_REQUIRES_ERASE);
            return false;
        }
    }

#undef SET_FLASH_FAILURE
    return true;
}

static void flash_write_byte(ez80_emulator_t *emu, uint32_t flash_addr, uint8_t value)
{
    if (!emu) return;
    flash_addr = mask24(flash_addr);
    if (flash_addr >= EZ80_FLASH_TOTAL_BYTES) return;

    g_flash_image[flash_addr] = value;
    if ((flash_addr + 1u) > emu->memory.flash_bytes_loaded) {
        emu->memory.flash_bytes_loaded = flash_addr + 1u;
    }
    if (flash_addr < EZ80_FLASH_WINDOW_BYTES) {
        emu->memory.flash_window[flash_addr] = value;
    }
}

static ez80_status_t flash_writeflashbyte(ez80_emulator_t *emu, uint32_t flash_addr, uint8_t value)
{
    uint8_t failure_reason = EZ80_DNGRADE_FAILURE_NONE;
    uint8_t failure_port = 0u;
    uint8_t write_status = EZ80_WRITEFLASH_STATUS_NONE;
    uint8_t before;
    uint8_t after;

    if (!emu) return EZ80_ERR_INVALID_ARG;
    flash_addr = mask24(flash_addr);
    before = flash_read_byte(emu, flash_addr);

    if (!flash_write_allowed(emu,
                             flash_addr,
                             value,
                             true,
                             &failure_reason,
                             &failure_port,
                             &write_status)) {
        platform_set_dngrade_failure(emu, failure_reason, failure_port);
        flash_record_writeflash(emu, write_status, flash_addr, value, before, before);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    after = (uint8_t)(before & value);
    flash_write_byte(emu, flash_addr, after);
    flash_record_writeflash(emu,
                            EZ80_WRITEFLASH_STATUS_SUCCESS,
                            flash_addr,
                            value,
                            before,
                            after);
    platform_set_dngrade_failure(emu, EZ80_DNGRADE_FAILURE_NONE, 0u);
    return EZ80_OK;
}

static size_t memory_fetch_stream(const ez80_emulator_t *emu,
                                  uint32_t pc,
                                  uint8_t *out,
                                  size_t cap)
{
    size_t i;

    if (!emu || !out || cap == 0u) return 0u;

    for (i = 0u; i < cap; i++) {
        uint32_t current = mask24(pc + (uint32_t)i);
        if (addr_in_range(current, EZ80_FLASH_BASE, EZ80_FLASH_TOTAL_BYTES)) {
            out[i] = flash_read_byte(emu, current - EZ80_FLASH_BASE);
        } else if (addr_in_range(current, EZ80_RAM_BASE, EZ80_RAM_BYTES)) {
            uint32_t ram_offset = current - EZ80_RAM_BASE;
            if (ram_offset < EZ80_OS_RAM_POOL_OFFSET ||
                ram_offset >= (EZ80_OS_RAM_POOL_OFFSET + EZ80_OS_RAM_POOL_BYTES)) {
                break;
            }
            out[i] = emu->memory.ram[ram_offset];
        } else {
            break;
        }
    }
    return i;
}

static void cpu_set_reg_a(ez80_cpu_state_t *cpu, uint8_t value)
{
    cpu->af = (cpu->af & ~((uint32_t)0x0000FF00u)) | ((uint32_t)value << 8);
}

static uint8_t cpu_get_reg_a(const ez80_cpu_state_t *cpu)
{
    return (uint8_t)((cpu->af >> 8) & 0xFFu);
}

static uint8_t cpu_get_reg_f(const ez80_cpu_state_t *cpu)
{
    return (uint8_t)(cpu->af & 0xFFu);
}

static void cpu_set_reg_f(ez80_cpu_state_t *cpu, uint8_t value)
{
    cpu->af = (cpu->af & ~((uint32_t)0x000000FFu)) | (uint32_t)value;
}

static uint16_t cpu_get_reg_i16(const ez80_cpu_state_t *cpu)
{
    return cpu->i_reg16;
}

static void cpu_set_reg_i16(ez80_cpu_state_t *cpu, uint16_t value)
{
    cpu->i_reg16 = value;
    cpu->i_reg = (uint8_t)(value & 0x00FFu);
}

static void cpu_set_reg_i_low8(ez80_cpu_state_t *cpu, uint8_t value)
{
    cpu->i_reg = value;
    cpu->i_reg16 = (uint16_t)((cpu->i_reg16 & 0xFF00u) | (uint16_t)value);
}

static uint32_t *cpu_index_ptr(ez80_cpu_state_t *cpu, bool use_iy)
{
    return use_iy ? &cpu->iy : &cpu->ix;
}

static uint16_t cpu_get_index16(const ez80_cpu_state_t *cpu, bool use_iy)
{
    return (uint16_t)((use_iy ? cpu->iy : cpu->ix) & 0xFFFFu);
}

static void cpu_set_index16(ez80_cpu_state_t *cpu, bool use_iy, uint16_t value)
{
    uint32_t *reg = cpu_index_ptr(cpu, use_iy);
    *reg = (*reg & 0x00FF0000u) | (uint32_t)value;
}

static uint8_t cpu_get_index_hi(const ez80_cpu_state_t *cpu, bool use_iy)
{
    uint32_t reg = use_iy ? cpu->iy : cpu->ix;
    return (uint8_t)((reg >> 8) & 0xFFu);
}

static uint8_t cpu_get_index_lo(const ez80_cpu_state_t *cpu, bool use_iy)
{
    uint32_t reg = use_iy ? cpu->iy : cpu->ix;
    return (uint8_t)(reg & 0xFFu);
}

static void cpu_set_index_hi(ez80_cpu_state_t *cpu, bool use_iy, uint8_t value)
{
    uint32_t *reg = cpu_index_ptr(cpu, use_iy);
    *reg = (*reg & ~((uint32_t)0x0000FF00u)) | ((uint32_t)value << 8);
}

static void cpu_set_index_lo(ez80_cpu_state_t *cpu, bool use_iy, uint8_t value)
{
    uint32_t *reg = cpu_index_ptr(cpu, use_iy);
    *reg = (*reg & ~((uint32_t)0x000000FFu)) | (uint32_t)value;
}

static uint32_t *cpu_pair_ptr(ez80_cpu_state_t *cpu, uint8_t pair)
{
    switch (pair & 0x03u) {
    case 0u: return &cpu->bc;
    case 1u: return &cpu->de;
    case 2u: return &cpu->hl;
    default: return &cpu->sp;
    }
}

static uint16_t cpu_get_pair16(const ez80_cpu_state_t *cpu, uint8_t pair)
{
    const uint32_t *reg;
    switch (pair & 0x03u) {
    case 0u: reg = &cpu->bc; break;
    case 1u: reg = &cpu->de; break;
    case 2u: reg = &cpu->hl; break;
    default: reg = &cpu->sp; break;
    }
    return (uint16_t)(*reg & 0xFFFFu);
}

static void cpu_set_pair16(ez80_cpu_state_t *cpu, uint8_t pair, uint16_t value)
{
    uint32_t *reg = cpu_pair_ptr(cpu, pair);
    *reg = (*reg & 0x00FF0000u) | (uint32_t)value;
}

static uint32_t cpu_get_pair_multibyte(const ez80_cpu_state_t *cpu, uint8_t pair)
{
    if (!cpu) return 0u;
    if (cpu_effective_pair_adl(cpu)) {
        return mask24(*cpu_pair_ptr((ez80_cpu_state_t *)(uintptr_t)cpu, pair));
    }
    return (uint32_t)cpu_get_pair16(cpu, pair);
}

static void cpu_set_pair_multibyte(ez80_cpu_state_t *cpu, uint8_t pair, uint32_t value)
{
    if (!cpu) return;
    if (cpu_effective_pair_adl(cpu)) {
        *cpu_pair_ptr(cpu, pair) = mask24(value);
        return;
    }
    cpu_set_pair16(cpu, pair, (uint16_t)value);
}

static uint32_t cpu_get_index_multibyte(const ez80_cpu_state_t *cpu, bool use_iy)
{
    if (!cpu) return 0u;
    if (cpu_effective_pair_adl(cpu)) {
        return mask24(use_iy ? cpu->iy : cpu->ix);
    }
    return (uint32_t)cpu_get_index16(cpu, use_iy);
}

static void cpu_set_index_multibyte(ez80_cpu_state_t *cpu, bool use_iy, uint32_t value)
{
    uint32_t *reg;
    if (!cpu) return;
    if (cpu_effective_pair_adl(cpu)) {
        reg = cpu_index_ptr(cpu, use_iy);
        *reg = mask24(value);
        return;
    }
    cpu_set_index16(cpu, use_iy, (uint16_t)value);
}

static uint32_t cpu_lea_index_disp(const ez80_cpu_state_t *cpu,
                                   uint32_t index_value,
                                   int8_t disp)
{
    if (!cpu) return 0u;
    if (cpu_effective_pair_adl(cpu)) {
        return mask24(mask24(index_value) + (uint32_t)(int32_t)disp);
    }
    return (uint32_t)(uint16_t)((uint16_t)index_value +
                                (uint16_t)(int16_t)disp);
}

static uint32_t cpu_get_pair_width(const ez80_cpu_state_t *cpu, uint8_t pair)
{
    if (!cpu) return 0u;
    if (cpu_effective_adl_mode(cpu)) {
        const uint32_t *reg;
        switch (pair & 0x03u) {
        case 0u: reg = &cpu->bc; break;
        case 1u: reg = &cpu->de; break;
        case 2u: reg = &cpu->hl; break;
        default: reg = &cpu->sp; break;
        }
        return mask24(*reg);
    }
    return (uint32_t)cpu_get_pair16(cpu, pair);
}

static void cpu_set_pair_width(ez80_cpu_state_t *cpu, uint8_t pair, uint32_t value)
{
    if (!cpu) return;
    if (cpu_effective_adl_mode(cpu)) {
        uint32_t *reg = cpu_pair_ptr(cpu, pair);
        *reg = mask24(value);
    } else {
        cpu_set_pair16(cpu, pair, (uint16_t)value);
    }
}

static uint32_t cpu_get_block_counter(const ez80_cpu_state_t *cpu)
{
    if (!cpu) return 0u;
    if (cpu_effective_adl_mode(cpu)) {
        return mask24(cpu->bc);
    }
    return (uint32_t)cpu_get_pair16(cpu, 0u);
}

static void cpu_set_block_counter(ez80_cpu_state_t *cpu, uint32_t value)
{
    if (!cpu) return;
    if (cpu_effective_adl_mode(cpu)) {
        cpu->bc = mask24(value);
    } else {
        cpu_set_pair16(cpu, 0u, (uint16_t)value);
    }
}

static uint32_t cpu_increment_width(const ez80_cpu_state_t *cpu, uint32_t value)
{
    if (cpu && cpu_effective_adl_mode(cpu)) {
        return mask24(value + 1u);
    }
    return (uint16_t)(value + 1u);
}

static uint32_t cpu_decrement_width(const ez80_cpu_state_t *cpu, uint32_t value)
{
    if (cpu && cpu_effective_adl_mode(cpu)) {
        return mask24(value - 1u);
    }
    return (uint16_t)(value - 1u);
}

static uint32_t cpu_decrement_block_counter(const ez80_cpu_state_t *cpu, uint32_t value)
{
    if (cpu && cpu_effective_adl_mode(cpu)) {
        return mask24(value - 1u);
    }
    return (uint16_t)(value - 1u);
}

static uint16_t cpu_get_pushpop16(const ez80_cpu_state_t *cpu, uint8_t qq)
{
    switch (qq & 0x03u) {
    case 0u: return (uint16_t)(cpu->bc & 0xFFFFu);
    case 1u: return (uint16_t)(cpu->de & 0xFFFFu);
    case 2u: return (uint16_t)(cpu->hl & 0xFFFFu);
    default: return (uint16_t)(cpu->af & 0xFFFFu);
    }
}

static void cpu_set_pushpop16(ez80_cpu_state_t *cpu, uint8_t qq, uint16_t value)
{
    switch (qq & 0x03u) {
    case 0u:
        cpu->bc = (cpu->bc & 0x00FF0000u) | (uint32_t)value;
        break;
    case 1u:
        cpu->de = (cpu->de & 0x00FF0000u) | (uint32_t)value;
        break;
    case 2u:
        cpu->hl = (cpu->hl & 0x00FF0000u) | (uint32_t)value;
        break;
    default:
        cpu->af = (cpu->af & 0x00FF0000u) | (uint32_t)value;
        break;
    }
}

static void cpu_swap_pair16(uint32_t *lhs, uint32_t *rhs)
{
    uint16_t lhs16;
    uint16_t rhs16;

    if (!lhs || !rhs) return;
    lhs16 = (uint16_t)(*lhs & 0xFFFFu);
    rhs16 = (uint16_t)(*rhs & 0xFFFFu);
    *lhs = (*lhs & 0x00FF0000u) | (uint32_t)rhs16;
    *rhs = (*rhs & 0x00FF0000u) | (uint32_t)lhs16;
}

static void cpu_swap_pair_multibyte(uint32_t *lhs, uint32_t *rhs, bool multibyte_adl)
{
    uint32_t tmp;

    if (!lhs || !rhs) return;
    if (!multibyte_adl) {
        cpu_swap_pair16(lhs, rhs);
        return;
    }

    tmp = mask24(*lhs);
    *lhs = mask24(*rhs);
    *rhs = tmp;
}

static uint8_t cpu_get_reg8(const ez80_cpu_state_t *cpu, uint8_t reg)
{
    switch (reg & 0x07u) {
    case 0u: return (uint8_t)((cpu->bc >> 8) & 0xFFu);
    case 1u: return (uint8_t)(cpu->bc & 0xFFu);
    case 2u: return (uint8_t)((cpu->de >> 8) & 0xFFu);
    case 3u: return (uint8_t)(cpu->de & 0xFFu);
    case 4u: return (uint8_t)((cpu->hl >> 8) & 0xFFu);
    case 5u: return (uint8_t)(cpu->hl & 0xFFu);
    default: return cpu_get_reg_a(cpu);
    }
}

static void cpu_set_reg8(ez80_cpu_state_t *cpu, uint8_t reg, uint8_t value)
{
    switch (reg & 0x07u) {
    case 0u:
        cpu->bc = (cpu->bc & ~((uint32_t)0x0000FF00u)) | ((uint32_t)value << 8);
        break;
    case 1u:
        cpu->bc = (cpu->bc & ~((uint32_t)0x000000FFu)) | (uint32_t)value;
        break;
    case 2u:
        cpu->de = (cpu->de & ~((uint32_t)0x0000FF00u)) | ((uint32_t)value << 8);
        break;
    case 3u:
        cpu->de = (cpu->de & ~((uint32_t)0x000000FFu)) | (uint32_t)value;
        break;
    case 4u:
        cpu->hl = (cpu->hl & ~((uint32_t)0x0000FF00u)) | ((uint32_t)value << 8);
        break;
    case 5u:
        cpu->hl = (cpu->hl & ~((uint32_t)0x000000FFu)) | (uint32_t)value;
        break;
    default:
        cpu_set_reg_a(cpu, value);
        break;
    }
}

static void cpu_flags_inc8(ez80_cpu_state_t *cpu, uint8_t before, uint8_t after)
{
    uint8_t flags = cpu_get_reg_f(cpu) & FLAG_C;
    if ((after & 0x80u) != 0u) flags |= FLAG_S;
    if (after == 0u) flags |= FLAG_Z;
    if (((before & 0x0Fu) + 1u) > 0x0Fu) flags |= FLAG_H;
    if (before == 0x7Fu) flags |= FLAG_PV;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_dec8(ez80_cpu_state_t *cpu, uint8_t before, uint8_t after)
{
    uint8_t flags = (cpu_get_reg_f(cpu) & FLAG_C) | FLAG_N;
    if ((after & 0x80u) != 0u) flags |= FLAG_S;
    if (after == 0u) flags |= FLAG_Z;
    if ((before & 0x0Fu) == 0u) flags |= FLAG_H;
    if (before == 0x80u) flags |= FLAG_PV;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_add8(ez80_cpu_state_t *cpu,
                           uint8_t lhs,
                           uint8_t rhs,
                           uint8_t carry_in,
                           uint8_t result)
{
    uint16_t wide = (uint16_t)lhs + (uint16_t)rhs + (uint16_t)carry_in;
    uint8_t flags = 0u;
    if ((result & 0x80u) != 0u) flags |= FLAG_S;
    if (result == 0u) flags |= FLAG_Z;
    if (((lhs & 0x0Fu) + (rhs & 0x0Fu) + carry_in) > 0x0Fu) flags |= FLAG_H;
    if (((~(lhs ^ rhs)) & (lhs ^ result) & 0x80u) != 0u) flags |= FLAG_PV;
    if (wide > 0xFFu) flags |= FLAG_C;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_sub8(ez80_cpu_state_t *cpu,
                           uint8_t lhs,
                           uint8_t rhs,
                           uint8_t carry_in,
                           uint8_t result)
{
    uint16_t rhs_total = (uint16_t)rhs + (uint16_t)carry_in;
    uint8_t rhs_effective = (uint8_t)rhs_total;
    uint8_t flags = FLAG_N;
    if ((result & 0x80u) != 0u) flags |= FLAG_S;
    if (result == 0u) flags |= FLAG_Z;
    if ((uint8_t)(lhs & 0x0Fu) < (uint8_t)((rhs & 0x0Fu) + carry_in)) flags |= FLAG_H;
    if (((lhs ^ rhs_effective) & (lhs ^ result) & 0x80u) != 0u) flags |= FLAG_PV;
    if ((uint16_t)lhs < rhs_total) flags |= FLAG_C;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_logic8(ez80_cpu_state_t *cpu, uint8_t result, bool set_h)
{
    uint8_t flags = 0u;
    if ((result & 0x80u) != 0u) flags |= FLAG_S;
    if (result == 0u) flags |= FLAG_Z;
    if (set_h) flags |= FLAG_H;
    if (parity_even8(result)) flags |= FLAG_PV;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_add_hl_ss(ez80_cpu_state_t *cpu, uint16_t hl, uint16_t ss, uint32_t sum)
{
    uint8_t flags = (uint8_t)(cpu_get_reg_f(cpu) & (FLAG_S | FLAG_Z | FLAG_PV));
    if (((hl & 0x0FFFu) + (ss & 0x0FFFu)) > 0x0FFFu) flags |= FLAG_H;
    if (sum > 0xFFFFu) flags |= FLAG_C;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_adc_hl_ss_16(ez80_cpu_state_t *cpu,
                                   uint16_t hl,
                                   uint16_t ss,
                                   uint8_t carry_in,
                                   uint16_t result,
                                   uint32_t sum)
{
    uint8_t flags = 0u;
    if ((result & 0x8000u) != 0u) flags |= FLAG_S;
    if (result == 0u) flags |= FLAG_Z;
    if (((hl & 0x0FFFu) + (ss & 0x0FFFu) + carry_in) > 0x0FFFu) flags |= FLAG_H;
    if (((~(hl ^ ss)) & (hl ^ result) & 0x8000u) != 0u) flags |= FLAG_PV;
    if (sum > 0xFFFFu) flags |= FLAG_C;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_adc_hl_ss_24(ez80_cpu_state_t *cpu,
                                   uint32_t hl,
                                   uint32_t ss,
                                   uint8_t carry_in,
                                   uint32_t result,
                                   uint64_t sum)
{
    uint8_t flags = 0u;
    if ((result & 0x800000u) != 0u) flags |= FLAG_S;
    if (result == 0u) flags |= FLAG_Z;
    if (((hl & 0x000FFFu) + (ss & 0x000FFFu) + carry_in) > 0x000FFFu) flags |= FLAG_H;
    if (((~(hl ^ ss)) & (hl ^ result) & 0x800000u) != 0u) flags |= FLAG_PV;
    if (sum > 0xFFFFFFu) flags |= FLAG_C;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_sbc_hl_ss_16(ez80_cpu_state_t *cpu,
                                   uint16_t hl,
                                   uint16_t ss,
                                   uint8_t carry_in,
                                   uint16_t result)
{
    uint32_t rhs_total = (uint32_t)ss + (uint32_t)carry_in;
    uint16_t rhs_effective = (uint16_t)rhs_total;
    uint8_t flags = FLAG_N;
    if ((result & 0x8000u) != 0u) flags |= FLAG_S;
    if (result == 0u) flags |= FLAG_Z;
    if ((uint16_t)(hl & 0x0FFFu) < (uint16_t)((ss & 0x0FFFu) + carry_in)) flags |= FLAG_H;
    if (((hl ^ rhs_effective) & (hl ^ result) & 0x8000u) != 0u) flags |= FLAG_PV;
    if ((uint32_t)hl < rhs_total) flags |= FLAG_C;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_sbc_hl_ss_24(ez80_cpu_state_t *cpu,
                                   uint32_t hl,
                                   uint32_t ss,
                                   uint8_t carry_in,
                                   uint32_t result)
{
    uint64_t rhs_total = (uint64_t)ss + (uint64_t)carry_in;
    uint32_t rhs_effective = (uint32_t)(rhs_total & 0xFFFFFFu);
    uint8_t flags = FLAG_N;
    if ((result & 0x800000u) != 0u) flags |= FLAG_S;
    if (result == 0u) flags |= FLAG_Z;
    if ((hl & 0x000FFFu) < ((ss & 0x000FFFu) + carry_in)) flags |= FLAG_H;
    if (((hl ^ rhs_effective) & (hl ^ result) & 0x800000u) != 0u) flags |= FLAG_PV;
    if ((uint64_t)hl < rhs_total) flags |= FLAG_C;
    cpu_set_reg_f(cpu, flags);
}

static bool cpu_condition_true(const ez80_cpu_state_t *cpu, uint8_t cond)
{
    uint8_t flags = cpu_get_reg_f(cpu);
    switch (cond & 0x07u) {
    case 0u: return (flags & FLAG_Z) == 0u; /* NZ */
    case 1u: return (flags & FLAG_Z) != 0u; /* Z */
    case 2u: return (flags & FLAG_C) == 0u; /* NC */
    case 3u: return (flags & FLAG_C) != 0u; /* C */
    case 4u: return (flags & FLAG_PV) == 0u; /* PO */
    case 5u: return (flags & FLAG_PV) != 0u; /* PE */
    case 6u: return (flags & FLAG_S) == 0u; /* P */
    default: return (flags & FLAG_S) != 0u; /* M */
    }
}

static uint32_t calc_index_disp_addr(const ez80_cpu_state_t *cpu, uint32_t index_value, int8_t disp)
{
    uint32_t page = mask24(index_value) & 0x00FF0000u;
    uint16_t low16 = (uint16_t)(index_value & 0xFFFFu);
    uint16_t wrapped = (uint16_t)((int32_t)low16 + (int32_t)disp);
    return map_data_addr_cpu(cpu, page | (uint32_t)wrapped);
}

static ez80_status_t cpu_read_reg_or_hl(ez80_emulator_t *emu, uint8_t reg, uint8_t *out)
{
    if (!emu || !out) return EZ80_ERR_INVALID_ARG;
    if ((reg & 0x07u) == 6u) {
        return ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, emu->cpu.hl), out);
    }
    *out = cpu_get_reg8(&emu->cpu, reg);
    return EZ80_OK;
}

static ez80_status_t cpu_write_reg_or_hl(ez80_emulator_t *emu, uint8_t reg, uint8_t value)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if ((reg & 0x07u) == 6u) {
        return ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, emu->cpu.hl), value);
    }
    cpu_set_reg8(&emu->cpu, reg, value);
    return EZ80_OK;
}

static void cpu_execute_alu_a(ez80_cpu_state_t *cpu, uint8_t group, uint8_t rhs)
{
    uint8_t a = cpu_get_reg_a(cpu);
    uint8_t carry = (cpu_get_reg_f(cpu) & FLAG_C) ? 1u : 0u;
    uint8_t result;

    switch (group & 0x07u) {
    case 0u:
        result = (uint8_t)(a + rhs);
        cpu_set_reg_a(cpu, result);
        cpu_flags_add8(cpu, a, rhs, 0u, result);
        break;

    case 1u:
        result = (uint8_t)(a + rhs + carry);
        cpu_set_reg_a(cpu, result);
        cpu_flags_add8(cpu, a, rhs, carry, result);
        break;

    case 2u:
        result = (uint8_t)(a - rhs);
        cpu_set_reg_a(cpu, result);
        cpu_flags_sub8(cpu, a, rhs, 0u, result);
        break;

    case 3u:
        result = (uint8_t)(a - rhs - carry);
        cpu_set_reg_a(cpu, result);
        cpu_flags_sub8(cpu, a, rhs, carry, result);
        break;

    case 4u:
        result = (uint8_t)(a & rhs);
        cpu_set_reg_a(cpu, result);
        cpu_flags_logic8(cpu, result, true);
        break;

    case 5u:
        result = (uint8_t)(a ^ rhs);
        cpu_set_reg_a(cpu, result);
        cpu_flags_logic8(cpu, result, false);
        break;

    case 6u:
        result = (uint8_t)(a | rhs);
        cpu_set_reg_a(cpu, result);
        cpu_flags_logic8(cpu, result, false);
        break;

    default:
        result = (uint8_t)(a - rhs);
        cpu_flags_sub8(cpu, a, rhs, 0u, result);
        break;
    }
}

static void cpu_flags_rotate_acc(ez80_cpu_state_t *cpu, bool carry_set)
{
    uint8_t flags = (uint8_t)(cpu_get_reg_f(cpu) & (FLAG_S | FLAG_Z | FLAG_PV));
    if (carry_set) flags |= FLAG_C;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_ld_a_from_ir(ez80_cpu_state_t *cpu, uint8_t value)
{
    uint8_t flags = (uint8_t)(cpu_get_reg_f(cpu) & FLAG_C);
    if ((value & 0x80u) != 0u) flags |= FLAG_S;
    if (value == 0u) flags |= FLAG_Z;
    if (cpu->iff2) flags |= FLAG_PV;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_ld_hl_from_i(ez80_cpu_state_t *cpu, uint16_t value)
{
    uint8_t flags = (uint8_t)(cpu_get_reg_f(cpu) & FLAG_C);
    if ((value & 0x8000u) != 0u) flags |= FLAG_S;
    if (value == 0u) flags |= FLAG_Z;
    if (cpu->iff2) flags |= FLAG_PV;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_execute_daa(ez80_cpu_state_t *cpu)
{
    uint8_t a = cpu_get_reg_a(cpu);
    uint8_t flags_in = cpu_get_reg_f(cpu);
    uint8_t correction = 0u;
    uint8_t result;
    bool subtract = (flags_in & FLAG_N) != 0u;
    bool carry = (flags_in & FLAG_C) != 0u;
    bool half = (flags_in & FLAG_H) != 0u;
    uint8_t flags = 0u;

    if (half || (!subtract && ((a & 0x0Fu) > 9u))) {
        correction |= 0x06u;
    }
    if (carry || (!subtract && a > 0x99u)) {
        correction |= 0x60u;
        carry = true;
    }

    result = subtract ? (uint8_t)(a - correction) : (uint8_t)(a + correction);

    if ((result & 0x80u) != 0u) flags |= FLAG_S;
    if (result == 0u) flags |= FLAG_Z;
    if (((a ^ result ^ correction) & 0x10u) != 0u) flags |= FLAG_H;
    if (parity_even8(result)) flags |= FLAG_PV;
    if (subtract) flags |= FLAG_N;
    if (carry) flags |= FLAG_C;

    cpu_set_reg_a(cpu, result);
    cpu_set_reg_f(cpu, flags);
}

static uint8_t cb_shift_rotate(ez80_cpu_state_t *cpu, uint8_t y, uint8_t value)
{
    uint8_t old_carry = (cpu_get_reg_f(cpu) & FLAG_C) ? 1u : 0u;
    uint8_t new_carry = 0u;
    uint8_t result = value;
    uint8_t flags;

    switch (y & 0x07u) {
    case 0u:
        new_carry = (uint8_t)((value >> 7) & 0x01u);
        result = (uint8_t)((value << 1) | new_carry);
        break;

    case 1u:
        new_carry = (uint8_t)(value & 0x01u);
        result = (uint8_t)((value >> 1) | (new_carry << 7));
        break;

    case 2u:
        new_carry = (uint8_t)((value >> 7) & 0x01u);
        result = (uint8_t)((value << 1) | old_carry);
        break;

    case 3u:
        new_carry = (uint8_t)(value & 0x01u);
        result = (uint8_t)((value >> 1) | (old_carry << 7));
        break;

    case 4u:
        new_carry = (uint8_t)((value >> 7) & 0x01u);
        result = (uint8_t)(value << 1);
        break;

    case 5u:
        new_carry = (uint8_t)(value & 0x01u);
        result = (uint8_t)((value >> 1) | (value & 0x80u));
        break;

    case 6u:
        new_carry = (uint8_t)((value >> 7) & 0x01u);
        result = (uint8_t)((value << 1) | 0x01u);
        break;

    default:
        new_carry = (uint8_t)(value & 0x01u);
        result = (uint8_t)(value >> 1);
        break;
    }

    flags = 0u;
    if ((result & 0x80u) != 0u) flags |= FLAG_S;
    if (result == 0u) flags |= FLAG_Z;
    if (parity_even8(result)) flags |= FLAG_PV;
    if (new_carry != 0u) flags |= FLAG_C;
    cpu_set_reg_f(cpu, flags);
    return result;
}

static ez80_status_t stack_push8(ez80_emulator_t *emu, uint8_t value)
{
    uint32_t next_sp;
    ez80_status_t status;

    if (!emu) return EZ80_ERR_INVALID_ARG;
    next_sp = mask24(emu->cpu.sp - 1u);
    status = ez80_memory_write8(emu, next_sp, value);
    if (status != EZ80_OK) return status;
    emu->cpu.sp = next_sp;
    return EZ80_OK;
}

static ez80_status_t stack_pop8(ez80_emulator_t *emu, uint8_t *out)
{
    ez80_status_t status;
    uint8_t value;
    uint32_t sp;

    if (!emu || !out) return EZ80_ERR_INVALID_ARG;
    sp = mask24(emu->cpu.sp);
    status = ez80_memory_read8(emu, sp, &value);
    if (status != EZ80_OK) return status;
    emu->cpu.sp = mask24(sp + 1u);
    *out = value;
    return EZ80_OK;
}

static ez80_status_t stack_push16(ez80_emulator_t *emu, uint16_t value)
{
    ez80_status_t status;

    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!memory_addr_writable(mask24(emu->cpu.sp - 1u)) ||
        !memory_addr_writable(mask24(emu->cpu.sp - 2u))) {
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    status = stack_push8(emu, (uint8_t)((value >> 8) & 0xFFu));
    if (status != EZ80_OK) return status;
    return stack_push8(emu, (uint8_t)(value & 0xFFu));
}

static ez80_status_t stack_pop16(ez80_emulator_t *emu, uint16_t *out)
{
    ez80_status_t status;
    uint8_t lo;
    uint8_t hi;

    if (!emu || !out) return EZ80_ERR_INVALID_ARG;
    if (!memory_addr_readable(mask24(emu->cpu.sp)) ||
        !memory_addr_readable(mask24(emu->cpu.sp + 1u))) {
        return EZ80_ERR_OUT_OF_BOUNDS;
    }
    status = stack_pop8(emu, &lo);
    if (status != EZ80_OK) return status;
    status = stack_pop8(emu, &hi);
    if (status != EZ80_OK) return status;
    *out = (uint16_t)lo | ((uint16_t)hi << 8);
    return EZ80_OK;
}

static ez80_status_t stack_push24(ez80_emulator_t *emu, uint32_t value)
{
    ez80_status_t status;

    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!memory_addr_writable(mask24(emu->cpu.sp - 1u)) ||
        !memory_addr_writable(mask24(emu->cpu.sp - 2u)) ||
        !memory_addr_writable(mask24(emu->cpu.sp - 3u))) {
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    status = stack_push8(emu, (uint8_t)((value >> 16) & 0xFFu));
    if (status != EZ80_OK) return status;
    status = stack_push8(emu, (uint8_t)((value >> 8) & 0xFFu));
    if (status != EZ80_OK) return status;
    return stack_push8(emu, (uint8_t)(value & 0xFFu));
}

static ez80_status_t stack_pop24(ez80_emulator_t *emu, uint32_t *out)
{
    ez80_status_t status;
    uint8_t b0;
    uint8_t b1;
    uint8_t b2;

    if (!emu || !out) return EZ80_ERR_INVALID_ARG;
    if (!memory_addr_readable(mask24(emu->cpu.sp)) ||
        !memory_addr_readable(mask24(emu->cpu.sp + 1u)) ||
        !memory_addr_readable(mask24(emu->cpu.sp + 2u))) {
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    status = stack_pop8(emu, &b0);
    if (status != EZ80_OK) return status;
    status = stack_pop8(emu, &b1);
    if (status != EZ80_OK) return status;
    status = stack_pop8(emu, &b2);
    if (status != EZ80_OK) return status;

    *out = mask24((uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16));
    return EZ80_OK;
}

static ez80_status_t stack_push_pc(ez80_emulator_t *emu, uint32_t value)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (cpu_effective_adl_mode(&emu->cpu)) {
        return stack_push24(emu, value);
    }
    return stack_push16(emu, (uint16_t)(value & 0xFFFFu));
}

static ez80_status_t stack_pop_pc(ez80_emulator_t *emu, uint32_t *out)
{
    if (!emu || !out) return EZ80_ERR_INVALID_ARG;
    if (cpu_effective_adl_mode(&emu->cpu)) {
        return stack_pop24(emu, out);
    }
    {
        uint16_t value16 = 0u;
        ez80_status_t status = stack_pop16(emu, &value16);
        if (status != EZ80_OK) return status;
        *out = (uint32_t)value16;
    }
    return EZ80_OK;
}

static ez80_status_t stack_push_qq(ez80_emulator_t *emu, uint8_t qq)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    qq &= 0x03u;
    if (cpu_effective_adl_mode(&emu->cpu) && qq != 3u) {
        uint32_t value24 = mask24(*cpu_pair_ptr(&emu->cpu, qq));
        return stack_push24(emu, value24);
    }
    return stack_push16(emu, cpu_get_pushpop16(&emu->cpu, qq));
}

static ez80_status_t stack_pop_qq(ez80_emulator_t *emu, uint8_t qq)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    qq &= 0x03u;
    if (cpu_effective_adl_mode(&emu->cpu) && qq != 3u) {
        uint32_t value24 = 0u;
        ez80_status_t status = stack_pop24(emu, &value24);
        if (status != EZ80_OK) return status;
        *cpu_pair_ptr(&emu->cpu, qq) = value24;
        return EZ80_OK;
    }
    {
        uint16_t value16 = 0u;
        ez80_status_t status = stack_pop16(emu, &value16);
        if (status != EZ80_OK) return status;
        cpu_set_pushpop16(&emu->cpu, qq, value16);
    }
    return EZ80_OK;
}

static ez80_status_t cpu_exchange_pair16_with_stack(ez80_emulator_t *emu, uint32_t *reg)
{
    ez80_status_t status;
    uint32_t sp;
    uint8_t width;
    uint8_t bytes[3] = { 0u, 0u, 0u };
    uint8_t i;
    uint32_t reg_value = 0u;
    uint32_t stack_value = 0u;
    bool adl_mode;

    if (!emu || !reg) return EZ80_ERR_INVALID_ARG;
    adl_mode = cpu_effective_adl_mode(&emu->cpu);
    width = adl_mode ? 3u : 2u;
    sp = mask24(emu->cpu.sp);
    for (i = 0u; i < width; i++) {
        if (!memory_addr_readable(mask24(sp + i)) ||
            !memory_addr_writable(mask24(sp + i))) {
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
    }

    for (i = 0u; i < width; i++) {
        status = ez80_memory_read8(emu, mask24(sp + i), &bytes[i]);
        if (status != EZ80_OK) return status;
        stack_value |= ((uint32_t)bytes[i] << (8u * i));
    }

    reg_value = adl_mode ? mask24(*reg) : (uint32_t)(*reg & 0xFFFFu);
    for (i = 0u; i < width; i++) {
        status = ez80_memory_write8(emu, mask24(sp + i), (uint8_t)((reg_value >> (8u * i)) & 0xFFu));
        if (status != EZ80_OK) return status;
    }

    if (adl_mode) {
        *reg = stack_value & 0x00FFFFFFu;
    } else {
        *reg = (*reg & 0x00FF0000u) | (stack_value & 0x0000FFFFu);
    }
    return EZ80_OK;
}

static ez80_status_t execute_cb_prefix(ez80_emulator_t *emu,
                                       const ez80_instruction_t *instruction,
                                       uint32_t next_pc)
{
    ez80_status_t status;
    uint8_t op = instruction->bytes[1];
    uint8_t x = (uint8_t)(op >> 6);
    uint8_t y = (uint8_t)((op >> 3) & 0x07u);
    uint8_t z = (uint8_t)(op & 0x07u);
    uint8_t value = 0u;
    uint8_t result = 0u;

    status = cpu_read_reg_or_hl(emu, z, &value);
    if (status != EZ80_OK) return status;

    switch (x) {
    case 0u:
        result = cb_shift_rotate(&emu->cpu, y, value);
        status = cpu_write_reg_or_hl(emu, z, result);
        if (status != EZ80_OK) return status;
        break;

    case 1u: {
        uint8_t bit_mask = (uint8_t)(1u << y);
        uint8_t bit_set = (uint8_t)(value & bit_mask);
        uint8_t flags = (uint8_t)(cpu_get_reg_f(&emu->cpu) & FLAG_C);
        flags |= FLAG_H;
        if (bit_set == 0u) {
            flags |= (FLAG_Z | FLAG_PV);
        }
        if (y == 7u && bit_set != 0u) {
            flags |= FLAG_S;
        }
        cpu_set_reg_f(&emu->cpu, flags);
        break;
    }

    case 2u:
        result = (uint8_t)(value & (uint8_t)~(1u << y));
        status = cpu_write_reg_or_hl(emu, z, result);
        if (status != EZ80_OK) return status;
        break;

    default:
        result = (uint8_t)(value | (uint8_t)(1u << y));
        status = cpu_write_reg_or_hl(emu, z, result);
        if (status != EZ80_OK) return status;
        break;
    }

    emu->cpu.pc = next_pc;
    return EZ80_OK;
}

static ez80_status_t execute_ddfd_cb_prefix(ez80_emulator_t *emu,
                                            const ez80_instruction_t *instruction,
                                            uint32_t next_pc,
                                            bool use_iy)
{
    ez80_status_t status;
    int8_t disp = (int8_t)instruction->bytes[2];
    uint8_t op = instruction->bytes[3];
    uint8_t x = (uint8_t)(op >> 6);
    uint8_t y = (uint8_t)((op >> 3) & 0x07u);
    uint8_t z = (uint8_t)(op & 0x07u);
    uint32_t index_addr = calc_index_disp_addr(&emu->cpu, *cpu_index_ptr(&emu->cpu, use_iy), disp);
    uint8_t value = 0u;
    uint8_t result = 0u;

    status = ez80_memory_read8(emu, index_addr, &value);
    if (status != EZ80_OK) return status;

    switch (x) {
    case 0u:
        result = cb_shift_rotate(&emu->cpu, y, value);
        status = ez80_memory_write8(emu, index_addr, result);
        if (status != EZ80_OK) return status;
        if (z != 6u) {
            cpu_set_reg8(&emu->cpu, z, result);
        }
        break;

    case 1u: {
        uint8_t bit_mask = (uint8_t)(1u << y);
        uint8_t bit_set = (uint8_t)(value & bit_mask);
        uint8_t flags = (uint8_t)(cpu_get_reg_f(&emu->cpu) & FLAG_C);
        flags |= FLAG_H;
        if (bit_set == 0u) {
            flags |= (FLAG_Z | FLAG_PV);
        }
        if (y == 7u && bit_set != 0u) {
            flags |= FLAG_S;
        }
        cpu_set_reg_f(&emu->cpu, flags);
        break;
    }

    case 2u:
        result = (uint8_t)(value & (uint8_t)~(1u << y));
        status = ez80_memory_write8(emu, index_addr, result);
        if (status != EZ80_OK) return status;
        if (z != 6u) {
            cpu_set_reg8(&emu->cpu, z, result);
        }
        break;

    default:
        result = (uint8_t)(value | (uint8_t)(1u << y));
        status = ez80_memory_write8(emu, index_addr, result);
        if (status != EZ80_OK) return status;
        if (z != 6u) {
            cpu_set_reg8(&emu->cpu, z, result);
        }
        break;
    }

    emu->cpu.pc = next_pc;
    return EZ80_OK;
}

static void cpu_flags_block_transfer(ez80_cpu_state_t *cpu, uint32_t bc)
{
    uint8_t flags = (uint8_t)(cpu_get_reg_f(cpu) & (FLAG_S | FLAG_Z | FLAG_C));
    if (bc != 0u) flags |= FLAG_PV;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_block_compare(ez80_cpu_state_t *cpu,
                                    uint8_t lhs,
                                    uint8_t rhs,
                                    uint8_t result,
                                    uint32_t bc_after)
{
    uint8_t flags = (uint8_t)(cpu_get_reg_f(cpu) & FLAG_C);
    flags |= FLAG_N;
    if ((result & 0x80u) != 0u) flags |= FLAG_S;
    if (result == 0u) flags |= FLAG_Z;
    if (((lhs ^ rhs ^ result) & 0x10u) != 0u) flags |= FLAG_H;
    if (bc_after != 0u) flags |= FLAG_PV;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_block_io_b(ez80_cpu_state_t *cpu, uint8_t transferred, uint8_t b_after)
{
    uint8_t flags = (uint8_t)(cpu_get_reg_f(cpu) & (FLAG_S | FLAG_H | FLAG_PV | FLAG_C));
    if (b_after == 0u) flags |= FLAG_Z;
    if ((transferred & 0x80u) != 0u) flags |= FLAG_N;
    cpu_set_reg_f(cpu, flags);
}

static void cpu_flags_block_io_bc(ez80_cpu_state_t *cpu, uint8_t transferred, uint32_t bc_after)
{
    uint8_t flags = (uint8_t)(cpu_get_reg_f(cpu) & (FLAG_S | FLAG_H | FLAG_PV | FLAG_C));
    if (bc_after == 0u) flags |= FLAG_Z;
    if ((transferred & 0x80u) != 0u) flags |= FLAG_N;
    cpu_set_reg_f(cpu, flags);
}

static ez80_status_t execute_ed_prefix(ez80_emulator_t *emu,
                                       const ez80_instruction_t *instruction,
                                       uint32_t pc,
                                       uint32_t next_pc)
{
    ez80_status_t status;
    uint8_t op = instruction->bytes[1];

    switch (op) {

    /* --- eZ80: IN0 r,(n) — ED 00+r*8 (r=B,C,D,E,H,L,F,A) --- */
    case 0x00u:
    case 0x08u:
    case 0x10u:
    case 0x18u:
    case 0x20u:
    case 0x28u:
    case 0x38u: {
        uint8_t r = (uint8_t)((op >> 3) & 0x07u);
        uint16_t port_addr = (uint16_t)instruction->bytes[2];
        uint8_t value = port_read(emu, port_addr);
        uint8_t flags = cpu_get_reg_f(&emu->cpu) & FLAG_C;
        cpu_set_reg8(&emu->cpu, r, value);
        if ((value & 0x80u) != 0u) flags |= FLAG_S;
        if (value == 0u) flags |= FLAG_Z;
        if (parity_even8(value)) flags |= FLAG_PV;
        cpu_set_reg_f(&emu->cpu, flags);
        io_record_last_in(emu, port_addr, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* Canonical: ED 30 = LD IY,(HL). */
    case 0x30u: {
        bool multibyte_adl = cpu_effective_pair_adl(&emu->cpu);
        uint32_t raw_addr = emu->cpu.hl;
        uint32_t value = 0u;
        uint8_t byte0 = 0u;
        uint8_t byte1 = 0u;
        uint8_t byte2 = 0u;
        status = ez80_memory_read8(emu,
                                   map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 0u, multibyte_adl),
                                   &byte0);
        if (status != EZ80_OK) return status;
        status = ez80_memory_read8(emu,
                                   map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 1u, multibyte_adl),
                                   &byte1);
        if (status != EZ80_OK) return status;
        value = (uint32_t)byte0 | ((uint32_t)byte1 << 8);
        if (multibyte_adl) {
            status = ez80_memory_read8(emu,
                                       map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 2u, true),
                                       &byte2);
            if (status != EZ80_OK) return status;
            value |= ((uint32_t)byte2 << 16);
        }
        cpu_set_index_multibyte(&emu->cpu, true, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: OUT0 (n),r — ED 01+r*8 (r=B,C,D,E,H,L,_,A) --- */
    case 0x01u:
    case 0x09u:
    case 0x11u:
    case 0x19u:
    case 0x21u:
    case 0x29u:
    case 0x39u: {
        uint8_t r = (uint8_t)((op >> 3) & 0x07u);
        uint16_t port_addr = (uint16_t)instruction->bytes[2];
        uint8_t value = cpu_get_reg8(&emu->cpu, r);
        port_write(emu, port_addr, value);
        io_record_last_out(emu, port_addr, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* OUT0 (n),0 — ED 31: output zero to port */
    /* Undocumented alias retained: ED 31 = OUT0 (n),0. */
    case 0x31u: {
        uint16_t port_addr = (uint16_t)instruction->bytes[2];
        port_write(emu, port_addr, 0u);
        io_record_last_out(emu, port_addr, 0u);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA BC,IX+d (ED 02) --- */
    case 0x02u: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, false),
                                             disp);
        cpu_set_pair_multibyte(&emu->cpu, 0u, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA BC,IY+d (ED 03) --- */
    case 0x03u: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, true),
                                             disp);
        cpu_set_pair_multibyte(&emu->cpu, 0u, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA DE,IX+d (ED 12) --- */
    case 0x12u: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, false),
                                             disp);
        cpu_set_pair_multibyte(&emu->cpu, 1u, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA DE,IY+d (ED 13) --- */
    case 0x13u: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, true),
                                             disp);
        cpu_set_pair_multibyte(&emu->cpu, 1u, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA HL,IX+d (ED 22) --- */
    case 0x22u: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, false),
                                             disp);
        cpu_set_pair_multibyte(&emu->cpu, 2u, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA HL,IY+d (ED 23) --- */
    case 0x23u: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, true),
                                             disp);
        cpu_set_pair_multibyte(&emu->cpu, 2u, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x06u:
    case 0x07u:
    case 0x16u:
    case 0x17u:
    case 0x26u:
    case 0x27u: {
        uint8_t ss = (uint8_t)(op >> 4);
        bool multibyte_adl = cpu_effective_pair_adl(&emu->cpu);
        uint32_t raw_addr = emu->cpu.hl;
        uint32_t value = 0u;
        uint8_t byte0 = 0u;
        uint8_t byte1 = 0u;
        uint8_t byte2 = 0u;
        status = ez80_memory_read8(emu,
                                   map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 0u, multibyte_adl),
                                   &byte0);
        if (status != EZ80_OK) return status;
        status = ez80_memory_read8(emu,
                                   map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 1u, multibyte_adl),
                                   &byte1);
        if (status != EZ80_OK) return status;
        value = (uint32_t)byte0 | ((uint32_t)byte1 << 8);
        if (multibyte_adl) {
            status = ez80_memory_read8(emu,
                                       map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 2u, true),
                                       &byte2);
            if (status != EZ80_OK) return status;
            value |= ((uint32_t)byte2 << 16);
        }
        cpu_set_pair_multibyte(&emu->cpu, ss, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x0Eu:
    case 0x0Fu:
    case 0x1Eu:
    case 0x1Fu:
    case 0x2Eu:
    case 0x2Fu: {
        uint8_t ss = (uint8_t)(op >> 4);
        bool multibyte_adl = cpu_effective_pair_adl(&emu->cpu);
        uint32_t value = cpu_get_pair_multibyte(&emu->cpu, ss);
        uint32_t raw_addr = emu->cpu.hl;
        status = ez80_memory_write8(emu,
                                    map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 0u, multibyte_adl),
                                    (uint8_t)(value & 0xFFu));
        if (status != EZ80_OK) return status;
        status = ez80_memory_write8(emu,
                                    map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 1u, multibyte_adl),
                                    (uint8_t)((value >> 8) & 0xFFu));
        if (status != EZ80_OK) return status;
        if (multibyte_adl) {
            status = ez80_memory_write8(emu,
                                        map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 2u, true),
                                        (uint8_t)((value >> 16) & 0xFFu));
            if (status != EZ80_OK) return status;
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x37u: {
        bool multibyte_adl = cpu_effective_pair_adl(&emu->cpu);
        uint32_t raw_addr = emu->cpu.hl;
        uint32_t value = 0u;
        uint8_t byte0 = 0u;
        uint8_t byte1 = 0u;
        uint8_t byte2 = 0u;
        status = ez80_memory_read8(emu,
                                   map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 0u, multibyte_adl),
                                   &byte0);
        if (status != EZ80_OK) return status;
        status = ez80_memory_read8(emu,
                                   map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 1u, multibyte_adl),
                                   &byte1);
        if (status != EZ80_OK) return status;
        value = (uint32_t)byte0 | ((uint32_t)byte1 << 8);
        if (multibyte_adl) {
            status = ez80_memory_read8(emu,
                                       map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 2u, true),
                                       &byte2);
            if (status != EZ80_OK) return status;
            value |= ((uint32_t)byte2 << 16);
        }
        cpu_set_index_multibyte(&emu->cpu, false, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x3Eu:
    case 0x3Fu: {
        bool store_iy = (op == 0x3Fu);
        bool multibyte_adl = cpu_effective_pair_adl(&emu->cpu);
        uint32_t value = cpu_get_index_multibyte(&emu->cpu, store_iy);
        uint32_t raw_addr = emu->cpu.hl;
        status = ez80_memory_write8(emu,
                                    map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 0u, multibyte_adl),
                                    (uint8_t)(value & 0xFFu));
        if (status != EZ80_OK) return status;
        status = ez80_memory_write8(emu,
                                    map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 1u, multibyte_adl),
                                    (uint8_t)((value >> 8) & 0xFFu));
        if (status != EZ80_OK) return status;
        if (multibyte_adl) {
            status = ez80_memory_write8(emu,
                                        map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 2u, true),
                                        (uint8_t)((value >> 16) & 0xFFu));
            if (status != EZ80_OK) return status;
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA BC,IY+d (ED 0A) --- */
    case 0x0Au: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, true),
                                             disp);
        cpu_set_pair_multibyte(&emu->cpu, 0u, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA DE,IY+d (ED 0B) --- */
    case 0x0Bu: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, true),
                                             disp);
        cpu_set_pair_multibyte(&emu->cpu, 1u, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA HL,IY+d (ED 3A) --- */
    case 0x3Au: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, true),
                                             disp);
        cpu_set_pair_multibyte(&emu->cpu, 2u, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA IY,IY+d (ED 3B) --- */
    case 0x3Bu: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, true),
                                             disp);
        cpu_set_index_multibyte(&emu->cpu, true, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: TST A,r — ED 04+r*8 --- */
    case 0x04u:
    case 0x0Cu:
    case 0x14u:
    case 0x1Cu:
    case 0x24u:
    case 0x2Cu:
    case 0x3Cu: {
        uint8_t r = (uint8_t)((op >> 3) & 0x07u);
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t rhs = cpu_get_reg8(&emu->cpu, r);
        uint8_t result = (uint8_t)(a & rhs);
        cpu_flags_logic8(&emu->cpu, result, true);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* TST A,(HL) — ED 34 */
    case 0x34u: {
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t value = 0u;
        uint8_t result;
        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, emu->cpu.hl), &value);
        if (status != EZ80_OK) return status;
        result = (uint8_t)(a & value);
        cpu_flags_logic8(&emu->cpu, result, true);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA IX,IX+d (ED 32) --- */
    case 0x32u: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, false),
                                             disp);
        cpu_set_index_multibyte(&emu->cpu, false, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA IY,IY+d (ED 33) --- */
    case 0x33u: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, true),
                                             disp);
        cpu_set_index_multibyte(&emu->cpu, true, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA IX,IY+d (ED 54) --- */
    case 0x54u: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, true),
                                             disp);
        cpu_set_index_multibyte(&emu->cpu, false, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: LEA IY,IX+d (ED 55) --- */
    case 0x55u: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t result = cpu_lea_index_disp(&emu->cpu,
                                             cpu_get_index_multibyte(&emu->cpu, false),
                                             disp);
        cpu_set_index_multibyte(&emu->cpu, true, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* NEG (canonical) */
    case 0x44u: {
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t result = (uint8_t)(0u - a);
        cpu_set_reg_a(&emu->cpu, result);
        cpu_flags_sub8(&emu->cpu, 0u, a, 0u, result);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x45u: /* RETN */
    case 0x75u: /* RETN alias */
    case 0x5Du: { /* RETN alias */
        uint32_t target = 0u;
        status = stack_pop_pc(emu, &target);
        if (status != EZ80_OK) return status;
        emu->cpu.iff1 = emu->cpu.iff2;
        notify_retn(emu);
        if (cpu_effective_adl_mode(&emu->cpu)) {
            emu->cpu.pc = mask24(target);
        } else {
            emu->cpu.pc = map_code_addr16_cpu(&emu->cpu, pc, (uint16_t)target);
        }
        cpu_commit_instruction_modes(&emu->cpu);
        return EZ80_OK;
    }
    case 0x4Du: { /* RETI */
        uint32_t target = 0u;
        status = stack_pop_pc(emu, &target);
        if (status != EZ80_OK) return status;
        emu->cpu.iff1 = emu->cpu.iff2;
        notify_reti(emu);
        if (cpu_effective_adl_mode(&emu->cpu)) {
            emu->cpu.pc = mask24(target);
        } else {
            emu->cpu.pc = map_code_addr16_cpu(&emu->cpu, pc, (uint16_t)target);
        }
        cpu_commit_instruction_modes(&emu->cpu);
        return EZ80_OK;
    }

    case 0x47u:
        cpu_set_reg_i_low8(&emu->cpu, cpu_get_reg_a(&emu->cpu));
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0x4Fu:
        emu->cpu.r_reg = cpu_get_reg_a(&emu->cpu);
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0x57u: {
        uint8_t value = emu->cpu.i_reg;
        cpu_set_reg_a(&emu->cpu, value);
        cpu_flags_ld_a_from_ir(&emu->cpu, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x5Fu: {
        uint8_t value = emu->cpu.r_reg;
        cpu_set_reg_a(&emu->cpu, value);
        cpu_flags_ld_a_from_ir(&emu->cpu, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x46u:
    case 0x4Eu:
        emu->cpu.interrupt_mode = 0u;
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0x56u:
        emu->cpu.interrupt_mode = 1u;
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0x5Eu:
        emu->cpu.interrupt_mode = 2u;
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    /* --- eZ80: TST A,n (ED 64) --- */
    case 0x64u: {
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t imm = instruction->bytes[2];
        uint8_t result = (uint8_t)(a & imm);
        cpu_flags_logic8(&emu->cpu, result, true);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: PEA IX+d (ED 65) --- */
    case 0x65u: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t ea = calc_index_disp_addr(&emu->cpu, emu->cpu.ix, disp);
        status = cpu_effective_adl_mode(&emu->cpu)
                 ? stack_push24(emu, ea)
                 : stack_push16(emu, (uint16_t)ea);
        if (status != EZ80_OK) return status;
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: PEA IY+d (ED 66) --- */
    case 0x66u: {
        int8_t disp = (int8_t)instruction->bytes[2];
        uint32_t ea = calc_index_disp_addr(&emu->cpu, emu->cpu.iy, disp);
        status = cpu_effective_adl_mode(&emu->cpu)
                 ? stack_push24(emu, ea)
                 : stack_push16(emu, (uint16_t)ea);
        if (status != EZ80_OK) return status;
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: TSTIO n (ED 74) --- */
    case 0x74u: {
        uint16_t port_addr = port_addr_from_bc(&emu->cpu);
        uint8_t port_val = port_read(emu, port_addr);
        uint8_t imm = instruction->bytes[2];
        uint8_t result = (uint8_t)(port_val & imm);
        cpu_flags_logic8(&emu->cpu, result, true);
        io_record_last_in(emu, port_addr, port_val);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: SLP (ED 76) --- */
    case 0x76u:
        pm_apply_mode_state(emu, EZ80_PM_MODE_STANDBY);
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    /* Undocumented ED-prefixed NOP aliases */
    case 0x77u:
    case 0x7Fu:
    case 0xFFu:
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    /* Compatibility no-op aliases for undefined ED slots in this scaffold. */
    case 0x05u:
    case 0x0Du:
    case 0x15u:
    case 0x1Au:
    case 0x1Bu:
    case 0x1Du:
    case 0x25u:
    case 0x2Au:
    case 0x2Bu:
    case 0x2Du:
    case 0x35u:
    case 0x36u:
    case 0x3Du:
    case 0xC0u:
    case 0xC1u:
    case 0xC4u:
    case 0xC5u:
    case 0xC6u:
    case 0xC8u:
    case 0xC9u:
    case 0xCCu:
    case 0xCDu:
    case 0xCEu:
    case 0xCFu:
    case 0xD0u:
    case 0xD1u:
    case 0xD2u:
    case 0xD3u:
    case 0xD4u:
    case 0xD5u:
    case 0xD6u:
    case 0xD8u:
    case 0xD9u:
    case 0xDAu:
    case 0xDBu:
    case 0xDCu:
    case 0xDDu:
    case 0xDEu:
    case 0xDFu:
    case 0xE0u:
    case 0xE1u:
    case 0xE2u:
    case 0xE3u:
    case 0xE4u:
    case 0xE5u:
    case 0xE6u:
    case 0xE7u:
    case 0xE8u:
    case 0xE9u:
    case 0xEAu:
    case 0xEBu:
    case 0xECu:
    case 0xEDu:
    case 0xEEu:
    case 0xEFu:
    case 0xF0u:
    case 0xF1u:
    case 0xF2u:
    case 0xF3u:
    case 0xF4u:
    case 0xF5u:
    case 0xF6u:
    case 0xF7u:
    case 0xF8u:
    case 0xF9u:
    case 0xFAu:
    case 0xFBu:
    case 0xFCu:
    case 0xFDu:
    case 0xFEu:
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0x40u:
    case 0x48u:
    case 0x50u:
    case 0x58u:
    case 0x60u:
    case 0x68u:
    case 0x78u: {
        uint8_t r = (uint8_t)((op >> 3) & 0x07u);
        uint16_t port_addr = port_addr_from_bc(&emu->cpu);
        uint8_t value = port_read(emu, port_addr);
        uint8_t flags = cpu_get_reg_f(&emu->cpu) & FLAG_C;
        cpu_set_reg8(&emu->cpu, r, value);
        if ((value & 0x80u) != 0u) flags |= FLAG_S;
        if (value == 0u) flags |= FLAG_Z;
        if (parity_even8(value)) flags |= FLAG_PV;
        cpu_set_reg_f(&emu->cpu, flags);
        io_record_last_in(emu, port_addr, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x70u: {
        uint16_t port_addr = port_addr_from_bc(&emu->cpu);
        uint8_t value = port_read(emu, port_addr);
        uint8_t flags = cpu_get_reg_f(&emu->cpu) & FLAG_C;
        if ((value & 0x80u) != 0u) flags |= FLAG_S;
        if (value == 0u) flags |= FLAG_Z;
        if (parity_even8(value)) flags |= FLAG_PV;
        cpu_set_reg_f(&emu->cpu, flags);
        io_record_last_in(emu, port_addr, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x41u:
    case 0x49u:
    case 0x51u:
    case 0x59u:
    case 0x61u:
    case 0x69u:
    case 0x79u: {
        uint8_t r = (uint8_t)((op >> 3) & 0x07u);
        uint16_t port_addr = port_addr_from_bc(&emu->cpu);
        uint8_t value = cpu_get_reg8(&emu->cpu, r);
        port_write(emu, port_addr, value);
        io_record_last_out(emu, port_addr, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x71u: {
        uint16_t port_addr = port_addr_from_bc(&emu->cpu);
        port_write(emu, port_addr, 0u);
        io_record_last_out(emu, port_addr, 0u);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x42u:
    case 0x52u:
    case 0x62u:
    case 0x72u: {
        uint8_t ss = (uint8_t)((op >> 4) & 0x03u);
        if (cpu_effective_pair_adl(&emu->cpu)) {
            uint32_t hl24 = cpu_get_pair_width(&emu->cpu, 2u);
            uint32_t val24 = cpu_get_pair_width(&emu->cpu, ss);
            uint8_t carry = (cpu_get_reg_f(&emu->cpu) & FLAG_C) ? 1u : 0u;
            uint64_t rhs_total = (uint64_t)val24 + (uint64_t)carry;
            uint32_t result24 = mask24((uint32_t)((uint64_t)hl24 - rhs_total));
            cpu_set_pair_width(&emu->cpu, 2u, result24);
            cpu_flags_sbc_hl_ss_24(&emu->cpu, hl24, val24, carry, result24);
        } else {
            uint16_t hl = cpu_get_pair16(&emu->cpu, 2u);
            uint16_t value = cpu_get_pair16(&emu->cpu, ss);
            uint8_t carry = (cpu_get_reg_f(&emu->cpu) & FLAG_C) ? 1u : 0u;
            uint32_t rhs_total = (uint32_t)value + (uint32_t)carry;
            uint16_t result = (uint16_t)((uint32_t)hl - rhs_total);
            cpu_set_pair16(&emu->cpu, 2u, result);
            cpu_flags_sbc_hl_ss_16(&emu->cpu, hl, value, carry, result);
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x43u:
    case 0x53u:
    case 0x63u:
    case 0x73u: {
        uint8_t ss = (uint8_t)((op >> 4) & 0x03u);
        if (cpu_effective_pair_adl(&emu->cpu)) {
            bool adl_encoding = cpu_effective_adl_encoding(&emu->cpu);
            uint32_t val24 = cpu_get_pair_width(&emu->cpu, ss);
            uint32_t addr0 = adl_encoding
                             ? mask24(read_u24(&instruction->bytes[2]))
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction->bytes[2]), 0u);
            uint32_t addr1 = adl_encoding
                             ? mask24(addr0 + 1u)
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction->bytes[2]), 1u);
            uint32_t addr2 = adl_encoding
                             ? mask24(addr0 + 2u)
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction->bytes[2]), 2u);
            status = ez80_memory_write8(emu, addr0, (uint8_t)(val24 & 0xFFu));
            if (status != EZ80_OK) return status;
            status = ez80_memory_write8(emu, addr1, (uint8_t)((val24 >> 8) & 0xFFu));
            if (status != EZ80_OK) return status;
            status = ez80_memory_write8(emu, addr2, (uint8_t)((val24 >> 16) & 0xFFu));
            if (status != EZ80_OK) return status;
        } else {
            uint16_t addr16 = read_u16(&instruction->bytes[2]);
            uint32_t addr0 = map_data_addr16_offset_cpu(&emu->cpu, addr16, 0u);
            uint32_t addr1 = map_data_addr16_offset_cpu(&emu->cpu, addr16, 1u);
            uint16_t value = cpu_get_pair16(&emu->cpu, ss);
            status = ez80_memory_write8(emu, addr0, (uint8_t)(value & 0xFFu));
            if (status != EZ80_OK) return status;
            status = ez80_memory_write8(emu, addr1, (uint8_t)((value >> 8) & 0xFFu));
            if (status != EZ80_OK) return status;
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x4Au:
    case 0x5Au:
    case 0x6Au:
    case 0x7Au: {
        uint8_t ss = (uint8_t)((op >> 4) & 0x03u);
        if (cpu_effective_pair_adl(&emu->cpu)) {
            uint32_t hl24 = cpu_get_pair_width(&emu->cpu, 2u);
            uint32_t val24 = cpu_get_pair_width(&emu->cpu, ss);
            uint8_t carry = (cpu_get_reg_f(&emu->cpu) & FLAG_C) ? 1u : 0u;
            uint64_t sum64 = (uint64_t)hl24 + (uint64_t)val24 + (uint64_t)carry;
            uint32_t result24 = mask24((uint32_t)sum64);
            cpu_set_pair_width(&emu->cpu, 2u, result24);
            cpu_flags_adc_hl_ss_24(&emu->cpu, hl24, val24, carry, result24, sum64);
        } else {
            uint16_t hl = cpu_get_pair16(&emu->cpu, 2u);
            uint16_t value = cpu_get_pair16(&emu->cpu, ss);
            uint8_t carry = (cpu_get_reg_f(&emu->cpu) & FLAG_C) ? 1u : 0u;
            uint32_t sum = (uint32_t)hl + (uint32_t)value + (uint32_t)carry;
            uint16_t result = (uint16_t)sum;
            cpu_set_pair16(&emu->cpu, 2u, result);
            cpu_flags_adc_hl_ss_16(&emu->cpu, hl, value, carry, result, sum);
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x4Bu:
    case 0x5Bu:
    case 0x6Bu:
    case 0x7Bu: {
        uint8_t ss = (uint8_t)((op >> 4) & 0x03u);
        if (cpu_effective_pair_adl(&emu->cpu)) {
            bool adl_encoding = cpu_effective_adl_encoding(&emu->cpu);
            uint8_t b0 = 0u;
            uint8_t b1 = 0u;
            uint8_t b2 = 0u;
            uint32_t addr0 = adl_encoding
                             ? mask24(read_u24(&instruction->bytes[2]))
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction->bytes[2]), 0u);
            uint32_t addr1 = adl_encoding
                             ? mask24(addr0 + 1u)
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction->bytes[2]), 1u);
            uint32_t addr2 = adl_encoding
                             ? mask24(addr0 + 2u)
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction->bytes[2]), 2u);
            status = ez80_memory_read8(emu, addr0, &b0);
            if (status != EZ80_OK) return status;
            status = ez80_memory_read8(emu, addr1, &b1);
            if (status != EZ80_OK) return status;
            status = ez80_memory_read8(emu, addr2, &b2);
            if (status != EZ80_OK) return status;
            cpu_set_pair_width(&emu->cpu, ss,
                               mask24((uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16)));
        } else {
            uint16_t addr16 = read_u16(&instruction->bytes[2]);
            uint32_t addr0 = map_data_addr16_offset_cpu(&emu->cpu, addr16, 0u);
            uint32_t addr1 = map_data_addr16_offset_cpu(&emu->cpu, addr16, 1u);
            uint8_t lo = 0u;
            uint8_t hi = 0u;
            status = ez80_memory_read8(emu, addr0, &lo);
            if (status != EZ80_OK) return status;
            status = ez80_memory_read8(emu, addr1, &hi);
            if (status != EZ80_OK) return status;
            cpu_set_pair16(&emu->cpu, ss, (uint16_t)lo | ((uint16_t)hi << 8));
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x4Cu:
    case 0x5Cu:
    case 0x6Cu:
    case 0x7Cu: {
        uint8_t ss = (uint8_t)((op >> 4) & 0x03u);
        uint16_t pair = cpu_get_pair16(&emu->cpu, ss);
        uint8_t hi_byte = (uint8_t)((pair >> 8) & 0xFFu);
        uint8_t lo_byte = (uint8_t)(pair & 0xFFu);
        uint16_t product = (uint16_t)hi_byte * (uint16_t)lo_byte;
        cpu_set_pair16(&emu->cpu, ss, product);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x6Du:
        emu->cpu.mbase_reg = cpu_get_reg_a(&emu->cpu);
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0x6Eu:
        cpu_set_reg_a(&emu->cpu, emu->cpu.mbase_reg);
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0xC7u:
        cpu_set_reg_i16(&emu->cpu, cpu_get_pair16(&emu->cpu, 2u));
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0xD7u: {
        uint16_t value = cpu_get_reg_i16(&emu->cpu);
        cpu_set_pair16(&emu->cpu, 2u, value);
        cpu_flags_ld_hl_from_i(&emu->cpu, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x67u: {
        uint32_t hl_addr = map_data_addr_cpu(&emu->cpu, emu->cpu.hl);
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t m = 0u;
        uint8_t new_a;
        uint8_t new_m;
        uint8_t flags;
        status = ez80_memory_read8(emu, hl_addr, &m);
        if (status != EZ80_OK) return status;
        new_a = (uint8_t)((a & 0xF0u) | (m & 0x0Fu));
        new_m = (uint8_t)(((a & 0x0Fu) << 4) | ((m >> 4) & 0x0Fu));
        cpu_set_reg_a(&emu->cpu, new_a);
        status = ez80_memory_write8(emu, hl_addr, new_m);
        if (status != EZ80_OK) return status;
        flags = cpu_get_reg_f(&emu->cpu) & FLAG_C;
        if ((new_a & 0x80u) != 0u) flags |= FLAG_S;
        if (new_a == 0u) flags |= FLAG_Z;
        if (parity_even8(new_a)) flags |= FLAG_PV;
        cpu_set_reg_f(&emu->cpu, flags);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x6Fu: {
        uint32_t hl_addr = map_data_addr_cpu(&emu->cpu, emu->cpu.hl);
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t m = 0u;
        uint8_t new_a;
        uint8_t new_m;
        uint8_t flags;
        status = ez80_memory_read8(emu, hl_addr, &m);
        if (status != EZ80_OK) return status;
        new_a = (uint8_t)((a & 0xF0u) | ((m >> 4) & 0x0Fu));
        new_m = (uint8_t)(((m & 0x0Fu) << 4) | (a & 0x0Fu));
        cpu_set_reg_a(&emu->cpu, new_a);
        status = ez80_memory_write8(emu, hl_addr, new_m);
        if (status != EZ80_OK) return status;
        flags = cpu_get_reg_f(&emu->cpu) & FLAG_C;
        if ((new_a & 0x80u) != 0u) flags |= FLAG_S;
        if (new_a == 0u) flags |= FLAG_Z;
        if (parity_even8(new_a)) flags |= FLAG_PV;
        cpu_set_reg_f(&emu->cpu, flags);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x7Du:
        emu->cpu.mixed_mode = true;
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0x7Eu:
        emu->cpu.mixed_mode = false;
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0xA0u:
    case 0xA8u:
    case 0xB0u:
    case 0xB8u: {
        bool adl_mode = cpu_effective_adl_mode(&emu->cpu);
        bool decrement = (op == 0xA8u || op == 0xB8u);
        bool repeat = (op == 0xB0u || op == 0xB8u);
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint32_t de = cpu_get_pair_width(&emu->cpu, 1u);
        uint32_t bc = cpu_get_block_counter(&emu->cpu);
        uint8_t value = 0u;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;
        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, de), value);
        if (status != EZ80_OK) return status;

        if (decrement) {
            if (adl_mode) {
                hl = mask24(hl - 1u);
                de = mask24(de - 1u);
            } else {
                hl = (uint16_t)(hl - 1u);
                de = (uint16_t)(de - 1u);
            }
        } else {
            if (adl_mode) {
                hl = mask24(hl + 1u);
                de = mask24(de + 1u);
            } else {
                hl = (uint16_t)(hl + 1u);
                de = (uint16_t)(de + 1u);
            }
        }
        if (adl_mode) {
            bc = mask24(bc - 1u);
        } else {
            bc = (uint16_t)(bc - 1u);
        }

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_pair_width(&emu->cpu, 1u, de);
        cpu_set_block_counter(&emu->cpu, bc);
        cpu_flags_block_transfer(&emu->cpu, bc);

        emu->cpu.pc = (repeat && bc != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    case 0xA1u:
    case 0xA9u:
    case 0xB1u:
    case 0xB9u: {
        bool adl_mode = cpu_effective_adl_mode(&emu->cpu);
        bool decrement = (op == 0xA9u || op == 0xB9u);
        bool repeat = (op == 0xB1u || op == 0xB9u);
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint32_t bc = cpu_get_block_counter(&emu->cpu);
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t value = 0u;
        uint8_t result;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;

        result = (uint8_t)(a - value);
        if (decrement) {
            if (adl_mode) {
                hl = mask24(hl - 1u);
            } else {
                hl = (uint16_t)(hl - 1u);
            }
        } else {
            if (adl_mode) {
                hl = mask24(hl + 1u);
            } else {
                hl = (uint16_t)(hl + 1u);
            }
        }
        if (adl_mode) {
            bc = mask24(bc - 1u);
        } else {
            bc = (uint16_t)(bc - 1u);
        }

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_block_counter(&emu->cpu, bc);
        cpu_flags_block_compare(&emu->cpu, a, value, result, bc);

        emu->cpu.pc = (repeat && bc != 0u && result != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    case 0xA2u:
    case 0xAAu:
    case 0xB2u:
    case 0xBAu: {
        bool decrement = (op == 0xAAu || op == 0xBAu);
        bool repeat = (op == 0xB2u || op == 0xBAu);
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = port_read(emu, c);

        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, hl), value);
        if (status != EZ80_OK) return status;

        if (decrement) {
            hl = cpu_decrement_width(&emu->cpu, hl);
        } else {
            hl = cpu_increment_width(&emu->cpu, hl);
        }
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);

        io_record_last_in(emu, (uint16_t)c, value);
        if (c == 0x02u) {
            mmio_set(emu, EZ80_MMIO_REG_LCD_STATUS, value);
        }

        emu->cpu.pc = (repeat && b != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    case 0xA3u:
    case 0xABu:
    case 0xB3u:
    case 0xBBu: {
        bool decrement = (op == 0xABu || op == 0xBBu);
        bool repeat = (op == 0xB3u || op == 0xBBu);
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = 0u;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;

        port_write(emu, c, value);
        io_record_last_out(emu, (uint16_t)c, value);
        if (c == 0x01u) {
            mmio_set(emu, EZ80_MMIO_REG_KEY_STATUS, value);
        }

        if (decrement) {
            hl = cpu_decrement_width(&emu->cpu, hl);
        } else {
            hl = cpu_increment_width(&emu->cpu, hl);
        }
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);

        emu->cpu.pc = (repeat && b != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: INIM (ED 82) — IN (C) → (HL); HL++; C++; B-- --- */
    case 0x80u:
    case 0x82u: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = port_read(emu, c);

        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, hl), value);
        if (status != EZ80_OK) return status;

        hl = cpu_increment_width(&emu->cpu, hl);
        c = (uint8_t)(c + 1u);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 1u, c);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        io_record_last_in(emu, (uint16_t)(uint8_t)(c - 1u), value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: OTIM (ED 83) — (HL) → OUT (C); HL++; C++; B-- --- */
    case 0x81u:
    case 0x83u:
    case 0xA4u: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = 0u;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;

        port_write(emu, c, value);
        io_record_last_out(emu, (uint16_t)c, value);

        hl = cpu_increment_width(&emu->cpu, hl);
        c = (uint8_t)(c + 1u);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 1u, c);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: INI2 (ED 84) — IN (C) → (HL); HL++; B-- (like INI) --- */
    case 0x84u:
    case 0x86u: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = port_read(emu, c);

        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, hl), value);
        if (status != EZ80_OK) return status;

        hl = cpu_increment_width(&emu->cpu, hl);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        io_record_last_in(emu, (uint16_t)c, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: OTI2 (ED 85/ED A5 alias) — (HL) → OUT (C); HL++; B-- (like OUTI) --- */
    case 0x85u:
    case 0x87u:
    case 0xA5u:
    case 0xA6u:
    case 0xA7u: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = 0u;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;

        port_write(emu, c, value);
        io_record_last_out(emu, (uint16_t)c, value);

        hl = cpu_increment_width(&emu->cpu, hl);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: INDM (ED 8A) — IN (C) → (HL); HL--; C--; B-- --- */
    case 0x88u:
    case 0x8Au: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = port_read(emu, c);

        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, hl), value);
        if (status != EZ80_OK) return status;

        hl = cpu_decrement_width(&emu->cpu, hl);
        c = (uint8_t)(c - 1u);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 1u, c);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        io_record_last_in(emu, (uint16_t)(uint8_t)(c + 1u), value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: OTDM (ED 8B) — (HL) → OUT (C); HL--; C--; B-- --- */
    case 0x89u:
    case 0x8Bu:
    case 0xACu: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = 0u;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;

        port_write(emu, c, value);
        io_record_last_out(emu, (uint16_t)c, value);

        hl = cpu_decrement_width(&emu->cpu, hl);
        c = (uint8_t)(c - 1u);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 1u, c);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: IND2 (ED 8C) — IN (C) → (HL); HL--; B-- --- */
    case 0x8Cu:
    case 0x8Eu: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = port_read(emu, c);

        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, hl), value);
        if (status != EZ80_OK) return status;

        hl = cpu_decrement_width(&emu->cpu, hl);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        io_record_last_in(emu, (uint16_t)c, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: OTD2 (ED 8D/ED AD alias) — (HL) → OUT (C); HL--; B-- --- */
    case 0x8Du:
    case 0x8Fu:
    case 0xADu:
    case 0xAEu:
    case 0xAFu: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = 0u;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;

        port_write(emu, c, value);
        io_record_last_out(emu, (uint16_t)c, value);

        hl = cpu_decrement_width(&emu->cpu, hl);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: INIMR (ED 92) — repeat INIM until B==0 --- */
    case 0x90u:
    case 0x92u: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = port_read(emu, c);

        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, hl), value);
        if (status != EZ80_OK) return status;

        hl = cpu_increment_width(&emu->cpu, hl);
        c = (uint8_t)(c + 1u);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 1u, c);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        io_record_last_in(emu, (uint16_t)(uint8_t)(c - 1u), value);
        emu->cpu.pc = (b != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: OTIMR (ED 93) — repeat OTIM until B==0 --- */
    case 0x91u:
    case 0x93u:
    case 0xB4u: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = 0u;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;

        port_write(emu, c, value);
        io_record_last_out(emu, (uint16_t)c, value);

        hl = cpu_increment_width(&emu->cpu, hl);
        c = (uint8_t)(c + 1u);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 1u, c);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        emu->cpu.pc = (b != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: INI2R (ED 94) — repeat INI2 until B==0 --- */
    case 0x94u:
    case 0x96u: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = port_read(emu, c);

        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, hl), value);
        if (status != EZ80_OK) return status;

        hl = cpu_increment_width(&emu->cpu, hl);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        io_record_last_in(emu, (uint16_t)c, value);
        emu->cpu.pc = (b != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: OTI2R (ED 95/ED B5 alias) — repeat OTI2 until B==0 --- */
    case 0x95u:
    case 0x97u:
    case 0xB6u:
    case 0xB7u:
    case 0xB5u: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = 0u;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;

        port_write(emu, c, value);
        io_record_last_out(emu, (uint16_t)c, value);

        hl = cpu_increment_width(&emu->cpu, hl);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        emu->cpu.pc = (b != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: INDMR (ED 9A) — repeat INDM until B==0 --- */
    case 0x98u:
    case 0x9Au: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = port_read(emu, c);

        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, hl), value);
        if (status != EZ80_OK) return status;

        hl = cpu_decrement_width(&emu->cpu, hl);
        c = (uint8_t)(c - 1u);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 1u, c);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        io_record_last_in(emu, (uint16_t)(uint8_t)(c + 1u), value);
        emu->cpu.pc = (b != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: OTDMR (ED 9B) — repeat OTDM until B==0 --- */
    case 0x99u:
    case 0x9Bu:
    case 0xBCu: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = 0u;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;

        port_write(emu, c, value);
        io_record_last_out(emu, (uint16_t)c, value);

        hl = cpu_decrement_width(&emu->cpu, hl);
        c = (uint8_t)(c - 1u);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 1u, c);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        emu->cpu.pc = (b != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: IND2R (ED 9C) — repeat IND2 until B==0 --- */
    case 0x9Cu:
    case 0x9Eu: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = port_read(emu, c);

        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, hl), value);
        if (status != EZ80_OK) return status;

        hl = cpu_decrement_width(&emu->cpu, hl);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        io_record_last_in(emu, (uint16_t)c, value);
        emu->cpu.pc = (b != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: OTD2R (ED 9D/ED BD alias) — repeat OTD2 until B==0 --- */
    case 0x9Du:
    case 0x9Fu:
    case 0xBEu:
    case 0xBFu:
    case 0xBDu: {
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t b = cpu_get_reg8(&emu->cpu, 0u);
        uint8_t c = cpu_get_reg8(&emu->cpu, 1u);
        uint8_t value = 0u;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;

        port_write(emu, c, value);
        io_record_last_out(emu, (uint16_t)c, value);

        hl = cpu_decrement_width(&emu->cpu, hl);
        b = (uint8_t)(b - 1u);

        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_set_reg8(&emu->cpu, 0u, b);
        cpu_flags_block_io_b(&emu->cpu, value, b);
        emu->cpu.pc = (b != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: INIRX (ED C2) — repeat IN (DE) to (HL), BC--, HL++ until BC==0 --- */
    case 0xC2u: {
        uint32_t bc = cpu_get_block_counter(&emu->cpu);
        uint32_t de = cpu_get_pair_width(&emu->cpu, 1u);
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t port = (uint8_t)(de & 0xFFu);
        uint8_t value = port_read(emu, port);

        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, hl), value);
        if (status != EZ80_OK) return status;

        bc = cpu_decrement_block_counter(&emu->cpu, bc);
        hl = cpu_increment_width(&emu->cpu, hl);

        cpu_set_block_counter(&emu->cpu, bc);
        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_flags_block_io_bc(&emu->cpu, value, bc);
        io_record_last_in(emu, (uint16_t)port, value);
        if (port == 0x02u) {
            mmio_set(emu, EZ80_MMIO_REG_LCD_STATUS, value);
        }

        emu->cpu.pc = (bc != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: OTIRX (ED C3) — repeat (HL) to OUT (DE), BC--, HL++ until BC==0 --- */
    case 0xC3u: {
        uint32_t bc = cpu_get_block_counter(&emu->cpu);
        uint32_t de = cpu_get_pair_width(&emu->cpu, 1u);
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t port = (uint8_t)(de & 0xFFu);
        uint8_t value = 0u;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;

        port_write(emu, port, value);
        io_record_last_out(emu, (uint16_t)port, value);
        if (port == 0x01u) {
            mmio_set(emu, EZ80_MMIO_REG_KEY_STATUS, value);
        }

        bc = cpu_decrement_block_counter(&emu->cpu, bc);
        hl = cpu_increment_width(&emu->cpu, hl);

        cpu_set_block_counter(&emu->cpu, bc);
        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_flags_block_io_bc(&emu->cpu, value, bc);
        emu->cpu.pc = (bc != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: INDRX (ED CA) — repeat IN (DE) to (HL), BC--, HL-- until BC==0 --- */
    case 0xCAu: {
        uint32_t bc = cpu_get_block_counter(&emu->cpu);
        uint32_t de = cpu_get_pair_width(&emu->cpu, 1u);
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t port = (uint8_t)(de & 0xFFu);
        uint8_t value = port_read(emu, port);

        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, hl), value);
        if (status != EZ80_OK) return status;

        bc = cpu_decrement_block_counter(&emu->cpu, bc);
        hl = cpu_decrement_width(&emu->cpu, hl);

        cpu_set_block_counter(&emu->cpu, bc);
        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_flags_block_io_bc(&emu->cpu, value, bc);
        io_record_last_in(emu, (uint16_t)port, value);
        if (port == 0x02u) {
            mmio_set(emu, EZ80_MMIO_REG_LCD_STATUS, value);
        }

        emu->cpu.pc = (bc != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    /* --- eZ80: OTDRX (ED CB) — repeat (HL) to OUT (DE), BC--, HL-- until BC==0 --- */
    case 0xCBu: {
        uint32_t bc = cpu_get_block_counter(&emu->cpu);
        uint32_t de = cpu_get_pair_width(&emu->cpu, 1u);
        uint32_t hl = cpu_get_pair_width(&emu->cpu, 2u);
        uint8_t port = (uint8_t)(de & 0xFFu);
        uint8_t value = 0u;

        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, hl), &value);
        if (status != EZ80_OK) return status;

        port_write(emu, port, value);
        io_record_last_out(emu, (uint16_t)port, value);
        if (port == 0x01u) {
            mmio_set(emu, EZ80_MMIO_REG_KEY_STATUS, value);
        }

        bc = cpu_decrement_block_counter(&emu->cpu, bc);
        hl = cpu_decrement_width(&emu->cpu, hl);

        cpu_set_block_counter(&emu->cpu, bc);
        cpu_set_pair_width(&emu->cpu, 2u, hl);
        cpu_flags_block_io_bc(&emu->cpu, value, bc);
        emu->cpu.pc = (bc != 0u) ? pc : next_pc;
        return EZ80_OK;
    }

    default:
        return EZ80_ERR_NOT_IMPLEMENTED;
    }
}

static ez80_status_t execute_ddfd_prefix(ez80_emulator_t *emu,
                                         const ez80_instruction_t *instruction,
                                         uint32_t next_pc,
                                         bool use_iy)
{
    ez80_status_t status;
    uint8_t op = instruction->bytes[1];
    uint16_t index16;
    uint32_t index_addr;

    switch (op) {
    case 0x21u:
        {
            uint32_t imm = read_imm_word(&instruction->bytes[2],
                                         cpu_effective_adl_encoding(&emu->cpu));
            if (cpu_effective_pair_adl(&emu->cpu)) {
                *cpu_index_ptr(&emu->cpu, use_iy) = mask24(imm);
            } else {
                cpu_set_index16(&emu->cpu, use_iy, (uint16_t)imm);
            }
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0x22u: {
        if (cpu_effective_pair_adl(&emu->cpu)) {
            bool adl_encoding = cpu_effective_adl_encoding(&emu->cpu);
            uint32_t val = mask24(*cpu_index_ptr(&emu->cpu, use_iy));
            uint32_t addr0 = adl_encoding
                             ? mask24(read_u24(&instruction->bytes[2]))
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction->bytes[2]), 0u);
            uint32_t addr1 = adl_encoding
                             ? mask24(addr0 + 1u)
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction->bytes[2]), 1u);
            uint32_t addr2 = adl_encoding
                             ? mask24(addr0 + 2u)
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction->bytes[2]), 2u);
            status = ez80_memory_write8(emu, addr0, (uint8_t)(val & 0xFFu));
            if (status != EZ80_OK) return status;
            status = ez80_memory_write8(emu, addr1, (uint8_t)((val >> 8) & 0xFFu));
            if (status != EZ80_OK) return status;
            status = ez80_memory_write8(emu, addr2, (uint8_t)((val >> 16) & 0xFFu));
            if (status != EZ80_OK) return status;
        } else {
            uint16_t addr16 = read_u16(&instruction->bytes[2]);
            uint32_t addr0 = map_data_addr16_offset_cpu(&emu->cpu, addr16, 0u);
            uint32_t addr1 = map_data_addr16_offset_cpu(&emu->cpu, addr16, 1u);
            uint16_t value = cpu_get_index16(&emu->cpu, use_iy);
            status = ez80_memory_write8(emu, addr0, (uint8_t)(value & 0xFFu));
            if (status != EZ80_OK) return status;
            status = ez80_memory_write8(emu, addr1, (uint8_t)((value >> 8) & 0xFFu));
            if (status != EZ80_OK) return status;
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x2Au: {
        if (cpu_effective_pair_adl(&emu->cpu)) {
            bool adl_encoding = cpu_effective_adl_encoding(&emu->cpu);
            uint8_t b0 = 0u;
            uint8_t b1 = 0u;
            uint8_t b2 = 0u;
            uint32_t addr0 = adl_encoding
                             ? mask24(read_u24(&instruction->bytes[2]))
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction->bytes[2]), 0u);
            uint32_t addr1 = adl_encoding
                             ? mask24(addr0 + 1u)
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction->bytes[2]), 1u);
            uint32_t addr2 = adl_encoding
                             ? mask24(addr0 + 2u)
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction->bytes[2]), 2u);
            status = ez80_memory_read8(emu, addr0, &b0);
            if (status != EZ80_OK) return status;
            status = ez80_memory_read8(emu, addr1, &b1);
            if (status != EZ80_OK) return status;
            status = ez80_memory_read8(emu, addr2, &b2);
            if (status != EZ80_OK) return status;
            *cpu_index_ptr(&emu->cpu, use_iy) =
                mask24((uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16));
        } else {
            uint8_t lo;
            uint8_t hi;
            uint16_t addr16 = read_u16(&instruction->bytes[2]);
            uint32_t addr0 = map_data_addr16_offset_cpu(&emu->cpu, addr16, 0u);
            uint32_t addr1 = map_data_addr16_offset_cpu(&emu->cpu, addr16, 1u);
            status = ez80_memory_read8(emu, addr0, &lo);
            if (status != EZ80_OK) return status;
            status = ez80_memory_read8(emu, addr1, &hi);
            if (status != EZ80_OK) return status;
            cpu_set_index16(&emu->cpu, use_iy, (uint16_t)lo | ((uint16_t)hi << 8));
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x23u:
        if (cpu_effective_pair_adl(&emu->cpu)) {
            *cpu_index_ptr(&emu->cpu, use_iy) = mask24(*cpu_index_ptr(&emu->cpu, use_iy) + 1u);
        } else {
            index16 = (uint16_t)(cpu_get_index16(&emu->cpu, use_iy) + 1u);
            cpu_set_index16(&emu->cpu, use_iy, index16);
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0x2Bu:
        if (cpu_effective_pair_adl(&emu->cpu)) {
            *cpu_index_ptr(&emu->cpu, use_iy) = mask24(*cpu_index_ptr(&emu->cpu, use_iy) - 1u);
        } else {
            index16 = (uint16_t)(cpu_get_index16(&emu->cpu, use_iy) - 1u);
            cpu_set_index16(&emu->cpu, use_iy, index16);
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0x09u:
    case 0x19u:
    case 0x29u:
    case 0x39u: {
        uint8_t ss = (uint8_t)((op >> 4) & 0x03u);
        if (cpu_effective_pair_adl(&emu->cpu)) {
            uint32_t lhs = mask24(*cpu_index_ptr(&emu->cpu, use_iy));
            uint32_t rhs = (ss == 2u) ? lhs : cpu_get_pair_width(&emu->cpu, ss);
            uint32_t sum = lhs + rhs;
            *cpu_index_ptr(&emu->cpu, use_iy) = mask24(sum);
            cpu_flags_add_hl_ss(&emu->cpu, (uint16_t)lhs, (uint16_t)rhs, sum);
        } else {
            uint16_t lhs = cpu_get_index16(&emu->cpu, use_iy);
            uint16_t rhs = (ss == 2u) ? cpu_get_index16(&emu->cpu, use_iy)
                                      : cpu_get_pair16(&emu->cpu, ss);
            uint32_t sum = (uint32_t)lhs + (uint32_t)rhs;
            cpu_set_index16(&emu->cpu, use_iy, (uint16_t)sum);
            cpu_flags_add_hl_ss(&emu->cpu, lhs, rhs, sum);
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0xE1u: {
        if (cpu_effective_adl_mode(&emu->cpu)) {
            uint32_t value24 = 0u;
            status = stack_pop24(emu, &value24);
            if (status != EZ80_OK) return status;
            *cpu_index_ptr(&emu->cpu, use_iy) = value24;
        } else {
            uint16_t value16 = 0u;
            status = stack_pop16(emu, &value16);
            if (status != EZ80_OK) return status;
            cpu_set_index16(&emu->cpu, use_iy, value16);
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0xE5u:
        status = cpu_effective_adl_mode(&emu->cpu)
                 ? stack_push24(emu, *cpu_index_ptr(&emu->cpu, use_iy))
                 : stack_push16(emu, cpu_get_index16(&emu->cpu, use_iy));
        if (status != EZ80_OK) return status;
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0xE3u:
        status = cpu_exchange_pair16_with_stack(emu, cpu_index_ptr(&emu->cpu, use_iy));
        if (status != EZ80_OK) return status;
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0xE9:
        if (cpu_effective_adl_mode(&emu->cpu)) {
            emu->cpu.pc = mask24(*cpu_index_ptr(&emu->cpu, use_iy));
        } else {
            emu->cpu.pc = z80_mbase_addr(emu->cpu.mbase_reg, cpu_get_index16(&emu->cpu, use_iy));
        }
        cpu_commit_instruction_modes(&emu->cpu);
        return EZ80_OK;

    case 0xF9:
        emu->cpu.sp = map_data_addr_cpu(&emu->cpu, *cpu_index_ptr(&emu->cpu, use_iy));
        emu->cpu.pc = next_pc;
        return EZ80_OK;

    case 0xCB:
        return execute_ddfd_cb_prefix(emu, instruction, next_pc, use_iy);

    case 0x07u:
    case 0x17u:
    case 0x27u: {
        uint8_t ss = (uint8_t)(op >> 4);
        bool multibyte_adl = cpu_effective_pair_adl(&emu->cpu);
        uint32_t raw_addr = mask24(*cpu_index_ptr(&emu->cpu, use_iy) & 0x00FF0000u)
                          | (uint32_t)(uint16_t)((int32_t)(uint16_t)(*cpu_index_ptr(&emu->cpu, use_iy) & 0xFFFFu)
                                               + (int32_t)(int8_t)instruction->bytes[2]);
        uint32_t value = 0u;
        uint8_t byte0 = 0u;
        uint8_t byte1 = 0u;
        uint8_t byte2 = 0u;
        status = ez80_memory_read8(emu,
                                   map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 0u, multibyte_adl),
                                   &byte0);
        if (status != EZ80_OK) return status;
        status = ez80_memory_read8(emu,
                                   map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 1u, multibyte_adl),
                                   &byte1);
        if (status != EZ80_OK) return status;
        value = (uint32_t)byte0 | ((uint32_t)byte1 << 8);
        if (multibyte_adl) {
            status = ez80_memory_read8(emu,
                                       map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 2u, true),
                                       &byte2);
            if (status != EZ80_OK) return status;
            value |= ((uint32_t)byte2 << 16);
        }
        cpu_set_pair_multibyte(&emu->cpu, ss, value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x0Fu:
    case 0x1Fu:
    case 0x2Fu: {
        uint8_t ss = (uint8_t)(op >> 4);
        bool multibyte_adl = cpu_effective_pair_adl(&emu->cpu);
        uint32_t raw_addr = mask24(*cpu_index_ptr(&emu->cpu, use_iy) & 0x00FF0000u)
                          | (uint32_t)(uint16_t)((int32_t)(uint16_t)(*cpu_index_ptr(&emu->cpu, use_iy) & 0xFFFFu)
                                               + (int32_t)(int8_t)instruction->bytes[2]);
        uint32_t value = cpu_get_pair_multibyte(&emu->cpu, ss);
        status = ez80_memory_write8(emu,
                                    map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 0u, multibyte_adl),
                                    (uint8_t)(value & 0xFFu));
        if (status != EZ80_OK) return status;
        status = ez80_memory_write8(emu,
                                    map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 1u, multibyte_adl),
                                    (uint8_t)((value >> 8) & 0xFFu));
        if (status != EZ80_OK) return status;
        if (multibyte_adl) {
            status = ez80_memory_write8(emu,
                                        map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 2u, true),
                                        (uint8_t)((value >> 16) & 0xFFu));
            if (status != EZ80_OK) return status;
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x31u:
    case 0x37u: {
        bool target_other_index = (op == 0x37u);
        bool multibyte_adl = cpu_effective_pair_adl(&emu->cpu);
        uint32_t raw_addr = mask24(*cpu_index_ptr(&emu->cpu, use_iy) & 0x00FF0000u)
                          | (uint32_t)(uint16_t)((int32_t)(uint16_t)(*cpu_index_ptr(&emu->cpu, use_iy) & 0xFFFFu)
                                               + (int32_t)(int8_t)instruction->bytes[2]);
        uint32_t value = 0u;
        uint8_t byte0 = 0u;
        uint8_t byte1 = 0u;
        uint8_t byte2 = 0u;
        status = ez80_memory_read8(emu,
                                   map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 0u, multibyte_adl),
                                   &byte0);
        if (status != EZ80_OK) return status;
        status = ez80_memory_read8(emu,
                                   map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 1u, multibyte_adl),
                                   &byte1);
        if (status != EZ80_OK) return status;
        value = (uint32_t)byte0 | ((uint32_t)byte1 << 8);
        if (multibyte_adl) {
            status = ez80_memory_read8(emu,
                                       map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 2u, true),
                                       &byte2);
            if (status != EZ80_OK) return status;
            value |= ((uint32_t)byte2 << 16);
        }
        cpu_set_index_multibyte(&emu->cpu,
                                target_other_index ? !use_iy : use_iy,
                                value);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    case 0x3Eu:
    case 0x3Fu: {
        bool source_other_index = (op == 0x3Eu);
        bool multibyte_adl = cpu_effective_pair_adl(&emu->cpu);
        uint32_t raw_addr = mask24(*cpu_index_ptr(&emu->cpu, use_iy) & 0x00FF0000u)
                          | (uint32_t)(uint16_t)((int32_t)(uint16_t)(*cpu_index_ptr(&emu->cpu, use_iy) & 0xFFFFu)
                                               + (int32_t)(int8_t)instruction->bytes[2]);
        uint32_t value = cpu_get_index_multibyte(&emu->cpu,
                                                 source_other_index ? !use_iy : use_iy);
        status = ez80_memory_write8(emu,
                                    map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 0u, multibyte_adl),
                                    (uint8_t)(value & 0xFFu));
        if (status != EZ80_OK) return status;
        status = ez80_memory_write8(emu,
                                    map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 1u, multibyte_adl),
                                    (uint8_t)((value >> 8) & 0xFFu));
        if (status != EZ80_OK) return status;
        if (multibyte_adl) {
            status = ez80_memory_write8(emu,
                                        map_multibyte_ptr_offset_cpu(&emu->cpu, raw_addr, 2u, true),
                                        (uint8_t)((value >> 16) & 0xFFu));
            if (status != EZ80_OK) return status;
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    default:
        break;
    }

    /* --- IXH/IXL (IYH/IYL) undocumented half-register ops --- */

    /* INC IXH (DD 24) / INC IYH (FD 24) */
    if (op == 0x24u) {
        uint8_t before = cpu_get_index_hi(&emu->cpu, use_iy);
        uint8_t after = (uint8_t)(before + 1u);
        cpu_set_index_hi(&emu->cpu, use_iy, after);
        cpu_flags_inc8(&emu->cpu, before, after);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* DEC IXH (DD 25) / DEC IYH (FD 25) */
    if (op == 0x25u) {
        uint8_t before = cpu_get_index_hi(&emu->cpu, use_iy);
        uint8_t after = (uint8_t)(before - 1u);
        cpu_set_index_hi(&emu->cpu, use_iy, after);
        cpu_flags_dec8(&emu->cpu, before, after);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* LD IXH,n (DD 26 nn) / LD IYH,n (FD 26 nn) */
    if (op == 0x26u) {
        cpu_set_index_hi(&emu->cpu, use_iy, instruction->bytes[2]);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* INC IXL (DD 2C) / INC IYL (FD 2C) */
    if (op == 0x2Cu) {
        uint8_t before = cpu_get_index_lo(&emu->cpu, use_iy);
        uint8_t after = (uint8_t)(before + 1u);
        cpu_set_index_lo(&emu->cpu, use_iy, after);
        cpu_flags_inc8(&emu->cpu, before, after);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* DEC IXL (DD 2D) / DEC IYL (FD 2D) */
    if (op == 0x2Du) {
        uint8_t before = cpu_get_index_lo(&emu->cpu, use_iy);
        uint8_t after = (uint8_t)(before - 1u);
        cpu_set_index_lo(&emu->cpu, use_iy, after);
        cpu_flags_dec8(&emu->cpu, before, after);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* LD IXL,n (DD 2E nn) / LD IYL,n (FD 2E nn) */
    if (op == 0x2Eu) {
        cpu_set_index_lo(&emu->cpu, use_iy, instruction->bytes[2]);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* LD r,IXH / LD r,IXL / LD IXH,r / LD IXL,r / LD IXH,IXL / LD IXL,IXH */
    if (op >= 0x40u && op <= 0x6Fu) {
        uint8_t dst = (uint8_t)((op >> 3) & 0x07u);
        uint8_t src = (uint8_t)(op & 0x07u);
        bool dst_is_ixh = (dst == 4u);
        bool dst_is_ixl = (dst == 5u);
        bool src_is_ixh = (src == 4u);
        bool src_is_ixl = (src == 5u);

        if (src == 6u || dst == 6u) {
            goto indexed_disp_block;
        }

        if (!dst_is_ixh && !dst_is_ixl && !src_is_ixh && !src_is_ixl) {
            goto indexed_disp_block;
        }

        {
            uint8_t value;
            if (src_is_ixh) {
                value = cpu_get_index_hi(&emu->cpu, use_iy);
            } else if (src_is_ixl) {
                value = cpu_get_index_lo(&emu->cpu, use_iy);
            } else {
                value = cpu_get_reg8(&emu->cpu, src);
            }
            if (dst_is_ixh) {
                cpu_set_index_hi(&emu->cpu, use_iy, value);
            } else if (dst_is_ixl) {
                cpu_set_index_lo(&emu->cpu, use_iy, value);
            } else {
                cpu_set_reg8(&emu->cpu, dst, value);
            }
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* LD r,IXH / LD r,IXL for 0x7C-0x7D range */
    if (op == 0x7Cu) {
        cpu_set_reg_a(&emu->cpu, cpu_get_index_hi(&emu->cpu, use_iy));
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }
    if (op == 0x7Du) {
        cpu_set_reg_a(&emu->cpu, cpu_get_index_lo(&emu->cpu, use_iy));
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    /* ALU A,IXH (DD 84,8C,94,9C,A4,AC,B4,BC) / ALU A,IXL (DD 85,8D,95,9D,A5,AD,B5,BD) */
    if (op >= 0x80u && op <= 0xBFu) {
        uint8_t src = (uint8_t)(op & 0x07u);
        uint8_t group = (uint8_t)((op >> 3) & 0x07u);
        if (src == 4u) {
            cpu_execute_alu_a(&emu->cpu, group, cpu_get_index_hi(&emu->cpu, use_iy));
            emu->cpu.pc = next_pc;
            return EZ80_OK;
        }
        if (src == 5u) {
            cpu_execute_alu_a(&emu->cpu, group, cpu_get_index_lo(&emu->cpu, use_iy));
            emu->cpu.pc = next_pc;
            return EZ80_OK;
        }
    }

indexed_disp_block:
    if (op == 0x34u || op == 0x35u || op == 0x36u ||
        op == 0x46u || op == 0x4Eu || op == 0x56u || op == 0x5Eu ||
        op == 0x66u || op == 0x6Eu || op == 0x7Eu ||
        op == 0x70u || op == 0x71u || op == 0x72u || op == 0x73u ||
        op == 0x74u || op == 0x75u || op == 0x77u ||
        op == 0x86u || op == 0x8Eu || op == 0x96u || op == 0x9Eu ||
        op == 0xA6u || op == 0xAEu || op == 0xB6u || op == 0xBEu) {
        int8_t disp = (int8_t)instruction->bytes[2];
        index_addr = calc_index_disp_addr(&emu->cpu, *cpu_index_ptr(&emu->cpu, use_iy), disp);

        if (op == 0x34u || op == 0x35u) {
            uint8_t value;
            uint8_t result;
            status = ez80_memory_read8(emu, index_addr, &value);
            if (status != EZ80_OK) return status;
            if (op == 0x34u) {
                result = (uint8_t)(value + 1u);
                cpu_flags_inc8(&emu->cpu, value, result);
            } else {
                result = (uint8_t)(value - 1u);
                cpu_flags_dec8(&emu->cpu, value, result);
            }
            status = ez80_memory_write8(emu, index_addr, result);
            if (status != EZ80_OK) return status;
            emu->cpu.pc = next_pc;
            return EZ80_OK;
        }

        if (op == 0x36u) {
            status = ez80_memory_write8(emu, index_addr, instruction->bytes[3]);
            if (status != EZ80_OK) return status;
            emu->cpu.pc = next_pc;
            return EZ80_OK;
        }

        if (op >= 0x70u && op <= 0x77u && op != 0x76u) {
            uint8_t src = (uint8_t)(op & 0x07u);
            status = ez80_memory_write8(emu, index_addr, cpu_get_reg8(&emu->cpu, src));
            if (status != EZ80_OK) return status;
            emu->cpu.pc = next_pc;
            return EZ80_OK;
        }

        if (op == 0x46u || op == 0x4Eu || op == 0x56u || op == 0x5Eu ||
            op == 0x66u || op == 0x6Eu || op == 0x7Eu) {
            uint8_t dst = (uint8_t)((op >> 3) & 0x07u);
            uint8_t value = 0u;
            status = ez80_memory_read8(emu, index_addr, &value);
            if (status != EZ80_OK) return status;
            cpu_set_reg8(&emu->cpu, dst, value);
            emu->cpu.pc = next_pc;
            return EZ80_OK;
        }

        if (op == 0x86u || op == 0x8Eu || op == 0x96u || op == 0x9Eu ||
            op == 0xA6u || op == 0xAEu || op == 0xB6u || op == 0xBEu) {
            uint8_t value = 0u;
            uint8_t group = (uint8_t)((op >> 3) & 0x07u);
            status = ez80_memory_read8(emu, index_addr, &value);
            if (status != EZ80_OK) return status;
            cpu_execute_alu_a(&emu->cpu, group, value);
            emu->cpu.pc = next_pc;
            return EZ80_OK;
        }
    }

    {
        uint32_t saved_pc = mask24(emu->cpu.pc);
        emu->cpu.pc = mask24(saved_pc + 1u);
        status = ez80_emulator_step(emu, NULL);
        if (status != EZ80_OK) {
            emu->cpu.pc = saved_pc;
            return status;
        }
        return EZ80_OK;
    }
}

static uint32_t os_allocate_ram_addr(ez80_emulator_t *emu, uint32_t size_bytes)
{
    uint32_t addr;
    if (!emu) return 0u;
    if (size_bytes == 0u) size_bytes = 1u;
    if (emu->os.ram_pool_next + size_bytes > EZ80_OS_RAM_POOL_BYTES) {
        return 0u;
    }
    addr = EZ80_RAM_BASE + EZ80_OS_RAM_POOL_OFFSET + emu->os.ram_pool_next;
    emu->os.ram_pool_next += size_bytes;
    return mask24(addr);
}

static uint32_t os_allocate_archive_addr(ez80_emulator_t *emu, uint32_t size_bytes)
{
    uint32_t addr;
    uint32_t end;
    if (!emu) return 0u;
    if (size_bytes == 0u) size_bytes = 1u;
    if (emu->os.archive_pool_next + size_bytes > EZ80_OS_ARCHIVE_POOL_BYTES) {
        return 0u;
    }
    addr = EZ80_OS_ARCHIVE_POOL_BASE + emu->os.archive_pool_next;
    end = addr + size_bytes;
    if (end > EZ80_FLASH_TOTAL_BYTES) {
        return 0u;
    }
    emu->os.archive_pool_next += size_bytes;
    if (end > emu->memory.flash_bytes_loaded) {
        emu->memory.flash_bytes_loaded = end;
    }
    return mask24(addr);
}

static bool os_addr_is_in_ram_pool(uint32_t addr)
{
    addr = mask24(addr);
    if (!addr_in_range(addr, EZ80_RAM_BASE, EZ80_RAM_BYTES)) return false;
    return (addr - EZ80_RAM_BASE) >= EZ80_OS_RAM_POOL_OFFSET;
}

static bool os_ram_range_fits(uint32_t ram_addr, uint32_t size_bytes)
{
    uint32_t offset;
    if (size_bytes == 0u) return false;
    ram_addr = mask24(ram_addr);
    if (!addr_in_range(ram_addr, EZ80_RAM_BASE, EZ80_RAM_BYTES)) return false;
    offset = ram_addr - EZ80_RAM_BASE;
    if (offset >= EZ80_RAM_BYTES) return false;
    return size_bytes <= (EZ80_RAM_BYTES - offset);
}

static ez80_status_t os_read_name_from_memory(const ez80_emulator_t *emu,
                                              uint32_t addr,
                                              char out_name[EZ80_OS_VAR_NAME_MAX + 1u])
{
    uint8_t value = 0u;
    uint8_t i;
    if (!emu || !out_name) return EZ80_ERR_INVALID_ARG;
    memset(out_name, 0, EZ80_OS_VAR_NAME_MAX + 1u);
    for (i = 0u; i < EZ80_OS_VAR_NAME_MAX; i++) {
        ez80_status_t status = ez80_memory_read8(emu, mask24(addr + i), &value);
        if (status != EZ80_OK) return status;
        if (value == 0u) break;
        out_name[i] = (char)toupper((unsigned char)value);
    }
    if (!os_validate_var_name(out_name)) return EZ80_ERR_INVALID_ARG;
    return EZ80_OK;
}

static void os_home_clear(ez80_emulator_t *emu)
{
    uint32_t row;
    if (!emu) return;
    for (row = 0u; row < EZ80_HOME_ROWS; row++) {
        memset(g_home_screen[row], ' ', EZ80_HOME_COLS);
    }
    emu->os.home_row = 0u;
    emu->os.home_col = 0u;
    os_sync_mmio(emu);
}

static void os_home_putc(ez80_emulator_t *emu, uint8_t ch)
{
    if (!emu) return;
    if (ch == '\n') {
        emu->os.home_col = 0u;
        emu->os.home_row = (uint8_t)((emu->os.home_row + 1u) % EZ80_HOME_ROWS);
    } else {
        g_home_screen[emu->os.home_row][emu->os.home_col] = (char)ch;
        emu->os.home_col++;
        if (emu->os.home_col >= EZ80_HOME_COLS) {
            emu->os.home_col = 0u;
            emu->os.home_row = (uint8_t)((emu->os.home_row + 1u) % EZ80_HOME_ROWS);
        }
    }
    os_sync_mmio(emu);
}

static void os_graph_clear(ez80_emulator_t *emu, uint8_t color)
{
    if (!emu) return;
    memset(g_graph_buffer, color, sizeof(g_graph_buffer));
    emu->os.graph_last_x = 0u;
    emu->os.graph_last_y = 0u;
    emu->os.graph_last_color = color;
    os_sync_mmio(emu);
}

static ez80_status_t os_graph_plot(ez80_emulator_t *emu, uint16_t x, uint16_t y, uint8_t color)
{
    uint32_t index;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (x >= EZ80_GRAPH_WIDTH || y >= EZ80_GRAPH_HEIGHT) return EZ80_ERR_OUT_OF_BOUNDS;
    index = (uint32_t)y * EZ80_GRAPH_WIDTH + (uint32_t)x;
    g_graph_buffer[index] = color;
    emu->os.graph_last_x = x;
    emu->os.graph_last_y = y;
    emu->os.graph_last_color = color;
    os_sync_mmio(emu);
    return EZ80_OK;
}

static void os_graph_render_to_lcd(ez80_emulator_t *emu)
{
    uint32_t i;
    if (!emu) return;
    for (i = 0u; i < (uint32_t)(EZ80_GRAPH_WIDTH * EZ80_GRAPH_HEIGHT); i++) {
        uint16_t shade = (uint16_t)g_graph_buffer[i];
        g_lcd_framebuffer[i] = (uint16_t)(((shade >> 3) << 11) | ((shade >> 2) << 5) | (shade >> 3));
    }
    emu->memory.mmio[EZ80_MMIO_REG_LCD_STATUS] = 0x01u;
}

static ez80_status_t os_slot_read_bytes(const ez80_emulator_t *emu,
                                        const ez80_os_var_slot_t *slot,
                                        uint8_t *out,
                                        size_t cap)
{
    uint32_t i;
    if (!emu || !slot || !out) return EZ80_ERR_INVALID_ARG;
    if (!slot->in_use) return EZ80_ERR_OUT_OF_BOUNDS;
    if (cap < slot->size_bytes) return EZ80_ERR_OUT_OF_BOUNDS;

    if (slot->archived && (!slot->loaded_in_ram || !os_addr_is_in_ram_pool(slot->ram_addr))) {
        for (i = 0u; i < slot->size_bytes; i++) {
            out[i] = flash_read_byte(emu, slot->archive_addr + i);
        }
    } else {
        uint32_t ram_offset;
        if (!os_ram_range_fits(slot->ram_addr, slot->size_bytes)) return EZ80_ERR_OUT_OF_BOUNDS;
        ram_offset = slot->ram_addr - EZ80_RAM_BASE;
        for (i = 0u; i < slot->size_bytes; i++) {
            out[i] = emu->memory.ram[ram_offset + i];
        }
    }
    return EZ80_OK;
}

static ez80_status_t os_slot_write_bytes(ez80_emulator_t *emu,
                                         ez80_os_var_slot_t *slot,
                                         const uint8_t *data,
                                         uint32_t size_bytes,
                                         bool archived)
{
    uint32_t i;
    uint32_t addr;
    if (!emu || !slot || !data) return EZ80_ERR_INVALID_ARG;
    if (size_bytes == 0u) return EZ80_ERR_INVALID_ARG;

    if (archived) {
        if (slot->archive_addr == 0u || slot->size_bytes < size_bytes) {
            addr = os_allocate_archive_addr(emu, size_bytes);
            if (addr == 0u) return EZ80_ERR_OUT_OF_BOUNDS;
            slot->archive_addr = addr;
        }
        for (i = 0u; i < size_bytes; i++) {
            flash_write_byte(emu, slot->archive_addr + i, data[i]);
        }
        slot->archived = true;
        slot->loaded_in_ram = false;
        slot->archive_crc32 = os_crc32_bytes(data, size_bytes);
        slot->ram_crc32 = 0u;
        if (slot->type != EZ80_OS_VAR_PROGRAM) {
            slot->loaded_in_ram = false;
            slot->ram_addr = 0u;
        }
    } else {
        uint32_t ram_offset;
        if (slot->ram_addr == 0u || slot->size_bytes < size_bytes) {
            addr = os_allocate_ram_addr(emu, size_bytes);
            if (addr == 0u) return EZ80_ERR_OUT_OF_BOUNDS;
            slot->ram_addr = addr;
        }
        if (!os_ram_range_fits(slot->ram_addr, size_bytes)) return EZ80_ERR_OUT_OF_BOUNDS;
        ram_offset = slot->ram_addr - EZ80_RAM_BASE;
        for (i = 0u; i < size_bytes; i++) {
            emu->memory.ram[ram_offset + i] = data[i];
        }
        slot->archived = false;
        slot->loaded_in_ram = true;
        slot->ram_crc32 = os_crc32_bytes(data, size_bytes);
        slot->archive_crc32 = 0u;
    }

    slot->size_bytes = size_bytes;
    return EZ80_OK;
}

static ez80_status_t os_slot_write_from_memory(ez80_emulator_t *emu,
                                               ez80_os_var_slot_t *slot,
                                               uint32_t src_addr,
                                               uint32_t size_bytes,
                                               bool archived)
{
    uint32_t i;
    uint32_t addr;
    uint8_t value;
    if (!emu || !slot) return EZ80_ERR_INVALID_ARG;
    if (size_bytes == 0u) return EZ80_ERR_INVALID_ARG;

    if (archived) {
        if (slot->archive_addr == 0u || slot->size_bytes < size_bytes) {
            addr = os_allocate_archive_addr(emu, size_bytes);
            if (addr == 0u) return EZ80_ERR_OUT_OF_BOUNDS;
            slot->archive_addr = addr;
        }
        for (i = 0u; i < size_bytes; i++) {
            ez80_status_t status = ez80_memory_read8(emu, mask24(src_addr + i), &value);
            if (status != EZ80_OK) return status;
            flash_write_byte(emu, slot->archive_addr + i, value);
        }
        slot->archived = true;
        slot->loaded_in_ram = false;
        slot->archive_crc32 = os_crc32_bytes(&g_flash_image[slot->archive_addr], size_bytes);
        slot->ram_crc32 = 0u;
        if (slot->type != EZ80_OS_VAR_PROGRAM) {
            slot->loaded_in_ram = false;
            slot->ram_addr = 0u;
        }
    } else {
        uint32_t ram_offset;
        if (slot->ram_addr == 0u || slot->size_bytes < size_bytes) {
            addr = os_allocate_ram_addr(emu, size_bytes);
            if (addr == 0u) return EZ80_ERR_OUT_OF_BOUNDS;
            slot->ram_addr = addr;
        }
        if (!os_ram_range_fits(slot->ram_addr, size_bytes)) return EZ80_ERR_OUT_OF_BOUNDS;
        ram_offset = slot->ram_addr - EZ80_RAM_BASE;
        for (i = 0u; i < size_bytes; i++) {
            ez80_status_t status = ez80_memory_read8(emu, mask24(src_addr + i), &value);
            if (status != EZ80_OK) return status;
            emu->memory.ram[ram_offset + i] = value;
        }
        slot->archived = false;
        slot->loaded_in_ram = true;
        slot->ram_crc32 = os_crc32_bytes(&emu->memory.ram[ram_offset], size_bytes);
        slot->archive_crc32 = 0u;
    }
    slot->size_bytes = size_bytes;
    return EZ80_OK;
}

static ez80_status_t os_slot_copy_to_memory(ez80_emulator_t *emu,
                                            const ez80_os_var_slot_t *slot,
                                            uint32_t dst_addr,
                                            uint32_t cap,
                                            uint32_t *out_size)
{
    uint32_t i;
    if (!emu || !slot) return EZ80_ERR_INVALID_ARG;
    if (!slot->in_use) return EZ80_ERR_OUT_OF_BOUNDS;
    if (cap < slot->size_bytes) return EZ80_ERR_OUT_OF_BOUNDS;

    if (slot->archived && (!slot->loaded_in_ram || !os_addr_is_in_ram_pool(slot->ram_addr))) {
        for (i = 0u; i < slot->size_bytes; i++) {
            ez80_status_t status = ez80_memory_write8(emu, mask24(dst_addr + i), flash_read_byte(emu, slot->archive_addr + i));
            if (status != EZ80_OK) return status;
        }
    } else {
        uint32_t ram_offset;
        if (!os_ram_range_fits(slot->ram_addr, slot->size_bytes)) return EZ80_ERR_OUT_OF_BOUNDS;
        ram_offset = slot->ram_addr - EZ80_RAM_BASE;
        for (i = 0u; i < slot->size_bytes; i++) {
            ez80_status_t status = ez80_memory_write8(emu, mask24(dst_addr + i), emu->memory.ram[ram_offset + i]);
            if (status != EZ80_OK) return status;
        }
    }

    if (out_size) {
        *out_size = slot->size_bytes;
    }
    return EZ80_OK;
}

static ez80_status_t os_program_archive_to_ram_internal(ez80_emulator_t *emu, ez80_os_var_slot_t *slot)
{
    uint32_t i;
    uint32_t archive_crc32;
    uint32_t ram_crc32;
    uint32_t ram_offset;

    if (!emu || !slot) return EZ80_ERR_INVALID_ARG;
    if (!slot->in_use || slot->type != EZ80_OS_VAR_PROGRAM) return EZ80_ERR_OUT_OF_BOUNDS;

    if (slot->size_bytes == 0u) {
        slot->loaded_in_ram = true;
        os_record_swap_crc(emu, 0u, 0u, true);
        return EZ80_OK;
    }

    if (!slot->archived) {
        if (!os_ram_range_fits(slot->ram_addr, slot->size_bytes)) {
            os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_INVALID_STATE);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        ram_offset = slot->ram_addr - EZ80_RAM_BASE;
        ram_crc32 = os_crc32_bytes(&emu->memory.ram[ram_offset], slot->size_bytes);
        slot->ram_crc32 = ram_crc32;
        emu->os.last_swap_archive_crc32 = ram_crc32;
        emu->os.last_swap_ram_crc32 = ram_crc32;
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_NONE);
        slot->loaded_in_ram = true;
        return EZ80_OK;
    }

    if (!addr_in_range(slot->archive_addr, EZ80_FLASH_BASE, EZ80_FLASH_TOTAL_BYTES) ||
        slot->archive_addr >= emu->memory.flash_bytes_loaded ||
        slot->size_bytes > (EZ80_FLASH_TOTAL_BYTES - slot->archive_addr) ||
        slot->size_bytes > (emu->memory.flash_bytes_loaded - slot->archive_addr)) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_ARCHIVE_RANGE);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }
    if (slot->size_bytes > EZ80_OS_RAM_POOL_BYTES) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_RAM_CAPACITY);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    if (slot->ram_addr == 0u || (slot->size_bytes > 0u && !os_addr_is_in_ram_pool(slot->ram_addr))) {
        slot->ram_addr = os_allocate_ram_addr(emu, slot->size_bytes);
        if (slot->ram_addr == 0u) {
            os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_RAM_CAPACITY);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
    }
    if (!os_ram_range_fits(slot->ram_addr, slot->size_bytes)) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_RAM_CAPACITY);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    ram_offset = slot->ram_addr - EZ80_RAM_BASE;
    for (i = 0u; i < slot->size_bytes; i++) {
        emu->memory.ram[ram_offset + i] = flash_read_byte(emu, slot->archive_addr + i);
    }
    archive_crc32 = os_crc32_bytes(&g_flash_image[slot->archive_addr], slot->size_bytes);
    if (slot->archive_crc32 != 0u && slot->archive_crc32 != archive_crc32) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_INTEGRITY);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }
    ram_crc32 = os_crc32_bytes(&emu->memory.ram[ram_offset], slot->size_bytes);
    if (archive_crc32 != ram_crc32) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_INTEGRITY);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    slot->loaded_in_ram = true;
    slot->archive_crc32 = archive_crc32;
    slot->ram_crc32 = ram_crc32;
    os_record_swap_crc(emu, archive_crc32, ram_crc32, true);
    return EZ80_OK;
}

static ez80_status_t os_program_return_to_archive_internal(ez80_emulator_t *emu, ez80_os_var_slot_t *slot)
{
    uint32_t i;
    uint32_t archive_crc32;
    uint32_t ram_crc32;
    uint32_t ram_offset;
    if (!emu || !slot) return EZ80_ERR_INVALID_ARG;
    if (!slot->in_use || slot->type != EZ80_OS_VAR_PROGRAM) return EZ80_ERR_OUT_OF_BOUNDS;
    if (!slot->archived || !slot->loaded_in_ram) return EZ80_OK;

    if (!os_ram_range_fits(slot->ram_addr, slot->size_bytes)) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_INVALID_STATE);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }
    if (!addr_in_range(slot->archive_addr, EZ80_FLASH_BASE, EZ80_FLASH_TOTAL_BYTES) ||
        slot->size_bytes > (EZ80_FLASH_TOTAL_BYTES - slot->archive_addr)) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_ARCHIVE_RANGE);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    ram_offset = slot->ram_addr - EZ80_RAM_BASE;
    ram_crc32 = os_crc32_bytes(&emu->memory.ram[ram_offset], slot->size_bytes);
    for (i = 0u; i < slot->size_bytes; i++) {
        flash_write_byte(emu, slot->archive_addr + i, emu->memory.ram[ram_offset + i]);
    }
    archive_crc32 = os_crc32_bytes(&g_flash_image[slot->archive_addr], slot->size_bytes);
    if (archive_crc32 != ram_crc32) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_INTEGRITY);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    slot->loaded_in_ram = false;
    slot->archive_crc32 = archive_crc32;
    slot->ram_crc32 = ram_crc32;
    os_record_swap_crc(emu, archive_crc32, ram_crc32, true);
    if (strncmp(emu->os.active_program_name, slot->name, EZ80_OS_VAR_NAME_MAX + 1u) == 0) {
        os_clear_active_program(emu);
    }
    return EZ80_OK;
}

static ez80_status_t os_program_launch_internal(ez80_emulator_t *emu,
                                                ez80_os_var_slot_t *slot,
                                                uint32_t *out_ram_addr)
{
    ez80_status_t status;
    int active_slot_index;

    if (!emu || !slot) return EZ80_ERR_INVALID_ARG;
    if (!slot->in_use || slot->type != EZ80_OS_VAR_PROGRAM) return EZ80_ERR_OUT_OF_BOUNDS;
    if (!emu->platform.native_unsigned_execution_allowed) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_INVALID_STATE);
        return EZ80_ERR_NOT_IMPLEMENTED;
    }

    if (emu->os.active_program_name[0] != '\0' &&
        strncmp(emu->os.active_program_name, slot->name, EZ80_OS_VAR_NAME_MAX + 1u) != 0) {
        active_slot_index = os_find_slot_index(emu, emu->os.active_program_name, EZ80_OS_VAR_PROGRAM);
        if (active_slot_index >= 0) {
            status = os_program_return_to_archive_internal(emu, &emu->os.vars[active_slot_index]);
            if (status != EZ80_OK) {
                return status;
            }
        } else {
            os_clear_active_program(emu);
        }
    }

    status = os_program_archive_to_ram_internal(emu, slot);
    if (status != EZ80_OK) {
        return status;
    }

    os_set_active_program(emu, slot);
    emu->os.launch_count++;
    os_home_putc(emu, '\n');
    os_home_putc(emu, 'R');
    os_home_putc(emu, 'U');
    os_home_putc(emu, 'N');
    os_home_putc(emu, ':');
    os_home_putc(emu, ' ');
    for (active_slot_index = 0;
         active_slot_index < (int)EZ80_OS_VAR_NAME_MAX &&
         slot->name[active_slot_index] != '\0';
         active_slot_index++) {
        os_home_putc(emu, (uint8_t)slot->name[active_slot_index]);
    }

    if (out_ram_addr) {
        *out_ram_addr = slot->ram_addr;
    }
    return EZ80_OK;
}

static ez80_status_t os_program_finish_internal(ez80_emulator_t *emu,
                                                ez80_os_var_slot_t *slot,
                                                uint32_t *out_archive_addr)
{
    ez80_status_t status;
    int i;

    if (!emu || !slot) return EZ80_ERR_INVALID_ARG;
    if (!slot->in_use || slot->type != EZ80_OS_VAR_PROGRAM) return EZ80_ERR_OUT_OF_BOUNDS;

    status = os_program_return_to_archive_internal(emu, slot);
    if (status != EZ80_OK) {
        return status;
    }
    if (strncmp(emu->os.active_program_name, slot->name, EZ80_OS_VAR_NAME_MAX + 1u) == 0) {
        os_clear_active_program(emu);
    }

    os_home_putc(emu, '\n');
    os_home_putc(emu, 'D');
    os_home_putc(emu, 'O');
    os_home_putc(emu, 'N');
    os_home_putc(emu, 'E');
    os_home_putc(emu, ':');
    os_home_putc(emu, ' ');
    for (i = 0; i < (int)EZ80_OS_VAR_NAME_MAX && slot->name[i] != '\0'; i++) {
        os_home_putc(emu, (uint8_t)slot->name[i]);
    }

    if (out_archive_addr) {
        *out_archive_addr = slot->archive_addr;
    }
    return EZ80_OK;
}

static ez80_status_t os_write_var_info_to_memory(ez80_emulator_t *emu,
                                                 uint32_t dst_addr,
                                                 uint32_t cap,
                                                 const ez80_os_var_slot_t *slot)
{
    ez80_status_t status;

    if (!emu || !slot) return EZ80_ERR_INVALID_ARG;
    if (cap < EZ80_OS_VAR_INFO_BYTES) return EZ80_ERR_OUT_OF_BOUNDS;

    status = ez80_memory_write8(emu, mask24(dst_addr + 0u), 1u);
    if (status != EZ80_OK) return status;
    status = ez80_memory_write8(emu, mask24(dst_addr + 1u), slot->archived ? 1u : 0u);
    if (status != EZ80_OK) return status;
    status = ez80_memory_write8(emu, mask24(dst_addr + 2u), slot->loaded_in_ram ? 1u : 0u);
    if (status != EZ80_OK) return status;
    status = ez80_memory_write8(emu, mask24(dst_addr + 3u), slot->type);
    if (status != EZ80_OK) return status;
    status = ez80_memory_write8(emu, mask24(dst_addr + 4u), (uint8_t)(slot->size_bytes & 0xFFu));
    if (status != EZ80_OK) return status;
    status = ez80_memory_write8(emu, mask24(dst_addr + 5u), (uint8_t)((slot->size_bytes >> 8) & 0xFFu));
    if (status != EZ80_OK) return status;
    status = ez80_memory_write8(emu, mask24(dst_addr + 6u), (uint8_t)((slot->size_bytes >> 16) & 0xFFu));
    if (status != EZ80_OK) return status;
    status = ez80_memory_write8(emu, mask24(dst_addr + 7u), (uint8_t)(slot->archive_addr & 0xFFu));
    if (status != EZ80_OK) return status;
    status = ez80_memory_write8(emu, mask24(dst_addr + 8u), (uint8_t)((slot->archive_addr >> 8) & 0xFFu));
    if (status != EZ80_OK) return status;
    status = ez80_memory_write8(emu, mask24(dst_addr + 9u), (uint8_t)((slot->archive_addr >> 16) & 0xFFu));
    if (status != EZ80_OK) return status;
    status = ez80_memory_write8(emu, mask24(dst_addr + 10u), (uint8_t)(slot->ram_addr & 0xFFu));
    if (status != EZ80_OK) return status;
    status = ez80_memory_write8(emu, mask24(dst_addr + 11u), (uint8_t)((slot->ram_addr >> 8) & 0xFFu));
    if (status != EZ80_OK) return status;
    status = ez80_memory_write8(emu, mask24(dst_addr + 12u), (uint8_t)((slot->ram_addr >> 16) & 0xFFu));
    if (status != EZ80_OK) return status;
    return EZ80_OK;
}

static ez80_status_t os_bcall_dispatch(ez80_emulator_t *emu,
                                       uint16_t vector,
                                       uint32_t arg0,
                                       uint32_t arg1,
                                       uint32_t arg2,
                                       uint32_t arg3,
                                       uint32_t *out_result)
{
    char normalized_name[EZ80_OS_VAR_NAME_MAX + 1u];
    int slot_index;
    ez80_os_var_slot_t *slot = NULL;
    uint32_t result = 0u;
    ez80_status_t status = EZ80_ERR_NOT_IMPLEMENTED;

    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!os_bcall_vector_supported(vector)) {
        emu->os.last_bcall_vector = vector;
        emu->os.last_bcall_status = (uint8_t)EZ80_ERR_NOT_IMPLEMENTED;
        emu->os.last_bcall_result = 0u;
        os_sync_mmio(emu);
        mmio_set(emu, EZ80_MMIO_REG_BCALL_STATUS, (uint8_t)EZ80_ERR_NOT_IMPLEMENTED);
        mmio_set24(emu, EZ80_MMIO_REG_BCALL_RESULT_0, 0u);
        if (out_result) {
            *out_result = 0u;
        }
        return EZ80_ERR_NOT_IMPLEMENTED;
    }

    switch (vector) {
    case EZ80_BCALL_HOME_CLEAR:
        os_home_clear(emu);
        status = EZ80_OK;
        break;
    case EZ80_BCALL_HOME_PUTC:
        os_home_putc(emu, (uint8_t)(arg0 & 0xFFu));
        status = EZ80_OK;
        break;
    case EZ80_BCALL_GRAPH_CLEAR:
        os_graph_clear(emu, (uint8_t)(arg0 & 0xFFu));
        status = EZ80_OK;
        break;
    case EZ80_BCALL_GRAPH_PLOT:
        status = os_graph_plot(emu,
                               (uint16_t)(arg0 & 0xFFFFu),
                               (uint16_t)(arg1 & 0xFFFFu),
                               (uint8_t)(arg2 & 0xFFu));
        break;
    case EZ80_BCALL_GRAPH_RENDER_TO_LCD:
        os_graph_render_to_lcd(emu);
        status = EZ80_OK;
        break;
    case EZ80_BCALL_VAR_EXISTS:
        status = os_read_name_from_memory(emu, arg0, normalized_name);
        if (status != EZ80_OK) break;
        slot_index = os_find_slot_index(emu, normalized_name, (uint8_t)(arg1 & 0xFFu));
        result = (slot_index >= 0) ? 1u : 0u;
        status = EZ80_OK;
        break;
    case EZ80_BCALL_VAR_GET_INFO:
        status = os_read_name_from_memory(emu, arg0, normalized_name);
        if (status != EZ80_OK) break;
        if (!os_valid_var_type((uint8_t)(arg1 & 0xFFu))) {
            status = EZ80_ERR_INVALID_ARG;
            break;
        }
        slot_index = os_find_slot_index(emu, normalized_name, (uint8_t)(arg1 & 0xFFu));
        if (slot_index < 0) {
            status = EZ80_ERR_OUT_OF_BOUNDS;
            break;
        }
        slot = &emu->os.vars[slot_index];
        status = os_write_var_info_to_memory(emu, arg2, arg3, slot);
        if (status == EZ80_OK) {
            result = slot->size_bytes;
        }
        break;
    case EZ80_BCALL_PROGRAM_ARCHIVE_TO_RAM:
        status = os_read_name_from_memory(emu, arg0, normalized_name);
        if (status != EZ80_OK) break;
        slot_index = os_find_slot_index(emu, normalized_name, EZ80_OS_VAR_PROGRAM);
        if (slot_index < 0) {
            os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_PROGRAM_NOT_FOUND);
            status = EZ80_ERR_OUT_OF_BOUNDS;
            break;
        }
        slot = &emu->os.vars[slot_index];
        status = os_program_archive_to_ram_internal(emu, slot);
        if (status == EZ80_OK) {
            result = slot->ram_addr;
        }
        break;
    case EZ80_BCALL_PROGRAM_RETURN_TO_ARCHIVE:
        status = os_read_name_from_memory(emu, arg0, normalized_name);
        if (status != EZ80_OK) break;
        slot_index = os_find_slot_index(emu, normalized_name, EZ80_OS_VAR_PROGRAM);
        if (slot_index < 0) {
            os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_PROGRAM_NOT_FOUND);
            status = EZ80_ERR_OUT_OF_BOUNDS;
            break;
        }
        slot = &emu->os.vars[slot_index];
        status = os_program_return_to_archive_internal(emu, slot);
        if (status == EZ80_OK) {
            result = slot->archive_addr;
        }
        break;
    case EZ80_BCALL_PROGRAM_LAUNCH:
        status = os_read_name_from_memory(emu, arg0, normalized_name);
        if (status != EZ80_OK) break;
        slot_index = os_find_slot_index(emu, normalized_name, EZ80_OS_VAR_PROGRAM);
        if (slot_index < 0) {
            os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_PROGRAM_NOT_FOUND);
            status = EZ80_ERR_OUT_OF_BOUNDS;
            break;
        }
        slot = &emu->os.vars[slot_index];
        status = os_program_launch_internal(emu, slot, &result);
        break;
    case EZ80_BCALL_PROGRAM_FINISH:
        if (arg0 == 0u) {
            if (emu->os.active_program_name[0] == '\0') {
                os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_INVALID_STATE);
                status = EZ80_ERR_OUT_OF_BOUNDS;
                break;
            }
            strncpy(normalized_name, emu->os.active_program_name, EZ80_OS_VAR_NAME_MAX);
            normalized_name[EZ80_OS_VAR_NAME_MAX] = '\0';
        } else {
            status = os_read_name_from_memory(emu, arg0, normalized_name);
            if (status != EZ80_OK) break;
        }
        slot_index = os_find_slot_index(emu, normalized_name, EZ80_OS_VAR_PROGRAM);
        if (slot_index < 0) {
            os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_PROGRAM_NOT_FOUND);
            status = EZ80_ERR_OUT_OF_BOUNDS;
            break;
        }
        slot = &emu->os.vars[slot_index];
        status = os_program_finish_internal(emu, slot, &result);
        break;
    case EZ80_BCALL_VAR_DELETE:
        status = os_read_name_from_memory(emu, arg0, normalized_name);
        if (status != EZ80_OK) break;
        slot_index = os_find_slot_index(emu, normalized_name, (uint8_t)(arg1 & 0xFFu));
        if (slot_index < 0) {
            status = EZ80_ERR_OUT_OF_BOUNDS;
            break;
        }
        if (emu->os.vars[slot_index].type == EZ80_OS_VAR_PROGRAM &&
            strncmp(emu->os.active_program_name,
                    emu->os.vars[slot_index].name,
                    EZ80_OS_VAR_NAME_MAX + 1u) == 0) {
            os_clear_active_program(emu);
        }
        memset(&emu->os.vars[slot_index], 0, sizeof(emu->os.vars[slot_index]));
        result = 1u;
        status = EZ80_OK;
        break;
    case EZ80_BCALL_VAR_GET:
        status = os_read_name_from_memory(emu, arg0, normalized_name);
        if (status != EZ80_OK) break;
        slot_index = os_find_slot_index(emu, normalized_name, (uint8_t)(arg1 & 0xFFu));
        if (slot_index < 0) {
            status = EZ80_ERR_OUT_OF_BOUNDS;
            break;
        }
        status = os_slot_copy_to_memory(emu,
                                        &emu->os.vars[slot_index],
                                        arg2,
                                        arg3,
                                        &result);
        break;
    case EZ80_BCALL_VAR_PUT:
        status = os_read_name_from_memory(emu, arg0, normalized_name);
        if (status != EZ80_OK) break;
        if (!os_valid_var_type((uint8_t)(arg1 & 0xFFu))) {
            status = EZ80_ERR_INVALID_ARG;
            break;
        }
        slot_index = os_find_slot_index(emu, normalized_name, (uint8_t)(arg1 & 0xFFu));
        if (slot_index < 0) {
            slot_index = os_find_free_slot_index(emu);
            if (slot_index < 0) {
                status = EZ80_ERR_OUT_OF_BOUNDS;
                break;
            }
            memset(&emu->os.vars[slot_index], 0, sizeof(emu->os.vars[slot_index]));
            emu->os.vars[slot_index].in_use = true;
            emu->os.vars[slot_index].type = (uint8_t)(arg1 & 0xFFu);
            strncpy(emu->os.vars[slot_index].name, normalized_name, EZ80_OS_VAR_NAME_MAX);
            emu->os.vars[slot_index].name[EZ80_OS_VAR_NAME_MAX] = '\0';
        }
        slot = &emu->os.vars[slot_index];
        status = os_slot_write_from_memory(emu, slot, arg2, arg3, (arg1 & 0x100u) != 0u);
        if (status == EZ80_OK) {
            result = slot->size_bytes;
        }
        break;
    default:
        status = EZ80_ERR_NOT_IMPLEMENTED;
        break;
    }

    emu->os.last_bcall_vector = vector;
    emu->os.last_bcall_status = (uint8_t)status;
    emu->os.last_bcall_result = result;
    os_sync_mmio(emu);
    mmio_set(emu, EZ80_MMIO_REG_BCALL_STATUS, (uint8_t)status);
    mmio_set24(emu, EZ80_MMIO_REG_BCALL_RESULT_0, result);
    if (out_result) {
        *out_result = result;
    }
    (void)arg3;
    return status;
}

static void os_execute_mmio_bcall(ez80_emulator_t *emu)
{
    uint16_t vector;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
    if (!emu) return;
    vector = (uint16_t)emu->memory.mmio[EZ80_MMIO_REG_BCALL_VECTOR_LO] |
             ((uint16_t)emu->memory.mmio[EZ80_MMIO_REG_BCALL_VECTOR_HI] << 8);
    arg0 = mmio_get24(emu, EZ80_MMIO_REG_BCALL_ARG0_0);
    arg1 = mmio_get24(emu, EZ80_MMIO_REG_BCALL_ARG1_0);
    arg2 = mmio_get24(emu, EZ80_MMIO_REG_BCALL_ARG2_0);
    arg3 = mmio_get24(emu, EZ80_MMIO_REG_BCALL_ARG3_0);
    (void)os_bcall_dispatch(emu, vector, arg0, arg1, arg2, arg3, NULL);
    emu->memory.mmio[EZ80_MMIO_REG_BCALL_EXECUTE] = 0u;
}

ez80_status_t ez80_emulator_init(ez80_emulator_t *emu)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    memset(emu, 0, sizeof(*emu));
    emu->initialized = true;
    platform_set_defaults(emu);
    memory_clear_runtime_state(emu);
    cpu_reset_runtime_state(&emu->cpu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_reset(ez80_emulator_t *emu)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    cpu_reset_runtime_state(&emu->cpu);
    emu->platform.artifice_bypass_active = false;
    emu->platform.artifice_entry_method = EZ80_ARTIFICE_ENTRY_NONE;
    emu->platform.artifice_key_window_count = 0u;
    emu->platform.dngrade_last_failure = EZ80_DNGRADE_FAILURE_NONE;
    emu->platform.dngrade_last_port = 0u;
    emu->platform.dngrade_last_strategy = EZ80_DNGRADE_STRATEGY_NONE;
    emu->platform.dngrade_last_attempt_status = EZ80_DNGRADE_ATTEMPT_STATUS_NONE;
    emu->platform.dngrade_last_timeout = false;
    emu->platform.dngrade_last_watchdog = false;
    emu->platform.dngrade_last_write_ok = false;
    emu->platform.dngrade_last_cycles = 0u;
    emu->platform.port28_unlock_read_latch = 0u;
    emu->platform.writeflash_last_status = EZ80_WRITEFLASH_STATUS_NONE;
    emu->platform.writeflash_last_addr = 0u;
    emu->platform.writeflash_last_requested = 0u;
    emu->platform.writeflash_last_before = 0u;
    emu->platform.writeflash_last_after = 0u;
    memset(emu->platform.artifice_key_window, 0, sizeof(emu->platform.artifice_key_window));
    flash_reset_protection(emu);
    os_set_defaults(emu);
    hardware_set_defaults(emu);
    platform_recompute_state(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_load_rom(ez80_emulator_t *emu, const char *rom_path)
{
    FILE *f;
    size_t bytes_read;
    size_t window_bytes;

    if (!emu || !rom_path || rom_path[0] == '\0') return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    f = fopen(rom_path, "rb");
    if (!f) return EZ80_ERR_ROM_FORMAT;

    memset(g_flash_image, 0, sizeof(g_flash_image));
    memset(emu->memory.flash_window, 0, sizeof(emu->memory.flash_window));
    bytes_read = fread(g_flash_image, 1u, sizeof(g_flash_image), f);
    if (ferror(f)) {
        fclose(f);
        emu->memory.flash_bytes_loaded = 0u;
        emu->rom_loaded = false;
        return EZ80_ERR_ROM_FORMAT;
    }
    fclose(f);

    if (bytes_read == 0u) {
        emu->memory.flash_bytes_loaded = 0u;
        emu->rom_loaded = false;
        return EZ80_ERR_ROM_FORMAT;
    }

    window_bytes = bytes_read;
    if (window_bytes > EZ80_FLASH_WINDOW_BYTES) {
        window_bytes = EZ80_FLASH_WINDOW_BYTES;
    }
    if (window_bytes > 0u) {
        memcpy(emu->memory.flash_window, g_flash_image, window_bytes);
    }

    emu->memory.flash_bytes_loaded = (uint32_t)bytes_read;
    platform_set_defaults(emu);
    memory_clear_runtime_state(emu);
    cpu_reset_runtime_state(&emu->cpu);
    if (rom_looks_like_adl_boot_vectors(g_flash_image, bytes_read)) {
        emu->cpu.adl_mode = false;
        emu->cpu.adl_encoding = false;
    }
    strncpy(emu->loaded_rom_path, rom_path, sizeof(emu->loaded_rom_path) - 1u);
    emu->loaded_rom_path[sizeof(emu->loaded_rom_path) - 1u] = '\0';
    emu->rom_loaded = true;
    platform_detect_os_version_from_loaded_rom(emu);
    platform_set_dngrade_failure(emu, EZ80_DNGRADE_FAILURE_NONE, 0u);
    mmio_set(emu, EZ80_MMIO_REG_ARTIFICE_LAST_KEY, 0u);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_set_os_version(ez80_emulator_t *emu,
                                           uint8_t major,
                                           uint8_t minor,
                                           uint8_t patch)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (major == 0u || major > 9u || minor > 99u) return EZ80_ERR_INVALID_ARG;

    emu->platform.os_major = major;
    emu->platform.os_minor = minor;
    emu->platform.os_patch = patch;
    emu->platform.os_version_detected = false;
    emu->platform.artifice_bypass_active = false;
    emu->platform.artifice_entry_method = EZ80_ARTIFICE_ENTRY_NONE;
    emu->platform.artifice_key_window_count = 0u;
    emu->platform.dngrade_last_failure = EZ80_DNGRADE_FAILURE_NONE;
    emu->platform.dngrade_last_port = 0u;
    emu->platform.dngrade_last_strategy = EZ80_DNGRADE_STRATEGY_NONE;
    emu->platform.dngrade_last_attempt_status = EZ80_DNGRADE_ATTEMPT_STATUS_NONE;
    emu->platform.dngrade_last_timeout = false;
    emu->platform.dngrade_last_watchdog = false;
    emu->platform.dngrade_last_write_ok = false;
    emu->platform.dngrade_last_cycles = 0u;
    emu->platform.port28_unlock_read_latch = 0u;
    emu->platform.writeflash_last_status = EZ80_WRITEFLASH_STATUS_NONE;
    emu->platform.writeflash_last_addr = 0u;
    emu->platform.writeflash_last_requested = 0u;
    emu->platform.writeflash_last_before = 0u;
    emu->platform.writeflash_last_after = 0u;
    memset(emu->platform.artifice_key_window, 0, sizeof(emu->platform.artifice_key_window));
    flash_reset_protection(emu);
    platform_recompute_state(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_inject_key(ez80_emulator_t *emu, uint8_t key_code)
{
    uint8_t i;
    uint8_t row;
    uint8_t col;
    uint8_t before;
    bool key_transition;

    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    if (emu->platform.artifice_key_window_count < EZ80_ARTIFICE_KEY_SEQ_LEN) {
        emu->platform.artifice_key_window[emu->platform.artifice_key_window_count] = key_code;
        emu->platform.artifice_key_window_count++;
    } else {
        for (i = 1u; i < EZ80_ARTIFICE_KEY_SEQ_LEN; i++) {
            emu->platform.artifice_key_window[i - 1u] = emu->platform.artifice_key_window[i];
        }
        emu->platform.artifice_key_window[EZ80_ARTIFICE_KEY_SEQ_LEN - 1u] = key_code;
    }

    mmio_set(emu, EZ80_MMIO_REG_ARTIFICE_LAST_KEY, key_code);
    row = (uint8_t)((key_code >> 3) & 0x07u);
    col = (uint8_t)(key_code & 0x07u);
    before = emu->hw.keypad_rows[row];
    emu->hw.keypad_rows[row] &= (uint8_t)~(1u << col);
    key_transition = ((before & (uint8_t)(1u << col)) != 0u) &&
                     ((emu->hw.keypad_rows[row] & (uint8_t)(1u << col)) == 0u);

    if (emu->hw.kbd_irq_on_keypress && key_transition) {
        hardware_raise_irq(emu, EZ80_IRQ_SOURCE_KEYPAD);
    } else if (key_transition) {
        pm_note_wake_event(emu, EZ80_PM_WAKE_KEYPAD, true);
    }
    emu->hw.kbd_last_state = keyboard_compute_group_data(emu);
    mmio_set(emu, EZ80_MMIO_REG_KEY_STATUS, emu->hw.kbd_last_state);
    hardware_sync_ports(emu);
    hardware_sync_mmio(emu);

    if (platform_os_is_locked(emu) && platform_artifice_sequence_match(&emu->platform)) {
        platform_activate_artifice(emu, EZ80_ARTIFICE_ENTRY_KEY_SEQUENCE);
    }

    return EZ80_OK;
}

ez80_status_t ez80_emulator_apply_artifice_memory_patch(ez80_emulator_t *emu,
                                                         uint32_t addr,
                                                         uint8_t value)
{
    ez80_status_t status;

    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    status = ez80_memory_write8(emu, addr, value);
    if (status != EZ80_OK) {
        return status;
    }

    if (platform_os_is_locked(emu) &&
        mask24(addr) == mask24(EZ80_ARTIFICE_PATCH_ADDR) &&
        value == EZ80_ARTIFICE_PATCH_MAGIC) {
        platform_activate_artifice(emu, EZ80_ARTIFICE_ENTRY_MEMORY_PATCH);
    }

    return EZ80_OK;
}

static void dngrade_result_init(ez80_dngrade_attempt_result_t *result,
                                uint8_t strategy,
                                uint32_t target_flash_addr,
                                uint8_t requested_value)
{
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->strategy = strategy;
    result->attempt_status = EZ80_DNGRADE_ATTEMPT_STATUS_NONE;
    result->failure_reason = EZ80_DNGRADE_FAILURE_NONE;
    result->failure_port = 0u;
    result->target_flash_addr = mask24(target_flash_addr);
    result->requested_value = requested_value;
}

static bool dngrade_consume_cycles(ez80_emulator_t *emu,
                                   ez80_dngrade_attempt_result_t *result,
                                   uint32_t add_cycles,
                                   uint32_t timeout_cycles,
                                   uint32_t watchdog_cycles)
{
    uint32_t current;
    uint64_t requested;
    bool timeout_exceeded;
    bool watchdog_exceeded;
    uint32_t trigger_limit;
    uint32_t consumed;

    if (!emu || !result) return false;
    if (add_cycles == 0u) return true;

    current = result->cycles_executed;
    requested = (uint64_t)current + (uint64_t)add_cycles;
    timeout_exceeded = (timeout_cycles > 0u) && (requested > timeout_cycles);
    watchdog_exceeded = (watchdog_cycles > 0u) && (requested > watchdog_cycles);

    if (!timeout_exceeded && !watchdog_exceeded) {
        result->cycles_executed = (uint32_t)requested;
        hardware_advance_cycles(emu, add_cycles);
        return true;
    }

    if (timeout_exceeded && (!watchdog_exceeded || timeout_cycles <= watchdog_cycles)) {
        trigger_limit = timeout_cycles;
        result->timed_out = true;
        result->failure_reason = EZ80_DNGRADE_FAILURE_TIMEOUT;
        result->failure_port = 0u;
    } else {
        trigger_limit = watchdog_cycles;
        result->watchdog_reset = true;
        result->failure_reason = EZ80_DNGRADE_FAILURE_WATCHDOG_RESET;
        result->failure_port = 0u;
    }

    if (trigger_limit > current) {
        consumed = trigger_limit - current;
        hardware_advance_cycles(emu, consumed);
    }
    result->cycles_executed = trigger_limit;
    return false;
}

ez80_status_t ez80_emulator_simulate_dngrade_attempt(ez80_emulator_t *emu,
                                                      uint8_t strategy,
                                                      uint32_t target_flash_addr,
                                                      uint8_t value,
                                                      uint32_t timeout_cycles,
                                                      uint32_t watchdog_cycles,
                                                      ez80_dngrade_attempt_result_t *out_result)
{
    ez80_dngrade_attempt_result_t result;
    ez80_status_t status;

    if (!emu || !out_result) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    target_flash_addr = mask24(target_flash_addr);
    if (target_flash_addr >= EZ80_FLASH_TOTAL_BYTES) return EZ80_ERR_INVALID_ARG;

    dngrade_result_init(&result, strategy, target_flash_addr, value);
    result.observed_before = flash_read_byte(emu, target_flash_addr);
    result.observed_after = result.observed_before;
    platform_set_dngrade_failure(emu, EZ80_DNGRADE_FAILURE_NONE, 0u);

    switch (strategy) {
    case EZ80_DNGRADE_STRATEGY_DIRECT_FLASH_WRITE:
        if (!dngrade_consume_cycles(emu,
                                    &result,
                                    EZ80_DNGRADE_CYCLES_PRECHECK,
                                    timeout_cycles,
                                    watchdog_cycles)) {
            break;
        }
        if (!dngrade_consume_cycles(emu,
                                    &result,
                                    EZ80_DNGRADE_CYCLES_FLASH_WRITE,
                                    timeout_cycles,
                                    watchdog_cycles)) {
            break;
        }
        result.write_attempted = true;
        status = flash_writeflashbyte(emu, target_flash_addr, value);
        if (status == EZ80_OK) {
            result.write_succeeded = true;
        } else {
            result.failure_reason = emu->platform.dngrade_last_failure;
            result.failure_port = emu->platform.dngrade_last_port;
        }
        break;

    case EZ80_DNGRADE_STRATEGY_PORT28_UNLOCK_THEN_WRITE:
        if (!dngrade_consume_cycles(emu,
                                    &result,
                                    EZ80_DNGRADE_CYCLES_PRECHECK,
                                    timeout_cycles,
                                    watchdog_cycles)) {
            break;
        }
        if (!dngrade_consume_cycles(emu,
                                    &result,
                                    EZ80_DNGRADE_CYCLES_PORT_WRITE,
                                    timeout_cycles,
                                    watchdog_cycles)) {
            break;
        }
        (void)ez80_memory_write8(emu, EZ80_PORT_BASE + EZ80_FLASH_UNLOCK_PORT, EZ80_FLASH_UNLOCK_MASK);
        if (!dngrade_consume_cycles(emu,
                                    &result,
                                    EZ80_DNGRADE_CYCLES_FLASH_WRITE,
                                    timeout_cycles,
                                    watchdog_cycles)) {
            break;
        }
        result.write_attempted = true;
        status = flash_writeflashbyte(emu, target_flash_addr, value);
        if (status == EZ80_OK) {
            result.write_succeeded = true;
        } else {
            result.failure_reason = emu->platform.dngrade_last_failure;
            result.failure_port = emu->platform.dngrade_last_port;
        }
        break;

    case EZ80_DNGRADE_STRATEGY_CERT_UNLOCK_THEN_WRITE:
        if (!dngrade_consume_cycles(emu,
                                    &result,
                                    EZ80_DNGRADE_CYCLES_PRECHECK,
                                    timeout_cycles,
                                    watchdog_cycles)) {
            break;
        }
        if (!dngrade_consume_cycles(emu,
                                    &result,
                                    EZ80_DNGRADE_CYCLES_PORT_WRITE,
                                    timeout_cycles,
                                    watchdog_cycles)) {
            break;
        }
        (void)ez80_memory_write8(emu,
                                 EZ80_PORT_BASE + EZ80_FLASH_UNLOCK_PORT,
                                 (uint8_t)(EZ80_FLASH_UNLOCK_MASK | EZ80_FLASH_UNLOCK_OS58_MASK));
        if (!dngrade_consume_cycles(emu,
                                    &result,
                                    EZ80_DNGRADE_CYCLES_PORT_WRITE,
                                    timeout_cycles,
                                    watchdog_cycles)) {
            break;
        }
        (void)ez80_memory_write8(emu,
                                 EZ80_PORT_BASE + EZ80_FLASH_PROTECT_PORT,
                                 EZ80_FLASH_CERT_WRITE_MASK);
        if (!dngrade_consume_cycles(emu,
                                    &result,
                                    EZ80_DNGRADE_CYCLES_FLASH_WRITE,
                                    timeout_cycles,
                                    watchdog_cycles)) {
            break;
        }
        result.write_attempted = true;
        status = flash_writeflashbyte(emu, target_flash_addr, value);
        if (status == EZ80_OK) {
            result.write_succeeded = true;
        } else {
            result.failure_reason = emu->platform.dngrade_last_failure;
            result.failure_port = emu->platform.dngrade_last_port;
        }
        break;

    default:
        result.failure_reason = EZ80_DNGRADE_FAILURE_INVALID_STRATEGY;
        result.failure_port = 0u;
        break;
    }

    if ((result.write_attempted || result.write_succeeded) &&
        !result.timed_out &&
        !result.watchdog_reset) {
        if (!dngrade_consume_cycles(emu,
                                    &result,
                                    EZ80_DNGRADE_CYCLES_VERIFY,
                                    timeout_cycles,
                                    watchdog_cycles)) {
            result.write_succeeded = false;
        }
    }

    result.observed_after = flash_read_byte(emu, target_flash_addr);

    if (result.write_succeeded) {
        result.attempt_status = EZ80_DNGRADE_ATTEMPT_STATUS_SUCCESS;
        result.failure_reason = EZ80_DNGRADE_FAILURE_NONE;
        result.failure_port = 0u;
        platform_set_dngrade_failure(emu, EZ80_DNGRADE_FAILURE_NONE, 0u);
    } else {
        result.attempt_status = EZ80_DNGRADE_ATTEMPT_STATUS_FAILURE;
        if (result.failure_reason == EZ80_DNGRADE_FAILURE_NONE && result.write_attempted) {
            result.failure_reason = emu->platform.dngrade_last_failure;
            result.failure_port = emu->platform.dngrade_last_port;
        }
    }

    platform_record_dngrade_attempt(emu, &result);
    *out_result = result;
    return EZ80_OK;
}

ez80_status_t ez80_emulator_bcall(ez80_emulator_t *emu,
                                  uint16_t vector,
                                  uint32_t arg0,
                                  uint32_t arg1,
                                  uint32_t arg2,
                                  uint32_t arg3,
                                  uint32_t *out_result)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    return os_bcall_dispatch(emu, vector, arg0, arg1, arg2, arg3, out_result);
}

ez80_status_t ez80_emulator_os_var_write(ez80_emulator_t *emu,
                                         const char *name,
                                         uint8_t type,
                                         const uint8_t *data,
                                         size_t size,
                                         bool archived)
{
    char normalized_name[EZ80_OS_VAR_NAME_MAX + 1u];
    int slot_index;
    ez80_os_var_slot_t *slot;

    if (!emu || !name || !data) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (size == 0u || size > 0x00FFFFFFu) return EZ80_ERR_INVALID_ARG;
    if (!os_valid_var_type(type)) return EZ80_ERR_INVALID_ARG;
    if (!os_validate_var_name(name)) return EZ80_ERR_INVALID_ARG;

    os_normalize_var_name(name, normalized_name);
    slot_index = os_find_slot_index(emu, normalized_name, type);
    if (slot_index < 0) {
        slot_index = os_find_free_slot_index(emu);
        if (slot_index < 0) return EZ80_ERR_OUT_OF_BOUNDS;
        memset(&emu->os.vars[slot_index], 0, sizeof(emu->os.vars[slot_index]));
        emu->os.vars[slot_index].in_use = true;
        emu->os.vars[slot_index].type = type;
        strncpy(emu->os.vars[slot_index].name, normalized_name, EZ80_OS_VAR_NAME_MAX);
        emu->os.vars[slot_index].name[EZ80_OS_VAR_NAME_MAX] = '\0';
    }
    slot = &emu->os.vars[slot_index];
    if (os_slot_write_bytes(emu, slot, data, (uint32_t)size, archived) != EZ80_OK) {
        return EZ80_ERR_OUT_OF_BOUNDS;
    }
    os_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_os_var_read(const ez80_emulator_t *emu,
                                        const char *name,
                                        uint8_t type,
                                        uint8_t *out,
                                        size_t cap,
                                        size_t *out_size)
{
    char normalized_name[EZ80_OS_VAR_NAME_MAX + 1u];
    int slot_index;
    ez80_status_t status;
    if (!emu || !name || !out) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!os_valid_var_type(type)) return EZ80_ERR_INVALID_ARG;
    if (!os_validate_var_name(name)) return EZ80_ERR_INVALID_ARG;
    os_normalize_var_name(name, normalized_name);
    slot_index = os_find_slot_index(emu, normalized_name, type);
    if (slot_index < 0) return EZ80_ERR_OUT_OF_BOUNDS;
    status = os_slot_read_bytes(emu, &emu->os.vars[slot_index], out, cap);
    if (status != EZ80_OK) return status;
    if (out_size) {
        *out_size = emu->os.vars[slot_index].size_bytes;
    }
    return EZ80_OK;
}

ez80_status_t ez80_emulator_os_var_delete(ez80_emulator_t *emu,
                                          const char *name,
                                          uint8_t type)
{
    char normalized_name[EZ80_OS_VAR_NAME_MAX + 1u];
    int slot_index;
    if (!emu || !name) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!os_valid_var_type(type)) return EZ80_ERR_INVALID_ARG;
    if (!os_validate_var_name(name)) return EZ80_ERR_INVALID_ARG;
    os_normalize_var_name(name, normalized_name);
    slot_index = os_find_slot_index(emu, normalized_name, type);
    if (slot_index < 0) return EZ80_ERR_OUT_OF_BOUNDS;
    if (type == EZ80_OS_VAR_PROGRAM &&
        strncmp(emu->os.active_program_name,
                emu->os.vars[slot_index].name,
                EZ80_OS_VAR_NAME_MAX + 1u) == 0) {
        os_clear_active_program(emu);
    }
    memset(&emu->os.vars[slot_index], 0, sizeof(emu->os.vars[slot_index]));
    os_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_os_var_get_info(const ez80_emulator_t *emu,
                                            const char *name,
                                            uint8_t type,
                                            ez80_os_var_info_t *out_info)
{
    char normalized_name[EZ80_OS_VAR_NAME_MAX + 1u];
    int slot_index;
    const ez80_os_var_slot_t *slot;
    if (!emu || !name || !out_info) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!os_valid_var_type(type)) return EZ80_ERR_INVALID_ARG;
    if (!os_validate_var_name(name)) return EZ80_ERR_INVALID_ARG;
    os_normalize_var_name(name, normalized_name);
    slot_index = os_find_slot_index(emu, normalized_name, type);
    memset(out_info, 0, sizeof(*out_info));
    if (slot_index < 0) return EZ80_ERR_OUT_OF_BOUNDS;
    slot = &emu->os.vars[slot_index];
    out_info->exists = true;
    out_info->type = slot->type;
    strncpy(out_info->name, slot->name, EZ80_OS_VAR_NAME_MAX);
    out_info->name[EZ80_OS_VAR_NAME_MAX] = '\0';
    out_info->archived = slot->archived;
    out_info->loaded_in_ram = slot->loaded_in_ram;
    out_info->size_bytes = slot->size_bytes;
    out_info->archive_addr = slot->archive_addr;
    out_info->ram_addr = slot->ram_addr;
    return EZ80_OK;
}

ez80_status_t ez80_emulator_load_program_file(ez80_emulator_t *emu,
                                              const char *program_name,
                                              const char *program_path,
                                              bool archived)
{
    FILE *f;
    long file_size;
    uint8_t *buffer;
    size_t bytes_read;
    ez80_status_t status;
    if (!emu || !program_name || !program_path) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!os_validate_var_name(program_name)) return EZ80_ERR_INVALID_ARG;

    f = fopen(program_path, "rb");
    if (!f) return EZ80_ERR_ROM_FORMAT;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return EZ80_ERR_ROM_FORMAT;
    }
    file_size = ftell(f);
    if (file_size <= 0 || file_size > 0x00FFFFFFl) {
        fclose(f);
        return EZ80_ERR_ROM_FORMAT;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return EZ80_ERR_ROM_FORMAT;
    }
    buffer = (uint8_t *)malloc((size_t)file_size);
    if (!buffer) {
        fclose(f);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }
    bytes_read = fread(buffer, 1u, (size_t)file_size, f);
    fclose(f);
    if (bytes_read != (size_t)file_size) {
        free(buffer);
        return EZ80_ERR_ROM_FORMAT;
    }

    status = ez80_emulator_os_var_write(emu,
                                        program_name,
                                        EZ80_OS_VAR_PROGRAM,
                                        buffer,
                                        (size_t)file_size,
                                        archived);
    free(buffer);
    return status;
}

ez80_status_t ez80_emulator_archive_program_to_ram(ez80_emulator_t *emu,
                                                   const char *program_name)
{
    char normalized_name[EZ80_OS_VAR_NAME_MAX + 1u];
    int slot_index;
    ez80_status_t status;
    if (!emu || !program_name) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!os_validate_var_name(program_name)) return EZ80_ERR_INVALID_ARG;
    os_normalize_var_name(program_name, normalized_name);
    slot_index = os_find_slot_index(emu, normalized_name, EZ80_OS_VAR_PROGRAM);
    if (slot_index < 0) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_PROGRAM_NOT_FOUND);
        os_sync_mmio(emu);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }
    status = os_program_archive_to_ram_internal(emu, &emu->os.vars[slot_index]);
    os_sync_mmio(emu);
    return status;
}

ez80_status_t ez80_emulator_return_program_to_archive(ez80_emulator_t *emu,
                                                      const char *program_name)
{
    char normalized_name[EZ80_OS_VAR_NAME_MAX + 1u];
    int slot_index;
    ez80_status_t status;
    if (!emu || !program_name) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!os_validate_var_name(program_name)) return EZ80_ERR_INVALID_ARG;
    os_normalize_var_name(program_name, normalized_name);
    slot_index = os_find_slot_index(emu, normalized_name, EZ80_OS_VAR_PROGRAM);
    if (slot_index < 0) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_PROGRAM_NOT_FOUND);
        os_sync_mmio(emu);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }
    status = os_program_return_to_archive_internal(emu, &emu->os.vars[slot_index]);
    os_sync_mmio(emu);
    return status;
}

ez80_status_t ez80_emulator_launch_program(ez80_emulator_t *emu,
                                           const char *program_name,
                                           uint32_t *out_ram_addr)
{
    char normalized_name[EZ80_OS_VAR_NAME_MAX + 1u];
    int slot_index;
    ez80_status_t status;
    if (!emu || !program_name) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!os_validate_var_name(program_name)) return EZ80_ERR_INVALID_ARG;
    os_normalize_var_name(program_name, normalized_name);
    slot_index = os_find_slot_index(emu, normalized_name, EZ80_OS_VAR_PROGRAM);
    if (slot_index < 0) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_PROGRAM_NOT_FOUND);
        os_sync_mmio(emu);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }
    status = os_program_launch_internal(emu, &emu->os.vars[slot_index], out_ram_addr);
    os_sync_mmio(emu);
    return status;
}

ez80_status_t ez80_emulator_finish_program(ez80_emulator_t *emu,
                                           const char *program_name,
                                           uint32_t *out_archive_addr)
{
    char normalized_name[EZ80_OS_VAR_NAME_MAX + 1u];
    int slot_index;
    ez80_status_t status;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!program_name || program_name[0] == '\0') {
        if (emu->os.active_program_name[0] == '\0') {
            os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_INVALID_STATE);
            os_sync_mmio(emu);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        strncpy(normalized_name, emu->os.active_program_name, EZ80_OS_VAR_NAME_MAX);
        normalized_name[EZ80_OS_VAR_NAME_MAX] = '\0';
    } else {
        if (!os_validate_var_name(program_name)) return EZ80_ERR_INVALID_ARG;
        os_normalize_var_name(program_name, normalized_name);
    }

    slot_index = os_find_slot_index(emu, normalized_name, EZ80_OS_VAR_PROGRAM);
    if (slot_index < 0) {
        os_set_last_swap_failure(emu, EZ80_OS_SWAP_FAILURE_PROGRAM_NOT_FOUND);
        os_sync_mmio(emu);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }
    status = os_program_finish_internal(emu, &emu->os.vars[slot_index], out_archive_addr);
    os_sync_mmio(emu);
    return status;
}

ez80_status_t ez80_emulator_home_get_line(const ez80_emulator_t *emu,
                                          uint8_t row,
                                          char *out,
                                          size_t cap)
{
    if (!emu || !out) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (row >= EZ80_HOME_ROWS || cap < (EZ80_HOME_COLS + 1u)) return EZ80_ERR_INVALID_ARG;
    memcpy(out, g_home_screen[row], EZ80_HOME_COLS);
    out[EZ80_HOME_COLS] = '\0';
    return EZ80_OK;
}

ez80_status_t ez80_emulator_graph_get_pixel(const ez80_emulator_t *emu,
                                            uint16_t x,
                                            uint16_t y,
                                            uint8_t *out)
{
    uint32_t index;
    if (!emu || !out) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (x >= EZ80_GRAPH_WIDTH || y >= EZ80_GRAPH_HEIGHT) return EZ80_ERR_OUT_OF_BOUNDS;
    index = (uint32_t)y * EZ80_GRAPH_WIDTH + (uint32_t)x;
    *out = g_graph_buffer[index];
    return EZ80_OK;
}

ez80_status_t ez80_emulator_lcd_get_pixel(const ez80_emulator_t *emu,
                                          uint16_t x,
                                          uint16_t y,
                                          uint16_t *out)
{
    uint32_t index;
    if (!emu || !out) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (x >= EZ80_GRAPH_WIDTH || y >= EZ80_GRAPH_HEIGHT) return EZ80_ERR_OUT_OF_BOUNDS;
    index = (uint32_t)y * EZ80_GRAPH_WIDTH + (uint32_t)x;
    *out = g_lcd_framebuffer[index];
    return EZ80_OK;
}

static ez80_status_t ez80_emulator_step_impl(ez80_emulator_t *emu, ez80_instruction_t *decoded)
{
    ez80_instruction_t instruction;
    ez80_status_t decode_status;
    ez80_status_t status;
    uint32_t pc;
    uint32_t next_pc;
    uint8_t fetch_bytes[6];
    uint8_t trace_opcode;
    uint8_t prefix_len = 0u;
    size_t fetch_size;
    const uint8_t *decode_stream;
    size_t decode_size;

    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!emu->rom_loaded) return EZ80_ERR_ROM_FORMAT;
    if (!emu->platform.native_unsigned_execution_allowed) {
        return EZ80_ERR_NOT_IMPLEMENTED;
    }

    pc = mask24(emu->cpu.pc);
    fetch_size = memory_fetch_stream(emu, pc, fetch_bytes, sizeof(fetch_bytes));
    if (fetch_size == 0u) {
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    cpu_clear_instruction_mode_override(&emu->cpu);
    decode_stream = fetch_bytes;
    decode_size = fetch_size;
    trace_opcode = fetch_bytes[0];

    if (ez80_suffix_prefix_modes(fetch_bytes[0],
                                 &emu->cpu.adl_mode_override,
                                 &emu->cpu.adl_encoding_override)) {
        if (fetch_size < 2u) {
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        emu->cpu.adl_mode_override_active = true;
        emu->cpu.adl_encoding_override_active = true;
        prefix_len = 1u;
        decode_stream = &fetch_bytes[1];
        decode_size = fetch_size - 1u;
    }

    decode_status = ez80_decode_instruction_adl(decode_stream, decode_size,
                                                cpu_effective_adl_encoding(&emu->cpu), &instruction);
    if (decode_status == EZ80_OK) {
        instruction.length = (uint8_t)(instruction.length + prefix_len);
    }
    if (decoded) {
        *decoded = instruction;
    }
    if (decode_status != EZ80_OK) {
        return decode_status;
    }

    next_pc = mask24(pc + instruction.length);

    /* Record trace entry if tracing is enabled */
    if (emu->hw.trace_enabled) {
        uint32_t idx = emu->hw.trace_head % EZ80_TRACE_BUFFER_SIZE;
        emu->hw.trace_buf[idx].pc = pc;
        emu->hw.trace_buf[idx].opcode = trace_opcode;
        emu->hw.trace_buf[idx].length = instruction.length;
        {
            uint8_t ti;
            for (ti = 0u; ti < instruction.length && ti < 6u; ti++) {
                emu->hw.trace_buf[idx].bytes[ti] = fetch_bytes[ti];
            }
        }
        emu->hw.trace_head = (emu->hw.trace_head + 1u) % EZ80_TRACE_BUFFER_SIZE;
        if (emu->hw.trace_count < EZ80_TRACE_BUFFER_SIZE) {
            emu->hw.trace_count++;
        }
    }

    if (instruction.opcode >= 0x40u && instruction.opcode <= 0x7Fu && instruction.opcode != 0x76u) {
        uint8_t dst = (uint8_t)((instruction.opcode >> 3) & 0x07u);
        uint8_t src = (uint8_t)(instruction.opcode & 0x07u);
        uint8_t value;

        if (src == 6u) {
            status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, emu->cpu.hl), &value);
            if (status != EZ80_OK) return status;
        } else {
            value = cpu_get_reg8(&emu->cpu, src);
        }

        if (dst == 6u) {
            status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, emu->cpu.hl), value);
            if (status != EZ80_OK) return status;
        } else {
            cpu_set_reg8(&emu->cpu, dst, value);
        }

        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    if ((instruction.opcode == 0x06u) || (instruction.opcode == 0x0Eu) || (instruction.opcode == 0x16u) ||
        (instruction.opcode == 0x1Eu) || (instruction.opcode == 0x26u) || (instruction.opcode == 0x2Eu) ||
        (instruction.opcode == 0x36u)) {
        uint8_t dst = (uint8_t)((instruction.opcode >> 3) & 0x07u);
        uint8_t value = instruction.bytes[1];
        if (dst == 6u) {
            status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, emu->cpu.hl), value);
            if (status != EZ80_OK) return status;
        } else {
            cpu_set_reg8(&emu->cpu, dst, value);
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    if ((instruction.opcode == 0x01u) || (instruction.opcode == 0x11u) ||
        (instruction.opcode == 0x21u) || (instruction.opcode == 0x31u)) {
        uint8_t pair = (uint8_t)((instruction.opcode >> 4) & 0x03u);
        uint32_t imm = read_imm_word(&instruction.bytes[1], cpu_effective_adl_encoding(&emu->cpu));
        if (cpu_effective_pair_adl(&emu->cpu)) {
            cpu_set_pair_width(&emu->cpu, pair, imm);
        } else {
            cpu_set_pair16(&emu->cpu, pair, (uint16_t)imm);
        }
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    if ((instruction.opcode & 0xC7u) == 0x04u) {
        uint8_t dst = (uint8_t)((instruction.opcode >> 3) & 0x07u);
        uint8_t before;
        uint8_t after;
        status = cpu_read_reg_or_hl(emu, dst, &before);
        if (status != EZ80_OK) return status;
        after = (uint8_t)(before + 1u);
        cpu_flags_inc8(&emu->cpu, before, after);
        status = cpu_write_reg_or_hl(emu, dst, after);
        if (status != EZ80_OK) return status;
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    if ((instruction.opcode & 0xC7u) == 0x05u) {
        uint8_t dst = (uint8_t)((instruction.opcode >> 3) & 0x07u);
        uint8_t before;
        uint8_t after;
        status = cpu_read_reg_or_hl(emu, dst, &before);
        if (status != EZ80_OK) return status;
        after = (uint8_t)(before - 1u);
        cpu_flags_dec8(&emu->cpu, before, after);
        status = cpu_write_reg_or_hl(emu, dst, after);
        if (status != EZ80_OK) return status;
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    if (instruction.opcode >= 0x80u && instruction.opcode <= 0xBFu) {
        uint8_t group = (uint8_t)((instruction.opcode >> 3) & 0x07u);
        uint8_t src = (uint8_t)(instruction.opcode & 0x07u);
        uint8_t rhs = 0u;
        status = cpu_read_reg_or_hl(emu, src, &rhs);
        if (status != EZ80_OK) return status;
        cpu_execute_alu_a(&emu->cpu, group, rhs);
        emu->cpu.pc = next_pc;
        return EZ80_OK;
    }

    switch (instruction.opcode) {
    case 0x00:
        emu->cpu.pc = next_pc;
        break;

    case 0x08:
        cpu_swap_pair16(&emu->cpu.af, &emu->cpu.af_alt);
        emu->cpu.pc = next_pc;
        break;

    case 0x10: {
        uint8_t b = (uint8_t)(cpu_get_reg8(&emu->cpu, 0u) - 1u);
        cpu_set_reg8(&emu->cpu, 0u, b);
        if (b != 0u) {
            int32_t rel = (int32_t)(int8_t)instruction.bytes[1];
            int32_t target = (int32_t)next_pc + rel;
            emu->cpu.pc = mask24((uint32_t)target);
        } else {
            emu->cpu.pc = next_pc;
        }
        break;
    }

    case 0x07: {
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        bool carry = (a & 0x80u) != 0u;
        uint8_t result = (uint8_t)((a << 1) | (carry ? 1u : 0u));
        cpu_set_reg_a(&emu->cpu, result);
        cpu_flags_rotate_acc(&emu->cpu, carry);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x0F: {
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        bool carry = (a & 0x01u) != 0u;
        uint8_t result = (uint8_t)((a >> 1) | (carry ? 0x80u : 0u));
        cpu_set_reg_a(&emu->cpu, result);
        cpu_flags_rotate_acc(&emu->cpu, carry);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x17: {
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t old_carry = (cpu_get_reg_f(&emu->cpu) & FLAG_C) ? 1u : 0u;
        bool carry = (a & 0x80u) != 0u;
        uint8_t result = (uint8_t)((a << 1) | old_carry);
        cpu_set_reg_a(&emu->cpu, result);
        cpu_flags_rotate_acc(&emu->cpu, carry);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x1F: {
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t old_carry = (cpu_get_reg_f(&emu->cpu) & FLAG_C) ? 1u : 0u;
        bool carry = (a & 0x01u) != 0u;
        uint8_t result = (uint8_t)((a >> 1) | (uint8_t)(old_carry << 7));
        cpu_set_reg_a(&emu->cpu, result);
        cpu_flags_rotate_acc(&emu->cpu, carry);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x27:
        cpu_execute_daa(&emu->cpu);
        emu->cpu.pc = next_pc;
        break;

    case 0x2F: {
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t flags = (uint8_t)(cpu_get_reg_f(&emu->cpu) & (FLAG_S | FLAG_Z | FLAG_PV | FLAG_C));
        cpu_set_reg_a(&emu->cpu, (uint8_t)~a);
        flags |= (FLAG_H | FLAG_N);
        cpu_set_reg_f(&emu->cpu, flags);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x37: {
        uint8_t flags = (uint8_t)(cpu_get_reg_f(&emu->cpu) & (FLAG_S | FLAG_Z | FLAG_PV));
        flags |= FLAG_C;
        cpu_set_reg_f(&emu->cpu, flags);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x3F: {
        uint8_t old_flags = cpu_get_reg_f(&emu->cpu);
        uint8_t flags = (uint8_t)(old_flags & (FLAG_S | FLAG_Z | FLAG_PV));
        if ((old_flags & FLAG_C) != 0u) {
            flags |= FLAG_H;
        } else {
            flags |= FLAG_C;
        }
        cpu_set_reg_f(&emu->cpu, flags);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x76:
        emu->cpu.halted = true;
        emu->hw.halt_pc_advanced = true;
        emu->cpu.pc = next_pc;
        break;

    case 0x3E:
        cpu_set_reg_a(&emu->cpu, instruction.bytes[1]);
        emu->cpu.pc = next_pc;
        break;

    case 0x3C: {
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t result = (uint8_t)(a + 1u);
        cpu_set_reg_a(&emu->cpu, result);
        cpu_flags_inc8(&emu->cpu, a, result);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x3D: {
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t result = (uint8_t)(a - 1u);
        cpu_set_reg_a(&emu->cpu, result);
        cpu_flags_dec8(&emu->cpu, a, result);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0xAF:
        cpu_set_reg_a(&emu->cpu, 0u);
        cpu_set_reg_f(&emu->cpu, (uint8_t)(FLAG_Z | FLAG_PV));
        emu->cpu.pc = next_pc;
        break;

    case 0xC6: {
        uint8_t a = cpu_get_reg_a(&emu->cpu);
        uint8_t imm = instruction.bytes[1];
        uint8_t result = (uint8_t)(a + imm);
        cpu_set_reg_a(&emu->cpu, result);
        cpu_flags_add8(&emu->cpu, a, imm, 0u, result);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0xCE:
    case 0xD6:
    case 0xDE:
    case 0xE6:
    case 0xEE:
    case 0xF6:
    case 0xFE: {
        uint8_t group;
        switch (instruction.opcode) {
        case 0xCE: group = 1u; break;
        case 0xD6: group = 2u; break;
        case 0xDE: group = 3u; break;
        case 0xE6: group = 4u; break;
        case 0xEE: group = 5u; break;
        case 0xF6: group = 6u; break;
        default: group = 7u; break;
        }
        cpu_execute_alu_a(&emu->cpu, group, instruction.bytes[1]);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x18: {
        int32_t rel = (int32_t)(int8_t)instruction.bytes[1];
        int32_t target = (int32_t)next_pc + rel;
        emu->cpu.pc = mask24((uint32_t)target);
        break;
    }

    case 0x20:
    case 0x28:
    case 0x30:
    case 0x38: {
        uint8_t cond = (uint8_t)((instruction.opcode >> 3) & 0x03u);
        if (cpu_condition_true(&emu->cpu, cond)) {
            int32_t rel = (int32_t)(int8_t)instruction.bytes[1];
            int32_t target = (int32_t)next_pc + rel;
            emu->cpu.pc = mask24((uint32_t)target);
        } else {
            emu->cpu.pc = next_pc;
        }
        break;
    }

    case 0xC3: {
        uint32_t target = read_imm_word(&instruction.bytes[1], cpu_effective_adl_encoding(&emu->cpu));
        if (cpu_effective_adl_mode(&emu->cpu)) {
            emu->cpu.pc = mask24(target);
        } else {
            emu->cpu.pc = map_code_addr16_cpu(&emu->cpu, pc, (uint16_t)target);
        }
        cpu_commit_instruction_modes(&emu->cpu);
        break;
    }

    case 0xC2:
    case 0xCA:
    case 0xD2:
    case 0xDA:
    case 0xE2:
    case 0xEA:
    case 0xF2:
    case 0xFA: {
        uint8_t cond = (uint8_t)((instruction.opcode >> 3) & 0x07u);
        if (cpu_condition_true(&emu->cpu, cond)) {
            uint32_t target = read_imm_word(&instruction.bytes[1], cpu_effective_adl_encoding(&emu->cpu));
            if (cpu_effective_adl_mode(&emu->cpu)) {
                emu->cpu.pc = mask24(target);
            } else {
                emu->cpu.pc = map_code_addr16_cpu(&emu->cpu, pc, (uint16_t)target);
            }
            cpu_commit_instruction_modes(&emu->cpu);
        } else {
            emu->cpu.pc = next_pc;
        }
        break;
    }

    case 0xE9:
        if (cpu_effective_adl_mode(&emu->cpu)) {
            emu->cpu.pc = mask24(emu->cpu.hl);
        } else {
            emu->cpu.pc = z80_mbase_addr(emu->cpu.mbase_reg, cpu_get_pair16(&emu->cpu, 2u));
        }
        cpu_commit_instruction_modes(&emu->cpu);
        break;

    case 0xCD: {
        uint32_t target = read_imm_word(&instruction.bytes[1], cpu_effective_adl_encoding(&emu->cpu));
        status = stack_push_pc(emu, next_pc);
        if (status != EZ80_OK) return status;
        if (cpu_effective_adl_mode(&emu->cpu)) {
            emu->cpu.pc = mask24(target);
        } else {
            emu->cpu.pc = map_code_addr16_cpu(&emu->cpu, pc, (uint16_t)target);
        }
        cpu_commit_instruction_modes(&emu->cpu);
        break;
    }

    case 0xC4:
    case 0xCC:
    case 0xD4:
    case 0xDC:
    case 0xE4:
    case 0xEC:
    case 0xF4:
    case 0xFC: {
        uint8_t cond = (uint8_t)((instruction.opcode >> 3) & 0x07u);
        if (cpu_condition_true(&emu->cpu, cond)) {
            uint32_t target = read_imm_word(&instruction.bytes[1], cpu_effective_adl_encoding(&emu->cpu));
            status = stack_push_pc(emu, next_pc);
            if (status != EZ80_OK) return status;
            if (cpu_effective_adl_mode(&emu->cpu)) {
                emu->cpu.pc = mask24(target);
            } else {
                emu->cpu.pc = map_code_addr16_cpu(&emu->cpu, pc, (uint16_t)target);
            }
            cpu_commit_instruction_modes(&emu->cpu);
        } else {
            emu->cpu.pc = next_pc;
        }
        break;
    }

    case 0xC9: {
        uint32_t target = 0u;
        status = stack_pop_pc(emu, &target);
        if (status != EZ80_OK) return status;
        if (cpu_effective_adl_mode(&emu->cpu)) {
            emu->cpu.pc = mask24(target);
        } else {
            emu->cpu.pc = map_code_addr16_cpu(&emu->cpu, pc, (uint16_t)target);
        }
        cpu_commit_instruction_modes(&emu->cpu);
        break;
    }

    case 0xC0:
    case 0xC8:
    case 0xD0:
    case 0xD8:
    case 0xE0:
    case 0xE8:
    case 0xF0:
    case 0xF8: {
        uint8_t cond = (uint8_t)((instruction.opcode >> 3) & 0x07u);
        if (cpu_condition_true(&emu->cpu, cond)) {
            uint32_t target = 0u;
            status = stack_pop_pc(emu, &target);
            if (status != EZ80_OK) return status;
            if (cpu_effective_adl_mode(&emu->cpu)) {
                emu->cpu.pc = mask24(target);
            } else {
                emu->cpu.pc = map_code_addr16_cpu(&emu->cpu, pc, (uint16_t)target);
            }
            cpu_commit_instruction_modes(&emu->cpu);
        } else {
            emu->cpu.pc = next_pc;
        }
        break;
    }

    case 0xC5:
    case 0xD5:
    case 0xE5:
    case 0xF5: {
        uint8_t qq = (uint8_t)((instruction.opcode >> 4) & 0x03u);
        status = stack_push_qq(emu, qq);
        if (status != EZ80_OK) return status;
        emu->cpu.pc = next_pc;
        break;
    }

    case 0xC1:
    case 0xD1:
    case 0xE1:
    case 0xF1: {
        uint8_t qq = (uint8_t)((instruction.opcode >> 4) & 0x03u);
        status = stack_pop_qq(emu, qq);
        if (status != EZ80_OK) return status;
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x09:
    case 0x19:
    case 0x29:
    case 0x39: {
        uint8_t ss = (uint8_t)((instruction.opcode >> 4) & 0x03u);
        if (cpu_effective_pair_adl(&emu->cpu)) {
            uint32_t hl24 = cpu_get_pair_width(&emu->cpu, 2u);
            uint32_t val24 = cpu_get_pair_width(&emu->cpu, ss);
            uint32_t sum = hl24 + val24;
            cpu_set_pair_width(&emu->cpu, 2u, sum);
            cpu_flags_add_hl_ss(&emu->cpu, (uint16_t)hl24, (uint16_t)val24, sum);
        } else {
            uint16_t hl = cpu_get_pair16(&emu->cpu, 2u);
            uint16_t value = cpu_get_pair16(&emu->cpu, ss);
            uint32_t sum = (uint32_t)hl + (uint32_t)value;
            cpu_set_pair16(&emu->cpu, 2u, (uint16_t)sum);
            cpu_flags_add_hl_ss(&emu->cpu, hl, value, sum);
        }
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x03:
    case 0x13:
    case 0x23:
    case 0x33: {
        uint8_t ss = (uint8_t)((instruction.opcode >> 4) & 0x03u);
        if (cpu_effective_pair_adl(&emu->cpu)) {
            uint32_t value = cpu_get_pair_width(&emu->cpu, ss);
            cpu_set_pair_width(&emu->cpu, ss, cpu_increment_width(&emu->cpu, value));
        } else {
            uint16_t value = (uint16_t)(cpu_get_pair16(&emu->cpu, ss) + 1u);
            cpu_set_pair16(&emu->cpu, ss, value);
        }
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x0B:
    case 0x1B:
    case 0x2B:
    case 0x3B: {
        uint8_t ss = (uint8_t)((instruction.opcode >> 4) & 0x03u);
        if (cpu_effective_pair_adl(&emu->cpu)) {
            uint32_t value = cpu_get_pair_width(&emu->cpu, ss);
            cpu_set_pair_width(&emu->cpu, ss, cpu_decrement_width(&emu->cpu, value));
        } else {
            uint16_t value = (uint16_t)(cpu_get_pair16(&emu->cpu, ss) - 1u);
            cpu_set_pair16(&emu->cpu, ss, value);
        }
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x02:
        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, emu->cpu.bc), cpu_get_reg_a(&emu->cpu));
        if (status != EZ80_OK) return status;
        emu->cpu.pc = next_pc;
        break;

    case 0x12:
        status = ez80_memory_write8(emu, map_data_addr_cpu(&emu->cpu, emu->cpu.de), cpu_get_reg_a(&emu->cpu));
        if (status != EZ80_OK) return status;
        emu->cpu.pc = next_pc;
        break;

    case 0x0A: {
        uint8_t value;
        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, emu->cpu.bc), &value);
        if (status != EZ80_OK) return status;
        cpu_set_reg_a(&emu->cpu, value);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x1A: {
        uint8_t value;
        status = ez80_memory_read8(emu, map_data_addr_cpu(&emu->cpu, emu->cpu.de), &value);
        if (status != EZ80_OK) return status;
        cpu_set_reg_a(&emu->cpu, value);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x32: {
        uint32_t addr = read_imm_word(&instruction.bytes[1], cpu_effective_adl_encoding(&emu->cpu));
        if (cpu_effective_adl_encoding(&emu->cpu)) {
            addr = mask24(addr);
        } else {
            addr = map_data_addr_cpu(&emu->cpu, addr);
        }
        status = ez80_memory_write8(emu, addr, cpu_get_reg_a(&emu->cpu));
        if (status != EZ80_OK) return status;
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x3A: {
        uint8_t value;
        uint32_t addr = read_imm_word(&instruction.bytes[1], cpu_effective_adl_encoding(&emu->cpu));
        if (cpu_effective_adl_encoding(&emu->cpu)) {
            addr = mask24(addr);
        } else {
            addr = map_data_addr_cpu(&emu->cpu, addr);
        }
        status = ez80_memory_read8(emu, addr, &value);
        if (status != EZ80_OK) return status;
        cpu_set_reg_a(&emu->cpu, value);
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x22: {
        if (cpu_effective_pair_adl(&emu->cpu)) {
            bool adl_encoding = cpu_effective_adl_encoding(&emu->cpu);
            uint32_t hl24 = mask24(emu->cpu.hl);
            uint32_t addr0 = adl_encoding
                             ? mask24(read_u24(&instruction.bytes[1]))
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction.bytes[1]), 0u);
            uint32_t addr1 = adl_encoding
                             ? mask24(addr0 + 1u)
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction.bytes[1]), 1u);
            uint32_t addr2 = adl_encoding
                             ? mask24(addr0 + 2u)
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction.bytes[1]), 2u);
            status = ez80_memory_write8(emu, addr0, (uint8_t)(hl24 & 0xFFu));
            if (status != EZ80_OK) return status;
            status = ez80_memory_write8(emu, addr1, (uint8_t)((hl24 >> 8) & 0xFFu));
            if (status != EZ80_OK) return status;
            status = ez80_memory_write8(emu, addr2, (uint8_t)((hl24 >> 16) & 0xFFu));
            if (status != EZ80_OK) return status;
        } else {
            uint16_t addr16 = read_u16(&instruction.bytes[1]);
            uint32_t addr0 = map_data_addr16_offset_cpu(&emu->cpu, addr16, 0u);
            uint32_t addr1 = map_data_addr16_offset_cpu(&emu->cpu, addr16, 1u);
            uint16_t hl = cpu_get_pair16(&emu->cpu, 2u);
            status = ez80_memory_write8(emu, addr0, (uint8_t)(hl & 0xFFu));
            if (status != EZ80_OK) return status;
            status = ez80_memory_write8(emu, addr1, (uint8_t)((hl >> 8) & 0xFFu));
            if (status != EZ80_OK) return status;
        }
        emu->cpu.pc = next_pc;
        break;
    }

    case 0x2A: {
        if (cpu_effective_pair_adl(&emu->cpu)) {
            bool adl_encoding = cpu_effective_adl_encoding(&emu->cpu);
            uint8_t b0 = 0u;
            uint8_t b1 = 0u;
            uint8_t b2 = 0u;
            uint32_t addr0 = adl_encoding
                             ? mask24(read_u24(&instruction.bytes[1]))
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction.bytes[1]), 0u);
            uint32_t addr1 = adl_encoding
                             ? mask24(addr0 + 1u)
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction.bytes[1]), 1u);
            uint32_t addr2 = adl_encoding
                             ? mask24(addr0 + 2u)
                             : map_data_addr16_offset_cpu(&emu->cpu, read_u16(&instruction.bytes[1]), 2u);
            status = ez80_memory_read8(emu, addr0, &b0);
            if (status != EZ80_OK) return status;
            status = ez80_memory_read8(emu, addr1, &b1);
            if (status != EZ80_OK) return status;
            status = ez80_memory_read8(emu, addr2, &b2);
            if (status != EZ80_OK) return status;
            emu->cpu.hl = mask24((uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16));
        } else {
            uint8_t lo;
            uint8_t hi;
            uint16_t addr16 = read_u16(&instruction.bytes[1]);
            uint32_t addr0 = map_data_addr16_offset_cpu(&emu->cpu, addr16, 0u);
            uint32_t addr1 = map_data_addr16_offset_cpu(&emu->cpu, addr16, 1u);
            status = ez80_memory_read8(emu, addr0, &lo);
            if (status != EZ80_OK) return status;
            status = ez80_memory_read8(emu, addr1, &hi);
            if (status != EZ80_OK) return status;
            cpu_set_pair16(&emu->cpu, 2u, (uint16_t)lo | ((uint16_t)hi << 8));
        }
        emu->cpu.pc = next_pc;
        break;
    }

    case 0xD3: {
        uint16_t port = (uint16_t)instruction.bytes[1];
        uint8_t value = cpu_get_reg_a(&emu->cpu);
        port_write(emu, port, value);
        io_record_last_out(emu, port, value);
        if ((port & 0x00FFu) == 0x01u) {
            mmio_set(emu, EZ80_MMIO_REG_KEY_STATUS, value);
        }
        emu->cpu.pc = next_pc;
        break;
    }

    case 0xDB: {
        uint16_t port = (uint16_t)instruction.bytes[1];
        uint8_t value = port_read(emu, port);
        cpu_set_reg_a(&emu->cpu, value);
        io_record_last_in(emu, port, value);
        if ((port & 0x00FFu) == 0x02u) {
            mmio_set(emu, EZ80_MMIO_REG_LCD_STATUS, value);
        }
        emu->cpu.pc = next_pc;
        break;
    }

    case 0xD9:
        cpu_swap_pair_multibyte(&emu->cpu.bc, &emu->cpu.bc_alt, cpu_effective_pair_adl(&emu->cpu));
        cpu_swap_pair_multibyte(&emu->cpu.de, &emu->cpu.de_alt, cpu_effective_pair_adl(&emu->cpu));
        cpu_swap_pair_multibyte(&emu->cpu.hl, &emu->cpu.hl_alt, cpu_effective_pair_adl(&emu->cpu));
        emu->cpu.pc = next_pc;
        break;

    case 0xE3:
        status = cpu_exchange_pair16_with_stack(emu, &emu->cpu.hl);
        if (status != EZ80_OK) return status;
        emu->cpu.pc = next_pc;
        break;

    case 0xEB:
        cpu_swap_pair_multibyte(&emu->cpu.de, &emu->cpu.hl, cpu_effective_pair_adl(&emu->cpu));
        emu->cpu.pc = next_pc;
        break;

    case 0xF9:
        if (cpu_effective_pair_adl(&emu->cpu)) {
            emu->cpu.sp = mask24(emu->cpu.hl);
        } else {
            emu->cpu.sp = map_data_addr_cpu(&emu->cpu, emu->cpu.hl);
        }
        emu->cpu.pc = next_pc;
        break;

    case 0xC7:
    case 0xCF:
    case 0xD7:
    case 0xDF:
    case 0xE7:
    case 0xEF:
    case 0xF7:
    case 0xFF: {
        uint8_t vector = (uint8_t)(instruction.opcode & 0x38u);
        status = stack_push_pc(emu, next_pc);
        if (status != EZ80_OK) return status;
        emu->cpu.pc = map_code_addr16_cpu(&emu->cpu, pc, (uint16_t)vector);
        cpu_commit_instruction_modes(&emu->cpu);
        break;
    }

    case 0xF3:
        emu->cpu.iff1 = false;
        emu->cpu.iff2 = false;
        emu->cpu.pc = next_pc;
        break;

    case 0xFB:
        emu->cpu.iff1 = true;
        emu->cpu.iff2 = true;
        emu->cpu.ei_pending = true;
        emu->cpu.pc = next_pc;
        break;

    case 0xCB:
        return execute_cb_prefix(emu, &instruction, next_pc);

    case 0xED:
        return execute_ed_prefix(emu, &instruction, pc, next_pc);

    case 0xDD:
        return execute_ddfd_prefix(emu, &instruction, next_pc, false);

    case 0xFD:
        return execute_ddfd_prefix(emu, &instruction, next_pc, true);

    default:
        return EZ80_ERR_NOT_IMPLEMENTED;
    }

    return EZ80_OK;
}

ez80_status_t ez80_emulator_step(ez80_emulator_t *emu, ez80_instruction_t *decoded)
{
    ez80_status_t status;
    if (emu) {
        emu->cpu_instruction_port_access_active = true;
        emu->cpu_instruction_port_access_pc = mask24(emu->cpu.pc);
    }
    status = ez80_emulator_step_impl(emu, decoded);
    if (emu) {
        emu->cpu_instruction_port_access_active = false;
        emu->cpu_instruction_port_access_pc = 0u;
        cpu_clear_instruction_mode_override(&emu->cpu);
    }
    return status;
}

static uint8_t runtime_effective_cycles(const ez80_instruction_t *decoded,
                                        const ez80_cpu_state_t *cpu_before,
                                        uint32_t pc_before,
                                        uint32_t pc_after)
{
    uint8_t cycles;
    uint32_t next_pc;

    if (!decoded) return 1u;
    cycles = decoded->cycles ? decoded->cycles : 1u;
    next_pc = mask24(pc_before + decoded->length);

    if (decoded->opcode == 0x10u) {
        if (cpu_before) {
            uint8_t b_before = (uint8_t)((cpu_before->bc >> 8) & 0xFFu);
            return ((uint8_t)(b_before - 1u) != 0u) ? 13u : 8u;
        }
        return (pc_after == next_pc) ? 8u : 13u;
    }

    if (decoded->opcode == 0x20u || decoded->opcode == 0x28u ||
        decoded->opcode == 0x30u || decoded->opcode == 0x38u) {
        if (cpu_before) {
            uint8_t cond = (uint8_t)((decoded->opcode >> 3) & 0x03u);
            return cpu_condition_true(cpu_before, cond) ? 12u : 7u;
        }
        return (pc_after == next_pc) ? 7u : 12u;
    }

    if ((decoded->opcode & 0xC7u) == 0xC4u) {
        if (cpu_before) {
            uint8_t cond = (uint8_t)((decoded->opcode >> 3) & 0x07u);
            return cpu_condition_true(cpu_before, cond) ? 17u : 10u;
        }
        return (pc_after == next_pc) ? 10u : 17u;
    }

    if ((decoded->opcode & 0xC7u) == 0xC0u) {
        if (cpu_before) {
            uint8_t cond = (uint8_t)((decoded->opcode >> 3) & 0x07u);
            return cpu_condition_true(cpu_before, cond) ? 11u : 5u;
        }
        return (pc_after == next_pc) ? 5u : 11u;
    }

    if (decoded->opcode == 0xDDu || decoded->opcode == 0xFDu) {
        uint8_t inner_op = decoded->bytes[1];
        uint32_t inner_pc = mask24(pc_before + 1u);
        uint32_t inner_next_pc = next_pc;

        if (inner_op == 0x10u) {
            if (cpu_before) {
                uint8_t b_before = (uint8_t)((cpu_before->bc >> 8) & 0xFFu);
                return ((uint8_t)(b_before - 1u) != 0u) ? 17u : 12u;
            }
            return (pc_after == inner_next_pc) ? 12u : 17u;
        }

        if (inner_op == 0x20u || inner_op == 0x28u ||
            inner_op == 0x30u || inner_op == 0x38u) {
            if (cpu_before) {
                uint8_t cond = (uint8_t)((inner_op >> 3) & 0x03u);
                return cpu_condition_true(cpu_before, cond) ? 16u : 11u;
            }
            return (pc_after == inner_next_pc) ? 11u : 16u;
        }

        if ((inner_op & 0xC7u) == 0xC4u) {
            if (cpu_before) {
                uint8_t cond = (uint8_t)((inner_op >> 3) & 0x07u);
                return cpu_condition_true(cpu_before, cond) ? 21u : 14u;
            }
            return (pc_after == inner_next_pc) ? 14u : 21u;
        }

        if ((inner_op & 0xC7u) == 0xC0u) {
            if (cpu_before) {
                uint8_t cond = (uint8_t)((inner_op >> 3) & 0x07u);
                return cpu_condition_true(cpu_before, cond) ? 15u : 9u;
            }
            return (pc_after == inner_next_pc) ? 9u : 15u;
        }

        if (inner_op == 0xEDu && decoded->length >= 3u) {
            switch (decoded->bytes[2]) {
            case 0xB0u:
            case 0xB1u:
            case 0xB2u:
            case 0xB3u:
            case 0xB8u:
            case 0xB9u:
            case 0xBAu:
            case 0xBBu:
            case 0x92u:
            case 0x93u:
            case 0x94u:
            case 0x95u:
            case 0x90u:
            case 0x91u:
            case 0x96u:
            case 0x97u:
            case 0xB5u:
            case 0xB6u:
            case 0xB7u:
            case 0x9Au:
            case 0x9Bu:
            case 0x9Cu:
            case 0x9Du:
            case 0x98u:
            case 0x99u:
            case 0x9Eu:
            case 0x9Fu:
            case 0xBDu:
            case 0xBEu:
            case 0xBFu:
            case 0xB4u:
            case 0xBCu:
            case 0xC2u:
            case 0xC3u:
            case 0xCAu:
            case 0xCBu:
                return (pc_after == inner_pc) ? 25u : 20u;
            default:
                break;
            }
        }
    }

    if (decoded->opcode == 0xEDu) {
        switch (decoded->bytes[1]) {
        case 0xB0u:
        case 0xB1u:
        case 0xB2u:
        case 0xB3u:
        case 0xB8u:
        case 0xB9u:
        case 0xBAu:
        case 0xBBu:
        case 0x92u:
        case 0x93u:
        case 0x94u:
        case 0x95u:
        case 0x90u:
        case 0x91u:
        case 0x96u:
        case 0x97u:
        case 0xB5u:
        case 0xB6u:
        case 0xB7u:
        case 0x9Au:
        case 0x9Bu:
        case 0x9Cu:
        case 0x9Du:
        case 0x98u:
        case 0x99u:
        case 0x9Eu:
        case 0x9Fu:
        case 0xBDu:
        case 0xBEu:
        case 0xBFu:
        case 0xB4u:
        case 0xBCu:
        case 0xC2u:
        case 0xC3u:
        case 0xCAu:
        case 0xCBu:
            return (pc_after == pc_before) ? 21u : 16u;
        default:
            break;
        }
    }

    return cycles;
}

/* Get the IM 2 vector offset for a given IRQ source bit */
/* ---- DMA transfer engine ---- */
static void dma_execute_channel(ez80_emulator_t *emu, uint8_t ch)
{
    ez80_dma_channel_t *dma;
    uint32_t i;
    uint8_t byte_val;
    uint16_t word_val;
    bool src_inc, dst_inc, word_mode;
    uint32_t src, dst;
    ez80_status_t status;

    if (!emu || ch >= EZ80_DMA_CHANNEL_COUNT) return;
    dma = &emu->hw.dma[ch];

    if ((dma->control & EZ80_DMA_CONTROL_ENABLE) == 0u) return;
    if (dma->length == 0u) {
        dma->status = EZ80_DMA_STATUS_DONE;
        return;
    }
    if (dma->length > EZ80_DMA_MAX_TRANSFER_BYTES) {
        dma->status = EZ80_DMA_STATUS_ERROR;
        return;
    }

    dma->status = EZ80_DMA_STATUS_ACTIVE;
    src_inc = (dma->control & EZ80_DMA_CONTROL_SRC_INC) != 0u;
    dst_inc = (dma->control & EZ80_DMA_CONTROL_DST_INC) != 0u;
    word_mode = (dma->control & EZ80_DMA_CONTROL_WORD_MODE) != 0u;
    src = dma->src_addr;
    dst = dma->dst_addr;
    dma->bytes_transferred = 0u;

    if (word_mode) {
        uint32_t word_count = dma->length / 2u;
        for (i = 0u; i < word_count; i++) {
            uint8_t lo = 0u, hi = 0u;
            status = ez80_memory_read8(emu, mask24(src), &lo);
            if (status != EZ80_OK) { dma->status = EZ80_DMA_STATUS_ERROR; return; }
            status = ez80_memory_read8(emu, mask24(src + 1u), &hi);
            if (status != EZ80_OK) { dma->status = EZ80_DMA_STATUS_ERROR; return; }
            word_val = (uint16_t)lo | ((uint16_t)hi << 8);
            status = ez80_memory_write8(emu, mask24(dst), (uint8_t)(word_val & 0xFFu));
            if (status != EZ80_OK) { dma->status = EZ80_DMA_STATUS_ERROR; return; }
            status = ez80_memory_write8(emu, mask24(dst + 1u), (uint8_t)((word_val >> 8) & 0xFFu));
            if (status != EZ80_OK) { dma->status = EZ80_DMA_STATUS_ERROR; return; }
            if (src_inc) src = mask24(src + 2u);
            if (dst_inc) dst = mask24(dst + 2u);
            dma->bytes_transferred += 2u;
        }
    } else {
        for (i = 0u; i < dma->length; i++) {
            byte_val = 0u;
            status = ez80_memory_read8(emu, mask24(src), &byte_val);
            if (status != EZ80_OK) { dma->status = EZ80_DMA_STATUS_ERROR; return; }
            status = ez80_memory_write8(emu, mask24(dst), byte_val);
            if (status != EZ80_OK) { dma->status = EZ80_DMA_STATUS_ERROR; return; }
            if (src_inc) src = mask24(src + 1u);
            if (dst_inc) dst = mask24(dst + 1u);
            dma->bytes_transferred++;
        }
    }

    dma->status = EZ80_DMA_STATUS_DONE;
    emu->hw.dma_total_transfers++;

    if ((dma->control & EZ80_DMA_CONTROL_IRQ_ON_DONE) != 0u) {
        hardware_raise_irq(emu, EZ80_IRQ_SOURCE_DMA);
    }

    if ((dma->control & EZ80_DMA_CONTROL_CONTINUOUS) != 0u) {
        dma->src_addr = src;
        dma->dst_addr = dst;
    } else {
        dma->control &= (uint8_t)~(EZ80_DMA_CONTROL_START);
    }
}

/* ---- NMI dispatch ---- */
static uint32_t service_nmi(ez80_emulator_t *emu)
{
    ez80_status_t status;
    if (!emu) return 0u;
    if (!emu->hw.nmi_pending) return 0u;
    if (!emu->interrupts_enabled_in_emulator) return 0u;

    emu->hw.nmi_pending = false;
    emu->hw.nmi_active = true;
    if (emu->hw.nmi_nesting_depth == 0u) {
        emu->hw.nmi_iff1_saved = emu->cpu.iff1;
    }
    if (emu->hw.nmi_nesting_depth < 0xFFu) {
        emu->hw.nmi_nesting_depth++;
    }
    emu->hw.nmi_count++;
    pm_note_wake_event(emu, EZ80_PM_WAKE_NMI, false);

    /* NMI clears IFF1 (non-maskable). */
    emu->cpu.iff1 = false;

    /* Un-halt the CPU */
    if (emu->cpu.halted) {
        emu->cpu.halted = false;
        if (!emu->hw.halt_pc_advanced) {
            emu->cpu.pc = mask24(emu->cpu.pc + 1u);
        }
        emu->hw.halt_pc_advanced = false;
    }

    /* Push PC and jump to NMI vector 0x0066 */
    status = stack_push_pc(emu, emu->cpu.pc);
    if (status != EZ80_OK) return 0u;
    emu->cpu.pc = EZ80_NMI_VECTOR_ADDR;

    return EZ80_NMI_DISPATCH_CYCLES;
}

/* ---- RETI/RETN notification ---- */
static void notify_reti(ez80_emulator_t *emu)
{
    if (!emu) return;
    emu->hw.total_reti_count++;
    if (emu->hw.irq_nesting_depth > 0u) {
        emu->hw.last_reti_source = emu->hw.irq_nesting_stack[emu->hw.irq_nesting_depth - 1u];
        emu->hw.irq_nesting_depth--;
    } else {
        emu->hw.last_reti_source = 0u;
    }
    hardware_sync_mmio(emu);
}

static void notify_retn(ez80_emulator_t *emu)
{
    uint8_t effective_depth;

    if (!emu) return;
    /* RETN restores IFF1 from IFF2 (done by caller).
     * Only the outermost RETN restores the pre-NMI IFF1 value. */
    effective_depth = emu->hw.nmi_nesting_depth;
    if (effective_depth == 0u) {
        if (!emu->hw.nmi_active) {
            hardware_sync_mmio(emu);
            return;
        }
        effective_depth = 1u;
    }

    effective_depth--;
    emu->hw.nmi_nesting_depth = effective_depth;
    if (effective_depth == 0u) {
        emu->hw.nmi_active = false;
        emu->cpu.iff1 = emu->hw.nmi_iff1_saved;
    } else {
        emu->hw.nmi_active = true;
        emu->cpu.iff1 = false;
    }
    hardware_sync_mmio(emu);
}

static uint8_t irq_source_vector_offset(uint8_t source)
{
    switch (source) {
    case EZ80_IRQ_SOURCE_KEYPAD: return EZ80_IRQ_VECTOR_OFFSET_KEYPAD;
    case EZ80_IRQ_SOURCE_TIMER0: return EZ80_IRQ_VECTOR_OFFSET_TIMER0;
    case EZ80_IRQ_SOURCE_TIMER1: return EZ80_IRQ_VECTOR_OFFSET_TIMER1;
    case EZ80_IRQ_SOURCE_USB:    return EZ80_IRQ_VECTOR_OFFSET_USB;
    case EZ80_IRQ_SOURCE_DMA:    return EZ80_IRQ_VECTOR_OFFSET_DMA;
    case EZ80_IRQ_SOURCE_SPI:    return EZ80_IRQ_VECTOR_OFFSET_SPI;
    case EZ80_IRQ_SOURCE_I2C:    return EZ80_IRQ_VECTOR_OFFSET_I2C;
    case EZ80_IRQ_SOURCE_UART:   return EZ80_IRQ_VECTOR_OFFSET_UART;
    default: return 0u;
    }
}

/* Service pending interrupts if IFF1 is set and there are unmasked pending
 * interrupts.  Returns the number of cycles consumed by interrupt dispatch
 * (0 if no interrupt was serviced). */
static uint32_t service_pending_interrupts(ez80_emulator_t *emu)
{
    uint8_t serviceable;
    uint8_t source;
    uint8_t vec_offset;
    uint32_t vector_addr;
    uint8_t vec_lo, vec_hi, vec_hi2;
    uint32_t isr_addr;
    ez80_status_t status;

    if (!emu) return 0u;
    if (!emu->interrupts_enabled_in_emulator) return 0u;
    if (!emu->cpu.iff1) return 0u;

    serviceable = emu->hw.irq_pending & emu->hw.irq_mask;
    if (serviceable == 0u) return 0u;

    /* Find highest-priority pending interrupt (lowest bit) */
    if (serviceable & EZ80_IRQ_SOURCE_KEYPAD) {
        source = EZ80_IRQ_SOURCE_KEYPAD;
    } else if (serviceable & EZ80_IRQ_SOURCE_TIMER0) {
        source = EZ80_IRQ_SOURCE_TIMER0;
    } else if (serviceable & EZ80_IRQ_SOURCE_TIMER1) {
        source = EZ80_IRQ_SOURCE_TIMER1;
    } else if (serviceable & EZ80_IRQ_SOURCE_USB) {
        source = EZ80_IRQ_SOURCE_USB;
    } else if (serviceable & EZ80_IRQ_SOURCE_DMA) {
        source = EZ80_IRQ_SOURCE_DMA;
    } else if (serviceable & EZ80_IRQ_SOURCE_SPI) {
        source = EZ80_IRQ_SOURCE_SPI;
    } else if (serviceable & EZ80_IRQ_SOURCE_I2C) {
        source = EZ80_IRQ_SOURCE_I2C;
    } else if (serviceable & EZ80_IRQ_SOURCE_UART) {
        source = EZ80_IRQ_SOURCE_UART;
    } else {
        return 0u;
    }

    /* Acknowledge the interrupt */
    emu->hw.irq_pending &= (uint8_t)(~source);
    pm_note_wake_event(emu, pm_wake_source_for_irq_source(source), false);

    /* Disable interrupts during dispatch */
    emu->cpu.iff1 = false;
    emu->cpu.iff2 = false;

    /* Un-halt the CPU */
    if (emu->cpu.halted) {
        emu->cpu.halted = false;
        if (!emu->hw.halt_pc_advanced) {
            emu->cpu.pc = mask24(emu->cpu.pc + 1u);
        }
        emu->hw.halt_pc_advanced = false;
    }

    /* Record telemetry and push nesting stack */
    emu->hw.last_irq_source = source;
    emu->hw.total_interrupts_serviced++;
    if (emu->hw.irq_nesting_depth < EZ80_IRQ_MAX_NESTING) {
        emu->hw.irq_nesting_stack[emu->hw.irq_nesting_depth] = source;
        emu->hw.irq_nesting_depth++;
    }

    switch (emu->cpu.interrupt_mode) {
    case 0u:
        /* IM 0: execute instruction on data bus (RST 38h equivalent) */
        status = stack_push_pc(emu, emu->cpu.pc);
        if (status != EZ80_OK) return 0u;
        emu->cpu.pc = 0x0038u;
        emu->hw.last_irq_vector_addr = 0x0038u;
        return EZ80_IRQ_DISPATCH_CYCLES_IM1;

    case 1u:
        /* IM 1: always call RST 38h */
        status = stack_push_pc(emu, emu->cpu.pc);
        if (status != EZ80_OK) return 0u;
        emu->cpu.pc = 0x0038u;
        emu->hw.last_irq_vector_addr = 0x0038u;
        return EZ80_IRQ_DISPATCH_CYCLES_IM1;

    case 2u:
        /* IM 2: vectored interrupts. Vector table at I*256 (or I16-based in ADL).
         * Read 2 or 3 byte ISR address from table. */
        vec_offset = irq_source_vector_offset(source);

        if (emu->cpu.adl_mode) {
            /* ADL mode: I is 16-bit, vector table at I16 * 256 */
            vector_addr = ((uint32_t)emu->cpu.i_reg16 << 8) | (uint32_t)vec_offset;
        } else {
            /* Z80 mode: I is 8-bit, vector table at I * 256 */
            vector_addr = ((uint32_t)emu->cpu.i_reg << 8) | (uint32_t)vec_offset;
        }

        emu->hw.last_irq_vector_addr = mask24(vector_addr);

        /* Read ISR address from vector table */
        vec_lo = 0u;
        vec_hi = 0u;
        vec_hi2 = 0u;
        ez80_memory_read8(emu, mask24(vector_addr), &vec_lo);
        ez80_memory_read8(emu, mask24(vector_addr + 1u), &vec_hi);

        if (emu->cpu.adl_mode) {
            ez80_memory_read8(emu, mask24(vector_addr + 2u), &vec_hi2);
            isr_addr = (uint32_t)vec_lo | ((uint32_t)vec_hi << 8) | ((uint32_t)vec_hi2 << 16);
        } else {
            isr_addr = (uint32_t)vec_lo | ((uint32_t)vec_hi << 8);
        }

        status = stack_push_pc(emu, emu->cpu.pc);
        if (status != EZ80_OK) return 0u;
        emu->cpu.pc = mask24(isr_addr);
        return EZ80_IRQ_DISPATCH_CYCLES_IM2;

    default:
        return 0u;
    }
}

static bool check_breakpoint(const ez80_emulator_t *emu, uint32_t addr)
{
    uint32_t i;
    addr = mask24(addr);
    for (i = 0u; i < emu->breakpoint_count; i++) {
        if (emu->breakpoints[i] == addr) return true;
    }
    return false;
}

ez80_status_t ez80_emulator_run_cycles(ez80_emulator_t *emu,
                                       uint32_t cycle_budget,
                                       ez80_run_result_t *out)
{
    ez80_instruction_t decoded;
    ez80_cpu_state_t cpu_before;
    ez80_status_t status;
    uint32_t pc_before;
    uint32_t pc_after;
    uint8_t step_cycles;
    uint32_t irq_cycles;
    uint32_t idle_cycles;
    bool ei_was_pending = false;

    if (!emu || !out) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!emu->rom_loaded) return EZ80_ERR_ROM_FORMAT;

    memset(out, 0, sizeof(*out));
    if (cycle_budget == 0) return EZ80_OK;

    while (out->cycles_executed < cycle_budget) {
        /* Check watchpoint trigger from previous step */
        if (emu->watchpoint_count > 0u && emu->watchpoint_triggered) {
            emu->watchpoint_triggered = false;
            out->hit_watchpoint = true;
            out->watchpoint_addr = emu->watchpoint_hit_addr;
            out->watchpoint_type = emu->watchpoint_hit_type;
        }

        /* Check breakpoints before executing */
        if (emu->breakpoint_count > 0u &&
            check_breakpoint(emu, emu->cpu.pc)) {
            out->hit_breakpoint = true;
        }

        if (out->hit_watchpoint || out->hit_breakpoint) {
            break;
        }

        if (emu->cpu.halted) {
            flash_boot_maybe_raise_nmi(emu);
            /* Service NMI first (non-maskable, always checked) */
            irq_cycles = service_nmi(emu);
            if (irq_cycles > 0u) {
                out->cycles_executed += irq_cycles;
                hardware_advance_cycles(emu, irq_cycles);
                continue;
            }
            /* Service maskable interrupts even while halted */
            irq_cycles = service_pending_interrupts(emu);
            if (irq_cycles > 0u) {
                out->cycles_executed += irq_cycles;
                hardware_advance_cycles(emu, irq_cycles);
                continue;
            }
            if (boot_irq_overlay_release_halt(emu)) {
                continue;
            }
            if (hardware_advances_while_halted(emu)) {
                idle_cycles = cycle_budget - out->cycles_executed;
                if (idle_cycles > 4u) {
                    idle_cycles = 4u;
                }
                hardware_advance_cycles(emu, idle_cycles);
                out->cycles_executed += idle_cycles;
                continue;
            }
            out->hit_halt = true;
            break;
        }

        ei_was_pending = emu->cpu.ei_pending;
        pc_before = mask24(emu->cpu.pc);
        cpu_before = emu->cpu;
        status = ez80_emulator_step(emu, &decoded);
        if (status != EZ80_OK) {
            return status;
        }

        /* After executing the instruction following EI, clear ei_pending.
         * EI itself sets iff1/iff2 immediately AND sets ei_pending.
         * The pending flag just suppresses interrupt service for one instruction. */
        if (ei_was_pending) {
            emu->cpu.ei_pending = false;
        }

        pc_after = mask24(emu->cpu.pc);
        step_cycles = runtime_effective_cycles(&decoded, &cpu_before, pc_before, pc_after);
        out->cycles_executed += step_cycles;
        hardware_advance_cycles(emu, step_cycles);

        /* Service NMI first (non-maskable, not affected by EI delay) */
        irq_cycles = service_nmi(emu);
        if (irq_cycles > 0u) {
            out->cycles_executed += irq_cycles;
            hardware_advance_cycles(emu, irq_cycles);
        }

        /* Service maskable interrupts after each instruction.
         * Skip if EI was pending before this instruction - the delay means
         * interrupts don't fire until after the instruction following EI. */
        if (!ei_was_pending) {
            irq_cycles = service_pending_interrupts(emu);
            if (irq_cycles > 0u) {
                out->cycles_executed += irq_cycles;
                hardware_advance_cycles(emu, irq_cycles);
            }
        }
    }

    if (emu->cpu.halted) {
        out->hit_halt = true;
    }
    return EZ80_OK;
}

ez80_status_t ez80_emulator_get_state(const ez80_emulator_t *emu, ez80_cpu_state_t *out)
{
    if (!emu || !out) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    *out = emu->cpu;
    return EZ80_OK;
}

ez80_status_t ez80_memory_read8(const ez80_emulator_t *emu, uint32_t addr, uint8_t *out)
{
    if (!emu || !out) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    addr = mask24(addr);

    /* Check read watchpoints (cast away const; only modifies watchpoint trigger state) */
    {
        ez80_emulator_t *mutable_emu = (ez80_emulator_t *)(uintptr_t)emu;
        if (mutable_emu->watchpoint_count > 0u) {
            check_watchpoints(mutable_emu, addr, EZ80_WATCHPOINT_READ);
        }
    }

    if (addr_in_range(addr, EZ80_FLASH_BASE, EZ80_FLASH_TOTAL_BYTES)) {
        *out = flash_read_byte(emu, addr - EZ80_FLASH_BASE);
        return EZ80_OK;
    }
    if (addr_in_range(addr, EZ80_RAM_BASE, EZ80_RAM_BYTES)) {
        *out = emu->memory.ram[addr - EZ80_RAM_BASE];
        return EZ80_OK;
    }
    if (lcd_addr_in_range(addr)) {
        *out = lcd_read_byte(addr);
        return EZ80_OK;
    }
    if (keypad_addr_in_range(addr)) {
        *out = keypad_read_byte(emu, addr);
        return EZ80_OK;
    }
    if (system_mmio_addr_in_range(addr)) {
        *out = emu->memory.system_mmio[addr - EZ80_SYSTEM_MMIO_BASE];
        return EZ80_OK;
    }
    if (addr_in_range(addr, EZ80_PORT_BASE, EZ80_PORT_SPACE_BYTES)) {
        *out = port_read((ez80_emulator_t *)(uintptr_t)emu,
                         (uint16_t)(addr - EZ80_PORT_BASE));
        return EZ80_OK;
    }
    if (addr_in_range(addr, EZ80_MMIO_BASE, EZ80_MMIO_BYTES)) {
        uint16_t offset = (uint16_t)(addr - EZ80_MMIO_BASE);
        if (mmio_try_read_i2c((ez80_emulator_t *)(uintptr_t)emu, offset, out)) {
            return EZ80_OK;
        }
        if (mmio_try_read_uart((ez80_emulator_t *)(uintptr_t)emu, offset, out)) {
            return EZ80_OK;
        }
        *out = emu->memory.mmio[offset];
        return EZ80_OK;
    }

    return EZ80_ERR_OUT_OF_BOUNDS;
}

ez80_status_t ez80_memory_write8(ez80_emulator_t *emu, uint32_t addr, uint8_t value)
{
    uint32_t flash_addr;
    uint8_t failure_reason = EZ80_DNGRADE_FAILURE_NONE;
    uint8_t failure_port = 0u;
    uint8_t write_status = EZ80_WRITEFLASH_STATUS_NONE;
    uint8_t before;

    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    addr = mask24(addr);

    check_watchpoints(emu, addr, EZ80_WATCHPOINT_WRITE);

    if (addr_in_range(addr, EZ80_FLASH_BASE, EZ80_FLASH_TOTAL_BYTES)) {
        flash_addr = addr - EZ80_FLASH_BASE;
        before = flash_read_byte(emu, flash_addr);
        if (!flash_write_allowed(emu,
                                 flash_addr,
                                 value,
                                 false,
                                 &failure_reason,
                                 &failure_port,
                                 &write_status)) {
            platform_set_dngrade_failure(emu, failure_reason, failure_port);
            flash_record_writeflash(emu, write_status, flash_addr, value, before, before);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        flash_write_byte(emu, flash_addr, value);
        flash_record_writeflash(emu,
                                EZ80_WRITEFLASH_STATUS_SUCCESS,
                                flash_addr,
                                value,
                                before,
                                value);
        platform_set_dngrade_failure(emu, EZ80_DNGRADE_FAILURE_NONE, 0u);
        return EZ80_OK;
    }
    if (addr_in_range(addr, EZ80_RAM_BASE, EZ80_RAM_BYTES)) {
        emu->memory.ram[addr - EZ80_RAM_BASE] = value;
        return EZ80_OK;
    }
    if (lcd_addr_in_range(addr)) {
        lcd_write_byte(emu, addr, value);
        return EZ80_OK;
    }
    if (keypad_addr_in_range(addr)) {
        keypad_write_byte(emu, addr, value);
        hardware_sync_ports(emu);
        hardware_sync_mmio(emu);
        return EZ80_OK;
    }
    if (system_mmio_addr_in_range(addr)) {
        emu->memory.system_mmio[addr - EZ80_SYSTEM_MMIO_BASE] = value;
        return EZ80_OK;
    }
    if (addr_in_range(addr, EZ80_PORT_BASE, EZ80_PORT_SPACE_BYTES)) {
        port_write(emu, (uint16_t)(addr - EZ80_PORT_BASE), value);
        return EZ80_OK;
    }
    if (addr_in_range(addr, EZ80_MMIO_BASE, EZ80_MMIO_BYTES)) {
        uint16_t offset = (uint16_t)(addr - EZ80_MMIO_BASE);
        mmio_write_byte(emu, offset, value);
        return EZ80_OK;
    }

    return EZ80_ERR_OUT_OF_BOUNDS;
}

ez80_status_t ez80_emulator_add_breakpoint(ez80_emulator_t *emu, uint32_t addr)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (emu->breakpoint_count >= EZ80_MAX_BREAKPOINTS) return EZ80_ERR_OUT_OF_BOUNDS;

    addr = mask24(addr);

    /* Check for duplicate */
    {
        uint32_t i;
        for (i = 0u; i < emu->breakpoint_count; i++) {
            if (emu->breakpoints[i] == addr) return EZ80_OK;
        }
    }

    emu->breakpoints[emu->breakpoint_count] = addr;
    emu->breakpoint_count++;
    return EZ80_OK;
}

ez80_status_t ez80_emulator_remove_breakpoint(ez80_emulator_t *emu, uint32_t addr)
{
    uint32_t i;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    addr = mask24(addr);

    for (i = 0u; i < emu->breakpoint_count; i++) {
        if (emu->breakpoints[i] == addr) {
            /* Shift remaining breakpoints down */
            uint32_t j;
            for (j = i; j + 1u < emu->breakpoint_count; j++) {
                emu->breakpoints[j] = emu->breakpoints[j + 1u];
            }
            emu->breakpoint_count--;
            return EZ80_OK;
        }
    }
    return EZ80_OK;
}

ez80_status_t ez80_emulator_clear_breakpoints(ez80_emulator_t *emu)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    emu->breakpoint_count = 0u;
    return EZ80_OK;
}

ez80_status_t ez80_emulator_enable_interrupts(ez80_emulator_t *emu, bool enable)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    emu->interrupts_enabled_in_emulator = enable;
    return EZ80_OK;
}

ez80_status_t ez80_emulator_raise_interrupt(ez80_emulator_t *emu, uint8_t source)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    hardware_raise_irq(emu, source);
    hardware_sync_ports(emu);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_release_key(ez80_emulator_t *emu, uint8_t key_code)
{
    uint8_t row, col;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    row = (uint8_t)((key_code >> 3) & 0x07u);
    col = (uint8_t)(key_code & 0x07u);
    emu->hw.keypad_rows[row] |= (uint8_t)(1u << col);
    emu->hw.kbd_last_state = keyboard_compute_group_data(emu);
    mmio_set(emu, EZ80_MMIO_REG_KEY_STATUS, emu->hw.kbd_last_state);
    hardware_sync_ports(emu);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_release_all_keys(ez80_emulator_t *emu)
{
    uint8_t i;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    for (i = 0u; i < 8u; i++) {
        emu->hw.keypad_rows[i] = 0xFFu;
    }
    emu->hw.kbd_last_state = keyboard_compute_group_data(emu);
    mmio_set(emu, EZ80_MMIO_REG_KEY_STATUS, emu->hw.kbd_last_state);
    hardware_sync_ports(emu);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_lcd_get_framebuffer(const ez80_emulator_t *emu,
                                                 uint16_t *out_buffer,
                                                 size_t buffer_pixels)
{
    size_t total_pixels;
    size_t copy_count;
    if (!emu || !out_buffer) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    total_pixels = (size_t)EZ80_GRAPH_WIDTH * (size_t)EZ80_GRAPH_HEIGHT;
    copy_count = (buffer_pixels < total_pixels) ? buffer_pixels : total_pixels;
    memcpy(out_buffer, g_lcd_framebuffer, copy_count * sizeof(uint16_t));
    return EZ80_OK;
}

static bool screen_has_homescreen_overlay(const ez80_emulator_t *emu)
{
    uint32_t row;
    uint32_t col;
    if (!emu) return false;

    if (emu->os.last_bcall_vector == EZ80_BCALL_GRAPH_RENDER_TO_LCD) {
        return false;
    }
    if (emu->os.last_bcall_vector == EZ80_BCALL_HOME_CLEAR ||
        emu->os.last_bcall_vector == EZ80_BCALL_HOME_PUTC) {
        return true;
    }
    for (row = 0u; row < EZ80_HOME_ROWS; row++) {
        for (col = 0u; col < EZ80_HOME_COLS; col++) {
            if (g_home_screen[row][col] != ' ' &&
                g_home_screen[row][col] != '\0') {
                return true;
            }
        }
    }
    return false;
}

ez80_status_t ez80_emulator_lcd_to_ascii(const ez80_emulator_t *emu,
                                          char *out_buffer,
                                          size_t buffer_size,
                                          uint32_t *out_length)
{
    /*
     * Convert the 320x240 LCD to a 40x24 ASCII grid. When the homescreen is
     * active, emit the exact homescreen characters for the visible rows so
     * agent/CLI consumers can reason about text output deterministically.
     * Otherwise, fall back to cell-wide framebuffer sampling.
     */
    uint32_t char_cols = EZ80_SCREEN_ASCII_COLS;
    uint32_t char_rows = EZ80_SCREEN_ASCII_ROWS;
    uint32_t out_pos = 0u;
    uint32_t row, col;
    uint32_t max_out;
    bool homescreen_overlay;

    if (!emu || !out_buffer || !out_length) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    homescreen_overlay = screen_has_homescreen_overlay(emu);

    /* Output size: 40 chars + newline per row, + null terminator */
    max_out = (char_cols + 1u) * char_rows + 1u;
    if (buffer_size < max_out) return EZ80_ERR_OUT_OF_BOUNDS;

    for (row = 0u; row < char_rows; row++) {
        if (homescreen_overlay && row < EZ80_HOME_ROWS) {
            for (col = 0u; col < char_cols; col++) {
                if (col < EZ80_HOME_COLS) {
                    unsigned char ch = (unsigned char)g_home_screen[row][col];
                    out_buffer[out_pos++] = (ch != '\0' && isprint(ch)) ? (char)ch : ' ';
                } else {
                    out_buffer[out_pos++] = ' ';
                }
            }
            out_buffer[out_pos++] = '\n';
            continue;
        }

        for (col = 0u; col < char_cols; col++) {
            uint32_t start_x = col * 8u;
            uint32_t start_y = row * 10u;
            uint32_t brightness_sum = 0u;
            uint32_t dark_pixels = 0u;
            uint32_t sample_y;
            uint32_t sample_x;

            for (sample_y = 0u; sample_y < 10u; sample_y++) {
                for (sample_x = 0u; sample_x < 8u; sample_x++) {
                    uint32_t px = start_x + sample_x;
                    uint32_t py = start_y + sample_y;
                    uint32_t pixel_index = py * EZ80_GRAPH_WIDTH + px;
                    uint16_t pixel = g_lcd_framebuffer[pixel_index];
                    uint8_t r = (uint8_t)((pixel >> 11) & 0x1Fu);
                    uint8_t g = (uint8_t)((pixel >> 5) & 0x3Fu);
                    uint8_t b = (uint8_t)(pixel & 0x1Fu);
                    uint8_t brightness = (uint8_t)((r * 2u + g + b * 2u) / 5u);

                    brightness_sum += brightness;
                    if (brightness <= 8u) {
                        dark_pixels++;
                    }
                }
            }

            if (dark_pixels >= 32u || brightness_sum <= (80u * 4u)) {
                out_buffer[out_pos++] = '#';
            } else if (dark_pixels >= 8u || brightness_sum <= (80u * 8u)) {
                out_buffer[out_pos++] = ':';
            } else if (dark_pixels >= 2u || brightness_sum <= (80u * 16u)) {
                out_buffer[out_pos++] = '.';
            } else {
                out_buffer[out_pos++] = ' ';
            }
        }
        out_buffer[out_pos++] = '\n';
    }
    out_buffer[out_pos] = '\0';
    *out_length = out_pos;
    return EZ80_OK;
}

ez80_status_t ez80_emulator_get_screen_text_state(const ez80_emulator_t *emu,
                                                  ez80_screen_text_state_t *out_state)
{
    uint32_t row;
    uint32_t col;
    uint32_t ascii_len = 0u;
    if (!emu || !out_state) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    memset(out_state, 0, sizeof(*out_state));
    out_state->homescreen_active = screen_has_homescreen_overlay(emu);
    out_state->cursor_row = (emu->os.home_row < EZ80_HOME_ROWS)
        ? emu->os.home_row
        : (EZ80_HOME_ROWS - 1u);
    out_state->cursor_col = (emu->os.home_col <= EZ80_HOME_COLS)
        ? emu->os.home_col
        : EZ80_HOME_COLS;
    out_state->visible_line_count = EZ80_HOME_ROWS;

    for (row = 0u; row < EZ80_HOME_ROWS; row++) {
        bool non_empty = false;
        memcpy(out_state->visible_lines[row], g_home_screen[row], EZ80_HOME_COLS);
        out_state->visible_lines[row][EZ80_HOME_COLS] = '\0';

        for (col = 0u; col < EZ80_HOME_COLS; col++) {
            char ch = out_state->visible_lines[row][col];
            if (ch != '\0' && ch != ' ') {
                non_empty = true;
                break;
            }
        }

        if (row < out_state->cursor_row && non_empty && out_state->step_line_count < EZ80_HOME_ROWS) {
            memcpy(out_state->step_lines[out_state->step_line_count],
                   out_state->visible_lines[row],
                   EZ80_HOME_COLS + 1u);
            out_state->step_line_count++;
        }
    }

    memcpy(out_state->input_line,
           out_state->visible_lines[out_state->cursor_row],
           EZ80_HOME_COLS + 1u);

    return ez80_emulator_lcd_to_ascii(emu,
                                      out_state->ascii,
                                      sizeof(out_state->ascii),
                                      &ascii_len);
}

static uint32_t png_crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    size_t i;
    static uint32_t table[256];
    static bool table_ready = false;

    if (!table_ready) {
        uint32_t idx;
        for (idx = 0u; idx < 256u; idx++) {
            uint32_t value = idx;
            uint32_t bit;
            for (bit = 0u; bit < 8u; bit++) {
                if ((value & 1u) != 0u) {
                    value = (value >> 1u) ^ 0xEDB88320u;
                } else {
                    value >>= 1u;
                }
            }
            table[idx] = value;
        }
        table_ready = true;
    }

    for (i = 0u; i < size; i++) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8u);
    }
    return crc;
}

static uint32_t png_adler32(const uint8_t *data, size_t size)
{
    size_t i;
    uint32_t a = 1u;
    uint32_t b = 0u;
    for (i = 0u; i < size; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16u) | a;
}

static void png_store_u32_be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)((value >> 24u) & 0xFFu);
    out[1] = (uint8_t)((value >> 16u) & 0xFFu);
    out[2] = (uint8_t)((value >> 8u) & 0xFFu);
    out[3] = (uint8_t)(value & 0xFFu);
}

static bool png_write_chunk(FILE *file,
                            const char chunk_type[4],
                            const uint8_t *data,
                            uint32_t size)
{
    uint8_t size_be[4];
    uint8_t crc_be[4];
    uint32_t crc = 0xFFFFFFFFu;

    png_store_u32_be(size_be, size);
    if (fwrite(size_be, 1u, sizeof(size_be), file) != sizeof(size_be)) return false;
    if (fwrite(chunk_type, 1u, 4u, file) != 4u) return false;
    crc = png_crc32_update(crc, (const uint8_t *)chunk_type, 4u);
    if (size > 0u) {
        if (!data) return false;
        if (fwrite(data, 1u, size, file) != size) return false;
        crc = png_crc32_update(crc, data, size);
    }
    crc ^= 0xFFFFFFFFu;
    png_store_u32_be(crc_be, crc);
    return fwrite(crc_be, 1u, sizeof(crc_be), file) == sizeof(crc_be);
}

ez80_status_t ez80_emulator_lcd_write_png(const ez80_emulator_t *emu,
                                          const char *path)
{
    static const uint8_t png_signature[8] = { 0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au };
    static const char ihdr_type[4] = { 'I', 'H', 'D', 'R' };
    static const char idat_type[4] = { 'I', 'D', 'A', 'T' };
    static const char iend_type[4] = { 'I', 'E', 'N', 'D' };
    uint8_t ihdr[13];
    uint8_t *raw = NULL;
    uint8_t *idat = NULL;
    size_t raw_size;
    size_t idat_size;
    size_t row_stride;
    size_t row;
    size_t col;
    size_t raw_pos = 0u;
    size_t raw_offset = 0u;
    size_t idat_pos = 0u;
    size_t blocks;
    FILE *file = NULL;
    ez80_status_t result = EZ80_OK;

    if (!emu || !path) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    row_stride = 1u + ((size_t)EZ80_GRAPH_WIDTH * 3u);
    raw_size = row_stride * (size_t)EZ80_GRAPH_HEIGHT;
    blocks = (raw_size + 65534u) / 65535u;
    idat_size = 2u + raw_size + (blocks * 5u) + 4u;

    raw = (uint8_t *)malloc(raw_size);
    idat = (uint8_t *)malloc(idat_size);
    if (!raw || !idat) {
        result = EZ80_ERR_OUT_OF_BOUNDS;
        goto cleanup;
    }

    for (row = 0u; row < EZ80_GRAPH_HEIGHT; row++) {
        raw[raw_pos++] = 0u;
        for (col = 0u; col < EZ80_GRAPH_WIDTH; col++) {
            uint16_t pixel = g_lcd_framebuffer[row * EZ80_GRAPH_WIDTH + col];
            uint8_t r5 = (uint8_t)((pixel >> 11u) & 0x1Fu);
            uint8_t g6 = (uint8_t)((pixel >> 5u) & 0x3Fu);
            uint8_t b5 = (uint8_t)(pixel & 0x1Fu);
            raw[raw_pos++] = (uint8_t)((r5 << 3u) | (r5 >> 2u));
            raw[raw_pos++] = (uint8_t)((g6 << 2u) | (g6 >> 4u));
            raw[raw_pos++] = (uint8_t)((b5 << 3u) | (b5 >> 2u));
        }
    }

    idat[idat_pos++] = 0x78u;
    idat[idat_pos++] = 0x01u;
    while (raw_offset < raw_size) {
        uint16_t block_len = (uint16_t)((raw_size - raw_offset > 65535u) ? 65535u : (raw_size - raw_offset));
        uint16_t block_nlen = (uint16_t)~block_len;
        bool final_block = (raw_offset + block_len) >= raw_size;
        idat[idat_pos++] = final_block ? 0x01u : 0x00u;
        idat[idat_pos++] = (uint8_t)(block_len & 0xFFu);
        idat[idat_pos++] = (uint8_t)((block_len >> 8u) & 0xFFu);
        idat[idat_pos++] = (uint8_t)(block_nlen & 0xFFu);
        idat[idat_pos++] = (uint8_t)((block_nlen >> 8u) & 0xFFu);
        memcpy(idat + idat_pos, raw + raw_offset, block_len);
        idat_pos += block_len;
        raw_offset += block_len;
    }
    png_store_u32_be(idat + idat_pos, png_adler32(raw, raw_size));
    idat_pos += 4u;

    png_store_u32_be(ihdr + 0u, EZ80_GRAPH_WIDTH);
    png_store_u32_be(ihdr + 4u, EZ80_GRAPH_HEIGHT);
    ihdr[8] = 8u;
    ihdr[9] = 2u;
    ihdr[10] = 0u;
    ihdr[11] = 0u;
    ihdr[12] = 0u;

    file = fopen(path, "wb");
    if (!file) {
        result = EZ80_ERR_OUT_OF_BOUNDS;
        goto cleanup;
    }
    if (fwrite(png_signature, 1u, sizeof(png_signature), file) != sizeof(png_signature) ||
        !png_write_chunk(file, ihdr_type, ihdr, sizeof(ihdr)) ||
        !png_write_chunk(file, idat_type, idat, (uint32_t)idat_pos) ||
        !png_write_chunk(file, iend_type, NULL, 0u)) {
        result = EZ80_ERR_OUT_OF_BOUNDS;
        goto cleanup;
    }

cleanup:
    if (file) fclose(file);
    free(idat);
    free(raw);
    return result;
}

const char *ez80_status_string(ez80_status_t status)
{
    switch (status) {
    case EZ80_OK: return "ok";
    case EZ80_ERR_INVALID_ARG: return "invalid argument";
    case EZ80_ERR_NOT_INITIALIZED: return "not initialized";
    case EZ80_ERR_NOT_IMPLEMENTED: return "not implemented";
    case EZ80_ERR_ROM_FORMAT: return "rom format/availability error";
    case EZ80_ERR_OUT_OF_BOUNDS: return "out of bounds";
    default: return "unknown status";
    }
}

ez80_status_t ez80_emulator_add_watchpoint(ez80_emulator_t *emu,
                                            uint32_t addr,
                                            uint32_t length,
                                            uint8_t type)
{
    uint32_t i;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (length == 0u) return EZ80_ERR_INVALID_ARG;
    if ((type & EZ80_WATCHPOINT_READWRITE) == 0u) return EZ80_ERR_INVALID_ARG;
    addr = mask24(addr);

    /* Check for duplicate */
    for (i = 0u; i < emu->watchpoint_count; i++) {
        if (emu->watchpoints[i].addr == addr &&
            emu->watchpoints[i].length == length &&
            emu->watchpoints[i].type == type) {
            return EZ80_OK;
        }
    }

    if (emu->watchpoint_count >= EZ80_MAX_WATCHPOINTS) {
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    emu->watchpoints[emu->watchpoint_count].addr = addr;
    emu->watchpoints[emu->watchpoint_count].length = length;
    emu->watchpoints[emu->watchpoint_count].type = type;
    emu->watchpoint_count++;
    emu->memory.mmio[EZ80_MMIO_REG_WATCHPOINT_COUNT] = (uint8_t)emu->watchpoint_count;
    return EZ80_OK;
}

ez80_status_t ez80_emulator_remove_watchpoint(ez80_emulator_t *emu,
                                               uint32_t addr)
{
    uint32_t i;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    addr = mask24(addr);

    for (i = 0u; i < emu->watchpoint_count; i++) {
        if (emu->watchpoints[i].addr == addr) {
            if (i < emu->watchpoint_count - 1u) {
                emu->watchpoints[i] = emu->watchpoints[emu->watchpoint_count - 1u];
            }
            emu->watchpoint_count--;
            emu->memory.mmio[EZ80_MMIO_REG_WATCHPOINT_COUNT] = (uint8_t)emu->watchpoint_count;
            return EZ80_OK;
        }
    }
    return EZ80_ERR_INVALID_ARG;
}

ez80_status_t ez80_emulator_clear_watchpoints(ez80_emulator_t *emu)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    emu->watchpoint_count = 0u;
    emu->watchpoint_triggered = false;
    emu->memory.mmio[EZ80_MMIO_REG_WATCHPOINT_COUNT] = 0u;
    return EZ80_OK;
}

ez80_status_t ez80_emulator_set_timer_prescaler(ez80_emulator_t *emu,
                                                 uint8_t timer_id,
                                                 uint8_t prescaler)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (prescaler > 3u) return EZ80_ERR_INVALID_ARG;

    if (timer_id == 0u) {
        emu->hw.timer0_prescaler = prescaler;
        emu->hw.timer0_prescaler_accum = 0u;
        emu->memory.mmio[EZ80_MMIO_REG_TIMER0_PRESCALER] = prescaler;
    } else if (timer_id == 1u) {
        emu->hw.timer1_prescaler = prescaler;
        emu->hw.timer1_prescaler_accum = 0u;
        emu->memory.mmio[EZ80_MMIO_REG_TIMER1_PRESCALER] = prescaler;
    } else {
        return EZ80_ERR_INVALID_ARG;
    }
    return EZ80_OK;
}

ez80_status_t ez80_emulator_set_timer_reload(ez80_emulator_t *emu,
                                              uint8_t timer_id,
                                              uint16_t reload_value)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    if (timer_id == 0u) {
        emu->hw.timer0_reload = reload_value;
        emu->memory.mmio[EZ80_MMIO_REG_TIMER0_RELOAD_LO] = (uint8_t)(reload_value & 0xFFu);
        emu->memory.mmio[EZ80_MMIO_REG_TIMER0_RELOAD_HI] = (uint8_t)((reload_value >> 8) & 0xFFu);
    } else if (timer_id == 1u) {
        emu->hw.timer1_reload = reload_value;
        emu->memory.mmio[EZ80_MMIO_REG_TIMER1_RELOAD_LO] = (uint8_t)(reload_value & 0xFFu);
        emu->memory.mmio[EZ80_MMIO_REG_TIMER1_RELOAD_HI] = (uint8_t)((reload_value >> 8) & 0xFFu);
    } else {
        return EZ80_ERR_INVALID_ARG;
    }
    return EZ80_OK;
}

/* ---- DMA API ---- */
ez80_status_t ez80_emulator_dma_configure(ez80_emulator_t *emu,
                                           uint8_t channel,
                                           uint32_t src_addr,
                                           uint32_t dst_addr,
                                           uint32_t length,
                                           uint8_t control)
{
    ez80_dma_channel_t *dma;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_DMA_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    dma = &emu->hw.dma[channel];
    dma->src_addr = mask24(src_addr);
    dma->dst_addr = mask24(dst_addr);
    dma->length = length;
    dma->control = control;
    dma->status = EZ80_DMA_STATUS_IDLE;
    dma->bytes_transferred = 0u;
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_dma_start(ez80_emulator_t *emu, uint8_t channel)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_DMA_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    emu->hw.dma[channel].control |= EZ80_DMA_CONTROL_START;
    dma_execute_channel(emu, channel);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_dma_abort(ez80_emulator_t *emu, uint8_t channel)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_DMA_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    emu->hw.dma[channel].control &= (uint8_t)~(EZ80_DMA_CONTROL_ENABLE | EZ80_DMA_CONTROL_START);
    emu->hw.dma[channel].status = EZ80_DMA_STATUS_IDLE;
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_dma_get_status(const ez80_emulator_t *emu,
                                            uint8_t channel,
                                            uint8_t *out_status,
                                            uint32_t *out_bytes_transferred)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_DMA_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    if (out_status) *out_status = emu->hw.dma[channel].status;
    if (out_bytes_transferred) *out_bytes_transferred = emu->hw.dma[channel].bytes_transferred;
    return EZ80_OK;
}

/* ---- NMI API ---- */
ez80_status_t ez80_emulator_raise_nmi(ez80_emulator_t *emu)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    emu->hw.nmi_pending = true;
    pm_note_wake_event(emu, EZ80_PM_WAKE_NMI, false);
    return EZ80_OK;
}

/* ---- SPI API ---- */
ez80_status_t ez80_emulator_spi_configure(ez80_emulator_t *emu,
                                           uint8_t channel,
                                           uint8_t control,
                                           uint8_t baud_divisor)
{
    ez80_spi_channel_t *spi;
    uint8_t old_control;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_SPI_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    spi = &emu->hw.spi[channel];
    old_control = spi->control;
    if ((control & EZ80_SPI_CONTROL_RESET) != 0u) {
        spi_handle_control_transition(emu, spi, old_control, 0u);
        spi_reset_channel(spi);
    } else {
        spi->control = control;
        if ((control & EZ80_SPI_CONTROL_IRQ_ENABLE) == 0u) {
            spi->status &= (uint8_t)~EZ80_SPI_STATUS_IRQ_PENDING;
        }
        spi_handle_control_transition(emu, spi, old_control, control);
    }
    spi->baud_divisor = baud_divisor;
    spi_update_status(spi);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_spi_transfer(ez80_emulator_t *emu,
                                          uint8_t channel,
                                          uint8_t tx_byte,
                                          uint8_t *out_rx_byte)
{
    ez80_spi_channel_t *spi;
    ez80_status_t transfer_status;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_SPI_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    spi = &emu->hw.spi[channel];
    if (out_rx_byte) *out_rx_byte = 0xFFu;
    if ((spi->control & EZ80_SPI_CONTROL_CS_ACTIVE) == 0u) {
        spi_finalize_burst(spi);
    }
    transfer_status = spi_perform_transfer(emu, spi, tx_byte, out_rx_byte, false);
    if (transfer_status != EZ80_OK) {
        hardware_sync_mmio(emu);
        return transfer_status;
    }
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_spi_get_status(const ez80_emulator_t *emu,
                                            uint8_t channel,
                                            uint8_t *out_status)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_SPI_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;
    if (out_status) *out_status = emu->hw.spi[channel].status;
    return EZ80_OK;
}

ez80_status_t ez80_emulator_spi_set_chip_select(ez80_emulator_t *emu,
                                                 uint8_t channel,
                                                 bool active)
{
    ez80_spi_channel_t *spi;
    uint8_t old_control;

    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_SPI_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    spi = &emu->hw.spi[channel];
    old_control = spi->control;
    if (active) {
        spi->control |= EZ80_SPI_CONTROL_CS_ACTIVE;
    } else {
        spi->control &= (uint8_t)~EZ80_SPI_CONTROL_CS_ACTIVE;
    }
    spi_handle_control_transition(emu, spi, old_control, spi->control);
    spi_update_status(spi);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_spi_load_rx(ez80_emulator_t *emu,
                                         uint8_t channel,
                                         uint8_t value)
{
    ez80_spi_channel_t *spi;
    uint8_t idx;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_SPI_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    spi = &emu->hw.spi[channel];
    if (spi->rx_source_count >= EZ80_SPI_FIFO_SIZE) {
        spi->status |= EZ80_SPI_STATUS_OVERRUN;
        spi_update_status(spi);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }
    idx = (uint8_t)((spi->rx_source_head + spi->rx_source_count) % EZ80_SPI_FIFO_SIZE);
    spi->rx_source_fifo[idx] = value;
    spi->rx_source_count++;
    spi_update_status(spi);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

/* ---- GPIO API ---- */
ez80_status_t ez80_emulator_gpio_write(ez80_emulator_t *emu,
                                        uint8_t port_index,
                                        uint8_t value)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (port_index >= EZ80_GPIO_PORT_COUNT) return EZ80_ERR_INVALID_ARG;

    emu->hw.gpio[port_index].output_latch = (uint8_t)(
        (emu->hw.gpio[port_index].output_latch & (uint8_t)~emu->hw.gpio[port_index].direction) |
        (value & emu->hw.gpio[port_index].direction)
    );

    if (port_index == EZ80_GPIO_BACKLIGHT_PORT &&
        ((emu->hw.gpio[port_index].alt0 | emu->hw.gpio[port_index].alt1) & EZ80_GPIO_BACKLIGHT_BIT) == 0u &&
        (emu->hw.gpio[port_index].direction & EZ80_GPIO_BACKLIGHT_BIT) != 0u) {
        emu->hw.backlight_on = (emu->hw.gpio[port_index].output_latch & EZ80_GPIO_BACKLIGHT_BIT) != 0u;
    }

    gpio_refresh_port_state(emu, port_index);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_gpio_read(const ez80_emulator_t *emu,
                                       uint8_t port_index,
                                       uint8_t *out_value)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (port_index >= EZ80_GPIO_PORT_COUNT) return EZ80_ERR_INVALID_ARG;
    gpio_refresh_port_state((ez80_emulator_t *)emu, port_index);
    if (out_value) *out_value = emu->hw.gpio[port_index].data;
    return EZ80_OK;
}

ez80_status_t ez80_emulator_gpio_set_direction(ez80_emulator_t *emu,
                                                uint8_t port_index,
                                                uint8_t direction)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (port_index >= EZ80_GPIO_PORT_COUNT) return EZ80_ERR_INVALID_ARG;

    emu->hw.gpio[port_index].direction = direction;
    gpio_refresh_port_state(emu, port_index);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_gpio_drive_input(ez80_emulator_t *emu,
                                              uint8_t port_index,
                                              uint8_t value,
                                              uint8_t mask)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (port_index >= EZ80_GPIO_PORT_COUNT) return EZ80_ERR_INVALID_ARG;

    emu->hw.gpio[port_index].input_latch = value;
    emu->hw.gpio[port_index].input_mask = mask;
    gpio_sync_external_inputs(emu);
    gpio_refresh_port_state(emu, port_index);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_set_backlight(ez80_emulator_t *emu, bool on)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    emu->hw.backlight_on = on;
    if (on) {
        emu->hw.gpio[EZ80_GPIO_BACKLIGHT_PORT].output_latch |= EZ80_GPIO_BACKLIGHT_BIT;
    } else {
        emu->hw.gpio[EZ80_GPIO_BACKLIGHT_PORT].output_latch &= (uint8_t)~EZ80_GPIO_BACKLIGHT_BIT;
    }
    gpio_sync_external_inputs(emu);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_set_battery_level(ez80_emulator_t *emu,
                                               uint8_t level)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (level > 100u) return EZ80_ERR_INVALID_ARG;

    emu->hw.battery_level = level;
    gpio_sync_external_inputs(emu);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

/* ---- USB controller API ---- */
ez80_status_t ez80_emulator_usb_set_connected(ez80_emulator_t *emu,
                                               bool connected)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    usb_set_connected_state(emu, connected);
    hardware_sync_ports(emu);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_usb_load_rx(ez80_emulator_t *emu,
                                         const uint8_t *data,
                                         uint8_t count)
{
    uint8_t i;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (count > 0u && !data) return EZ80_ERR_INVALID_ARG;

    for (i = 0u; i < count; i++) {
        usb_push_rx_byte(emu, data[i], true);
    }
    hardware_sync_ports(emu);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_usb_get_status(const ez80_emulator_t *emu,
                                            uint8_t *out_status)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!out_status) return EZ80_ERR_INVALID_ARG;
    *out_status = emu->hw.usb_status;
    return EZ80_OK;
}

/* ---- Flash sector erase API ---- */
ez80_status_t ez80_emulator_flash_erase_sector(ez80_emulator_t *emu,
                                                uint8_t sector)
{
    uint32_t base_addr;

    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (sector >= EZ80_FLASH_SECTOR_COUNT) return EZ80_ERR_INVALID_ARG;

    emu->hw.flash_erase_last_sector = sector;

    /* Check flash protection mode */
    if (emu->platform.flash_protect_mode == EZ80_FLASH_PROTECT_MODE_LOCKED) {
        emu->hw.flash_erase_last_status = EZ80_FLASH_ERASE_STATUS_LOCKED;
        hardware_sync_mmio(emu);
        return EZ80_OK;
    }

    /* Check OS-locked sectors (boot pages, cert pages on OS 5.8+) */
    if (flash_sector_os_locked(emu, sector)) {
        emu->hw.flash_erase_last_status = EZ80_FLASH_ERASE_STATUS_OS_LOCKED;
        hardware_sync_mmio(emu);
        return EZ80_OK;
    }

    /* Check per-sector protection */
    if (emu->platform.flash_sector_protected[sector]) {
        emu->hw.flash_erase_last_status = EZ80_FLASH_ERASE_STATUS_PROTECTED;
        hardware_sync_mmio(emu);
        return EZ80_OK;
    }

    /* Perform the erase: set all bytes in the sector to 0xFF */
    base_addr = (uint32_t)sector * EZ80_FLASH_SECTOR_BYTES;
    flash_erase_range(emu, base_addr, EZ80_FLASH_SECTOR_BYTES);

    emu->hw.flash_erase_last_status = EZ80_FLASH_ERASE_STATUS_SUCCESS;
    emu->hw.flash_erase_count++;
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

/* ---- I2C controller API ---- */

static void i2c_update_status_flags(ez80_i2c_channel_t *i2c)
{
    if ((i2c->control & EZ80_I2C_CTL_IFLG) != 0u) {
        /* interrupt flag already set, status reflects last operation */
    }
}

ez80_status_t ez80_emulator_i2c_configure(ez80_emulator_t *emu,
                                           uint8_t channel,
                                           uint8_t control,
                                           uint8_t clock_control)
{
    ez80_i2c_channel_t *i2c;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_I2C_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    i2c = &emu->hw.i2c[channel];
    i2c->control = control;
    i2c->clock_control = clock_control;
    if ((control & EZ80_I2C_CTL_ENAB) == 0u) {
        i2c->status = EZ80_I2C_SR_IDLE;
        i2c->tx_head = 0u;
        i2c->tx_count = 0u;
        i2c->rx_head = 0u;
        i2c->rx_count = 0u;
        i2c->start_pending = false;
        i2c->stop_pending = false;
    }
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_i2c_start(ez80_emulator_t *emu,
                                       uint8_t channel,
                                       uint8_t slave_addr,
                                       bool read)
{
    ez80_i2c_channel_t *i2c;
    uint8_t addr_byte;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_I2C_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    i2c = &emu->hw.i2c[channel];
    if ((i2c->control & EZ80_I2C_CTL_ENAB) == 0u) return EZ80_ERR_NOT_INITIALIZED;

    addr_byte = (uint8_t)((slave_addr << 1) | (read ? 1u : 0u));
    i2c->slave_addr = addr_byte;

    /* In our simulation, start always succeeds */
    if (i2c->status == EZ80_I2C_SR_IDLE) {
        i2c->status = EZ80_I2C_SR_START_SENT;
    } else {
        i2c->status = EZ80_I2C_SR_REP_START_SENT;
    }
    i2c->control |= EZ80_I2C_CTL_IFLG;
    i2c->start_pending = true;

    /* After start, the next step is SLA+W/R ack */
    if (read) {
        i2c->status = EZ80_I2C_SR_SLA_R_ACK;
    } else {
        i2c->status = EZ80_I2C_SR_SLA_W_ACK;
    }

    if ((i2c->control & EZ80_I2C_CTL_IEN) != 0u) {
        hardware_raise_irq(emu, EZ80_IRQ_SOURCE_I2C);
    }
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_i2c_write(ez80_emulator_t *emu,
                                       uint8_t channel,
                                       uint8_t data)
{
    ez80_i2c_channel_t *i2c;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_I2C_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    i2c = &emu->hw.i2c[channel];
    if ((i2c->control & EZ80_I2C_CTL_ENAB) == 0u) return EZ80_ERR_NOT_INITIALIZED;

    /* Store in TX buffer */
    if (i2c->tx_count < EZ80_I2C_BUFFER_SIZE) {
        uint8_t idx = (uint8_t)((i2c->tx_head + i2c->tx_count) % EZ80_I2C_BUFFER_SIZE);
        i2c->tx_buf[idx] = data;
        i2c->tx_count++;
    }
    i2c->total_tx_bytes++;
    i2c->status = EZ80_I2C_SR_DATA_W_ACK;
    i2c->control |= EZ80_I2C_CTL_IFLG;

    if ((i2c->control & EZ80_I2C_CTL_IEN) != 0u) {
        hardware_raise_irq(emu, EZ80_IRQ_SOURCE_I2C);
    }
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_i2c_read(ez80_emulator_t *emu,
                                      uint8_t channel,
                                      uint8_t *out_data,
                                      bool ack)
{
    ez80_i2c_channel_t *i2c;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_I2C_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    i2c = &emu->hw.i2c[channel];
    if ((i2c->control & EZ80_I2C_CTL_ENAB) == 0u) return EZ80_ERR_NOT_INITIALIZED;

    if (i2c->rx_count > 0u) {
        if (out_data) *out_data = i2c->rx_buf[i2c->rx_head];
        i2c->rx_head = (uint8_t)((i2c->rx_head + 1u) % EZ80_I2C_BUFFER_SIZE);
        i2c->rx_count--;
        i2c->total_rx_bytes++;
    } else {
        if (out_data) *out_data = 0xFFu;
    }

    if (ack) {
        i2c->status = EZ80_I2C_SR_DATA_R_ACK;
        i2c->control |= EZ80_I2C_CTL_AAK;
    } else {
        i2c->status = EZ80_I2C_SR_DATA_R_NACK;
        i2c->control &= (uint8_t)~EZ80_I2C_CTL_AAK;
    }
    i2c->control |= EZ80_I2C_CTL_IFLG;

    if ((i2c->control & EZ80_I2C_CTL_IEN) != 0u) {
        hardware_raise_irq(emu, EZ80_IRQ_SOURCE_I2C);
    }
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_i2c_stop(ez80_emulator_t *emu,
                                      uint8_t channel)
{
    ez80_i2c_channel_t *i2c;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_I2C_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    i2c = &emu->hw.i2c[channel];
    i2c->status = EZ80_I2C_SR_IDLE;
    i2c->control &= (uint8_t)~(EZ80_I2C_CTL_STA | EZ80_I2C_CTL_STP | EZ80_I2C_CTL_IFLG);
    i2c->start_pending = false;
    i2c->stop_pending = false;
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_i2c_load_rx(ez80_emulator_t *emu,
                                         uint8_t channel,
                                         const uint8_t *data,
                                         uint8_t count)
{
    ez80_i2c_channel_t *i2c;
    uint8_t i;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_I2C_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;
    if (!data && count > 0u) return EZ80_ERR_INVALID_ARG;

    i2c = &emu->hw.i2c[channel];
    for (i = 0u; i < count && i2c->rx_count < EZ80_I2C_BUFFER_SIZE; i++) {
        uint8_t idx = (uint8_t)((i2c->rx_head + i2c->rx_count) % EZ80_I2C_BUFFER_SIZE);
        i2c->rx_buf[idx] = data[i];
        i2c->rx_count++;
    }
    return EZ80_OK;
}

ez80_status_t ez80_emulator_i2c_get_status(const ez80_emulator_t *emu,
                                            uint8_t channel,
                                            uint8_t *out_status)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_I2C_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;
    if (out_status) *out_status = emu->hw.i2c[channel].status;
    return EZ80_OK;
}

/* ---- I2C port handlers ---- */
static uint8_t i2c_channel_from_port(uint16_t port)
{
    if (port >= EZ80_PORT_I2C1_BASE && port <= EZ80_PORT_I2C1_SRR) return 1u;
    return 0u;
}

static uint8_t i2c_reg_from_port(uint16_t port)
{
    if (port >= EZ80_PORT_I2C1_BASE) return (uint8_t)(port - EZ80_PORT_I2C1_BASE);
    return (uint8_t)(port - EZ80_PORT_I2C0_BASE);
}

static ez80_status_t io_i2c_handler_write(ez80_emulator_t *emu,
                                           uint16_t port,
                                           uint8_t value,
                                           void *user_data)
{
    uint8_t ch = i2c_channel_from_port(port);
    uint8_t reg = i2c_reg_from_port(port);
    ez80_i2c_channel_t *i2c;
    (void)user_data;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (ch >= EZ80_I2C_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;
    i2c = &emu->hw.i2c[ch];

    switch (reg) {
    case 0u: /* SAR - slave address */
        i2c->slave_addr = value;
        break;
    case 1u: /* XSAR - extended slave address */
        i2c->xslave_addr = value;
        break;
    case 2u: /* DR - data register (write = TX) */
        if ((i2c->control & EZ80_I2C_CTL_ENAB) != 0u) {
            ez80_emulator_i2c_write(emu, ch, value);
        }
        break;
    case 3u: /* CTL - control register */
        i2c->control = value;
        if ((value & EZ80_I2C_CTL_STA) != 0u) {
            i2c->start_pending = true;
        }
        if ((value & EZ80_I2C_CTL_STP) != 0u) {
            ez80_emulator_i2c_stop(emu, ch);
        }
        break;
    case 5u: /* CCR - clock control */
        i2c->clock_control = value;
        break;
    case 6u: /* SRR - software reset */
        i2c->control = 0u;
        i2c->status = EZ80_I2C_SR_IDLE;
        i2c->tx_head = 0u;
        i2c->tx_count = 0u;
        i2c->rx_head = 0u;
        i2c->rx_count = 0u;
        i2c->start_pending = false;
        i2c->stop_pending = false;
        break;
    default:
        break;
    }
    return EZ80_OK;
}

static ez80_status_t io_i2c_handler_read(ez80_emulator_t *emu,
                                          uint16_t port,
                                          uint8_t *out_value,
                                          void *user_data)
{
    uint8_t ch = i2c_channel_from_port(port);
    uint8_t reg = i2c_reg_from_port(port);
    ez80_i2c_channel_t *i2c;
    (void)user_data;
    if (!emu || !out_value) return EZ80_ERR_INVALID_ARG;
    if (ch >= EZ80_I2C_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;
    i2c = &emu->hw.i2c[ch];

    switch (reg) {
    case 0u: *out_value = i2c->slave_addr; break;
    case 1u: *out_value = i2c->xslave_addr; break;
    case 2u: /* DR - data register (read = RX) */
        if (i2c->rx_count > 0u) {
            *out_value = i2c->rx_buf[i2c->rx_head];
            i2c->rx_head = (uint8_t)((i2c->rx_head + 1u) % EZ80_I2C_BUFFER_SIZE);
            i2c->rx_count--;
            i2c->total_rx_bytes++;
        } else {
            *out_value = 0xFFu;
        }
        break;
    case 3u: *out_value = i2c->control; break;
    case 4u: *out_value = i2c->status; break;
    case 5u: *out_value = i2c->clock_control; break;
    default: *out_value = 0u; break;
    }
    return EZ80_OK;
}

/* ---- UART controller API ---- */

static void uart_update_lsr(ez80_uart_channel_t *uart)
{
    /* Preserve sticky error bits (OE, PE, FE, BI, ERR) */
    uint8_t err_bits = uart->lsr & (EZ80_UART_LSR_OE | EZ80_UART_LSR_PE |
                                     EZ80_UART_LSR_FE | EZ80_UART_LSR_BI |
                                     EZ80_UART_LSR_ERR);
    uint8_t lsr = err_bits;
    if (uart->rx_count > 0u) lsr |= EZ80_UART_LSR_DR;
    if (uart->tx_count == 0u) lsr |= EZ80_UART_LSR_THRE | EZ80_UART_LSR_TEMT;
    uart->lsr = lsr;
}

ez80_status_t ez80_emulator_uart_configure(ez80_emulator_t *emu,
                                            uint8_t channel,
                                            uint8_t lctl,
                                            uint16_t brg)
{
    ez80_uart_channel_t *uart;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_UART_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    uart = &emu->hw.uart[channel];
    uart->lctl = lctl;
    uart->brg = (brg == 0u) ? 1u : brg;
    uart_update_lsr(uart);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_uart_write(ez80_emulator_t *emu,
                                        uint8_t channel,
                                        uint8_t data)
{
    ez80_uart_channel_t *uart;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_UART_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    uart = &emu->hw.uart[channel];
    if (uart->tx_count < EZ80_UART_FIFO_SIZE) {
        uint8_t idx = (uint8_t)((uart->tx_head + uart->tx_count) % EZ80_UART_FIFO_SIZE);
        uart->tx_fifo[idx] = data;
        uart->tx_count++;
    } else {
        /* Overrun: data lost */
        uart->lsr |= EZ80_UART_LSR_OE;
    }
    uart->total_tx_bytes++;
    uart_update_lsr(uart);

    if ((uart->ier & EZ80_UART_IER_THRE) != 0u && uart->tx_count == 0u) {
        hardware_raise_irq(emu, EZ80_IRQ_SOURCE_UART);
    }
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_uart_read(ez80_emulator_t *emu,
                                       uint8_t channel,
                                       uint8_t *out_data)
{
    ez80_uart_channel_t *uart;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_UART_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;

    uart = &emu->hw.uart[channel];
    if (uart->rx_count > 0u) {
        if (out_data) *out_data = uart->rx_fifo[uart->rx_head];
        uart->rx_head = (uint8_t)((uart->rx_head + 1u) % EZ80_UART_FIFO_SIZE);
        uart->rx_count--;
        uart->total_rx_bytes++;
    } else {
        if (out_data) *out_data = 0x00u;
    }
    uart_update_lsr(uart);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_uart_load_rx(ez80_emulator_t *emu,
                                          uint8_t channel,
                                          const uint8_t *data,
                                          uint8_t count)
{
    ez80_uart_channel_t *uart;
    uint8_t i;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_UART_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;
    if (!data && count > 0u) return EZ80_ERR_INVALID_ARG;

    uart = &emu->hw.uart[channel];
    for (i = 0u; i < count && uart->rx_count < EZ80_UART_FIFO_SIZE; i++) {
        uint8_t idx = (uint8_t)((uart->rx_head + uart->rx_count) % EZ80_UART_FIFO_SIZE);
        uart->rx_fifo[idx] = data[i];
        uart->rx_count++;
    }
    uart_update_lsr(uart);
    if ((uart->ier & EZ80_UART_IER_RECV) != 0u && uart->rx_count > 0u) {
        hardware_raise_irq(emu, EZ80_IRQ_SOURCE_UART);
    }
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_uart_get_lsr(const ez80_emulator_t *emu,
                                          uint8_t channel,
                                          uint8_t *out_lsr)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (channel >= EZ80_UART_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;
    if (out_lsr) *out_lsr = emu->hw.uart[channel].lsr;
    return EZ80_OK;
}

/* ---- UART port handlers ---- */
static uint8_t uart_channel_from_port(uint16_t port)
{
    if (port >= EZ80_PORT_UART1_BASE && port <= EZ80_PORT_UART1_SPR) return 1u;
    return 0u;
}

static uint8_t uart_reg_from_port(uint16_t port)
{
    if (port >= EZ80_PORT_UART1_BASE) return (uint8_t)(port - EZ80_PORT_UART1_BASE);
    return (uint8_t)(port - EZ80_PORT_UART0_BASE);
}

static ez80_status_t io_uart_handler_write(ez80_emulator_t *emu,
                                            uint16_t port,
                                            uint8_t value,
                                            void *user_data)
{
    uint8_t ch = uart_channel_from_port(port);
    uint8_t reg = uart_reg_from_port(port);
    ez80_uart_channel_t *uart;
    (void)user_data;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (ch >= EZ80_UART_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;
    uart = &emu->hw.uart[ch];

    switch (reg) {
    case 0u: /* THR / BRG_L */
        if ((uart->lctl & EZ80_UART_LCTL_DLAB) != 0u) {
            uart->brg = (uint16_t)((uart->brg & 0xFF00u) | value);
        } else {
            ez80_emulator_uart_write(emu, ch, value);
        }
        break;
    case 1u: /* IER / BRG_H */
        if ((uart->lctl & EZ80_UART_LCTL_DLAB) != 0u) {
            uart->brg = (uint16_t)((uart->brg & 0x00FFu) | ((uint16_t)value << 8));
        } else {
            uart->ier = value;
        }
        break;
    case 2u: /* FCTL (write) */
        uart->fctl = value;
        if ((value & EZ80_UART_FCTL_CLRRx) != 0u) {
            uart->rx_head = 0u;
            uart->rx_count = 0u;
        }
        if ((value & EZ80_UART_FCTL_CLRTx) != 0u) {
            uart->tx_head = 0u;
            uart->tx_count = 0u;
        }
        uart_update_lsr(uart);
        break;
    case 3u: /* LCTL */
        uart->lctl = value;
        break;
    case 4u: /* MCR */
        uart->mcr = value;
        break;
    case 7u: /* SPR */
        uart->spr = value;
        break;
    default:
        break;
    }
    return EZ80_OK;
}

static ez80_status_t io_uart_handler_read(ez80_emulator_t *emu,
                                           uint16_t port,
                                           uint8_t *out_value,
                                           void *user_data)
{
    uint8_t ch = uart_channel_from_port(port);
    uint8_t reg = uart_reg_from_port(port);
    ez80_uart_channel_t *uart;
    (void)user_data;
    if (!emu || !out_value) return EZ80_ERR_INVALID_ARG;
    if (ch >= EZ80_UART_CHANNEL_COUNT) return EZ80_ERR_INVALID_ARG;
    uart = &emu->hw.uart[ch];

    switch (reg) {
    case 0u: /* RBR / BRG_L */
        if ((uart->lctl & EZ80_UART_LCTL_DLAB) != 0u) {
            *out_value = (uint8_t)(uart->brg & 0xFFu);
        } else {
            ez80_emulator_uart_read(emu, ch, out_value);
        }
        break;
    case 1u: /* IER / BRG_H */
        if ((uart->lctl & EZ80_UART_LCTL_DLAB) != 0u) {
            *out_value = (uint8_t)((uart->brg >> 8) & 0xFFu);
        } else {
            *out_value = uart->ier;
        }
        break;
    case 2u: /* IIR (read) */
        *out_value = uart->iir;
        break;
    case 3u: *out_value = uart->lctl; break;
    case 4u: *out_value = uart->mcr; break;
    case 5u: /* LSR */
        uart_update_lsr(uart);
        *out_value = uart->lsr;
        break;
    case 6u: *out_value = uart->msr; break;
    case 7u: *out_value = uart->spr; break;
    default: *out_value = 0u; break;
    }
    return EZ80_OK;
}

/* ---- Execution trace API ---- */

ez80_status_t ez80_emulator_trace_enable(ez80_emulator_t *emu, bool enable)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    emu->hw.trace_enabled = enable;
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_trace_clear(ez80_emulator_t *emu)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    emu->hw.trace_head = 0u;
    emu->hw.trace_count = 0u;
    memset(emu->hw.trace_buf, 0, sizeof(emu->hw.trace_buf));
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_trace_get_entry(const ez80_emulator_t *emu,
                                             uint32_t index,
                                             ez80_trace_entry_t *out_entry)
{
    uint32_t actual_idx;
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!out_entry) return EZ80_ERR_INVALID_ARG;
    if (index >= emu->hw.trace_count) return EZ80_ERR_OUT_OF_BOUNDS;

    /* Index 0 = oldest entry in the ring buffer */
    if (emu->hw.trace_count < EZ80_TRACE_BUFFER_SIZE) {
        actual_idx = index;
    } else {
        actual_idx = (emu->hw.trace_head + index) % EZ80_TRACE_BUFFER_SIZE;
    }
    *out_entry = emu->hw.trace_buf[actual_idx];
    return EZ80_OK;
}

ez80_status_t ez80_emulator_trace_get_count(const ez80_emulator_t *emu,
                                             uint32_t *out_count)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (out_count) *out_count = emu->hw.trace_count;
    return EZ80_OK;
}

/* ---- Memory snapshot/restore API ---- */

static uint32_t g_snapshot_counter = 0u;

ez80_status_t ez80_emulator_snapshot_save(const ez80_emulator_t *emu,
                                           ez80_snapshot_t *out_snap)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!out_snap) return EZ80_ERR_INVALID_ARG;

    out_snap->cpu = emu->cpu;
    memcpy(out_snap->ram, emu->memory.ram, EZ80_RAM_BYTES);
    memcpy(out_snap->ports, emu->memory.ports, EZ80_PORT_BYTES);
    memcpy(out_snap->port_space, emu->memory.port_space, EZ80_PORT_SPACE_BYTES);
    out_snap->snapshot_id = ++g_snapshot_counter;
    return EZ80_OK;
}

ez80_status_t ez80_emulator_snapshot_restore(ez80_emulator_t *emu,
                                              const ez80_snapshot_t *snap)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!snap) return EZ80_ERR_INVALID_ARG;
    if (snap->snapshot_id == 0u) return EZ80_ERR_INVALID_ARG;

    emu->cpu = snap->cpu;
    memcpy(emu->memory.ram, snap->ram, EZ80_RAM_BYTES);
    memcpy(emu->memory.ports, snap->ports, EZ80_PORT_BYTES);
    memcpy(emu->memory.port_space, snap->port_space, EZ80_PORT_SPACE_BYTES);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

/* ---- RTC Alarm API ---- */
ez80_status_t ez80_emulator_rtc_alarm_set(ez80_emulator_t *emu,
                                           uint8_t hours,
                                           uint8_t minutes,
                                           uint8_t seconds)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (hours >= 24u || minutes >= 60u || seconds >= 60u)
        return EZ80_ERR_INVALID_ARG;

    emu->hw.rtc_alarm_hours = hours;
    emu->hw.rtc_alarm_minutes = minutes;
    emu->hw.rtc_alarm_seconds = seconds;
    emu->hw.rtc_alarm_fired = false;
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_rtc_alarm_enable(ez80_emulator_t *emu, bool enable)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    if (enable) {
        emu->hw.rtc_alarm_control |= EZ80_RTC_ALARM_CONTROL_ENABLE;
    } else {
        emu->hw.rtc_alarm_control &= (uint8_t)~EZ80_RTC_ALARM_CONTROL_ENABLE;
        emu->hw.rtc_alarm_fired = false;
    }
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_rtc_alarm_get_status(const ez80_emulator_t *emu,
                                                  bool *out_fired)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!out_fired) return EZ80_ERR_INVALID_ARG;
    *out_fired = emu->hw.rtc_alarm_fired;
    return EZ80_OK;
}

/* ---- Hardware Watchdog Timer API ---- */
ez80_status_t ez80_emulator_wdt_configure(ez80_emulator_t *emu,
                                           uint8_t control,
                                           uint16_t timeout)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (timeout == 0u) return EZ80_ERR_INVALID_ARG;

    emu->hw.wdt_control = control;
    emu->hw.wdt_timeout = timeout;
    emu->hw.wdt_counter = 0u;
    emu->hw.wdt_prescaler_accum = 0u;
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_wdt_feed(ez80_emulator_t *emu)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;

    emu->hw.wdt_counter = 0u;
    emu->hw.wdt_prescaler_accum = 0u;
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_wdt_get_counter(const ez80_emulator_t *emu,
                                             uint16_t *out_counter)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!out_counter) return EZ80_ERR_INVALID_ARG;
    *out_counter = emu->hw.wdt_counter;
    return EZ80_OK;
}

/* ---- Power Management API ---- */
ez80_status_t ez80_emulator_pm_set_mode(ez80_emulator_t *emu, uint8_t mode)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (mode > EZ80_PM_MODE_SLEEP) return EZ80_ERR_INVALID_ARG;

    pm_apply_mode_state(emu, mode);
    hardware_sync_mmio(emu);
    return EZ80_OK;
}

ez80_status_t ez80_emulator_pm_get_mode(const ez80_emulator_t *emu,
                                         uint8_t *out_mode)
{
    if (!emu) return EZ80_ERR_INVALID_ARG;
    if (!emu->initialized) return EZ80_ERR_NOT_INITIALIZED;
    if (!out_mode) return EZ80_ERR_INVALID_ARG;
    *out_mode = emu->hw.pm_mode;
    return EZ80_OK;
}
