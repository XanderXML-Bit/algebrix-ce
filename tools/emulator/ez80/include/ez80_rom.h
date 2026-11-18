#ifndef EZ80_ROM_H
#define EZ80_ROM_H

#include <stddef.h>
#include <stdint.h>

#include "ez80_emulator.h"

typedef struct {
    size_t file_size_bytes;
    uint32_t crc32;
    bool has_valid_header;
} ez80_rom_metadata_t;

ez80_status_t ez80_rom_probe_header(const uint8_t *rom_data,
                                    size_t rom_size,
                                    ez80_rom_metadata_t *out);

ez80_status_t ez80_rom_load_file_metadata(const char *rom_path,
                                          ez80_rom_metadata_t *out);

#endif /* EZ80_ROM_H */
