#include "../include/ez80_rom.h"

#include <stdio.h>
#include <string.h>

static uint32_t crc32_stub(const uint8_t *data, size_t size)
{
    uint32_t crc = 0u;
    size_t i;
    for (i = 0; i < size; i++) {
        crc = (crc << 5) ^ (crc >> 27) ^ data[i];
    }
    return crc;
}

ez80_status_t ez80_rom_probe_header(const uint8_t *rom_data,
                                    size_t rom_size,
                                    ez80_rom_metadata_t *out)
{
    if (!rom_data || !out) return EZ80_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->file_size_bytes = rom_size;
    out->crc32 = crc32_stub(rom_data, rom_size);
    if (rom_size < 16u) {
        out->has_valid_header = false;
        return EZ80_ERR_ROM_FORMAT;
    }
    out->has_valid_header = true;
    return EZ80_ERR_NOT_IMPLEMENTED;
}

ez80_status_t ez80_rom_load_file_metadata(const char *rom_path,
                                          ez80_rom_metadata_t *out)
{
    FILE *f;
    uint8_t header[32];
    size_t nread;
    long file_size;
    ez80_rom_metadata_t probe;
    ez80_status_t status;
    if (!rom_path || rom_path[0] == '\0' || !out) return EZ80_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    f = fopen(rom_path, "rb");
    if (!f) return EZ80_ERR_ROM_FORMAT;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return EZ80_ERR_ROM_FORMAT;
    }
    file_size = ftell(f);
    if (file_size < 0) {
        fclose(f);
        return EZ80_ERR_ROM_FORMAT;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return EZ80_ERR_ROM_FORMAT;
    }
    nread = fread(header, 1u, sizeof(header), f);
    if (ferror(f)) {
        fclose(f);
        return EZ80_ERR_ROM_FORMAT;
    }
    fclose(f);
    memset(&probe, 0, sizeof(probe));
    status = ez80_rom_probe_header(header, nread, &probe);

    out->file_size_bytes = (size_t)file_size;
    out->crc32 = probe.crc32;
    out->has_valid_header = probe.has_valid_header;

    return status;
}
