/* ez80_8xp.c -- TI-84 Plus CE .8xp file format parser
 *
 * Binary format (little-endian throughout):
 *
 *   Offset  Size  Description
 *   ------  ----  -----------
 *   0x00    8     Signature: "**TI83F*"
 *   0x08    3     Extra signature: 0x1A 0x0A 0x00
 *   0x0B    42    Comment (padded with 0x00)
 *   0x35    2     Data section length (total bytes of all var entries)
 *   0x37    ...   Variable entries (one or more)
 *   end-2   2     Checksum (sum of all bytes in data section, mod 0x10000)
 *
 * Each variable entry:
 *   Offset  Size  Description
 *   ------  ----  -----------
 *   0x00    2     Entry header constant (0x0D 0x00 for TI-83/84)
 *   0x02    2     Data length (of the data portion, after this header)
 *   0x04    1     Variable type ID
 *   0x05    8     Variable name (zero-padded)
 *   0x0D    1     Version byte
 *   0x0E    1     Flag byte (bit 7 = archived)
 *   0x0F    2     Data length again (same as offset 0x02)
 *   0x11    ...   Variable data
 *
 * For programs (type 0x05/0x06), the variable data starts with a
 * 2-byte little-endian size prefix giving the actual machine code size,
 * followed by that many bytes of eZ80 machine code.
 */

#include "../include/ez80_8xp.h"
#include "../include/ez80_emulator.h"

#include <stdio.h>
#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint16_t compute_checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0u;
    size_t i;
    for (i = 0; i < length; i++) {
        sum += data[i];
    }
    return (uint16_t)(sum & 0xFFFFu);
}

ez80_status_t ez80_8xp_parse(const uint8_t *data,
                              size_t size,
                              ez80_8xp_parsed_t *out)
{
    size_t offset;
    uint16_t data_section_length;
    size_t data_section_start;
    size_t data_section_end;
    uint32_t var_count;

    if (!data || !out) return EZ80_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->total_file_size = (uint32_t)size;

    if (size < EZ80_8XP_MIN_FILE_SIZE) {
        return EZ80_ERR_ROM_FORMAT;
    }

    /* Validate 8-byte signature */
    if (memcmp(data, EZ80_8XP_SIGNATURE, EZ80_8XP_SIGNATURE_LEN) != 0) {
        return EZ80_ERR_ROM_FORMAT;
    }

    /* Validate extra signature bytes */
    if (data[8] != EZ80_8XP_SIG_BYTE_8 ||
        data[9] != EZ80_8XP_SIG_BYTE_9 ||
        data[10] != EZ80_8XP_SIG_BYTE_10) {
        return EZ80_ERR_ROM_FORMAT;
    }

    /* Parse header */
    memcpy(out->header.signature, data, 8u);
    out->header.signature[8] = '\0';
    memcpy(out->header.sig_extra, data + 8u, 3u);
    memcpy(out->header.comment, data + 11u, 42u);
    out->header.comment[41] = '\0';
    out->header.data_length = le16(data + 53u);
    data_section_length = out->header.data_length;

    /* Validate data section length fits in file (need +2 for trailing checksum) */
    data_section_start = EZ80_8XP_HEADER_SIZE;
    data_section_end = data_section_start + (size_t)data_section_length;
    if (data_section_end + 2u > size) {
        return EZ80_ERR_ROM_FORMAT;
    }

    /* Parse checksum */
    out->checksum_stored = le16(data + data_section_end);
    out->checksum_computed = compute_checksum(data + data_section_start,
                                              (size_t)data_section_length);
    out->checksum_valid = (out->checksum_stored == out->checksum_computed);

    /* Parse variable entries */
    offset = data_section_start;
    var_count = 0u;

    while (offset < data_section_end && var_count < EZ80_8XP_MAX_VAR_ENTRIES) {
        ez80_8xp_var_entry_t *entry = &out->entries[var_count];
        uint16_t var_data_len;
        size_t var_data_start;

        /* Need at least the variable entry header */
        if (offset + EZ80_8XP_VAR_ENTRY_HEADER_SIZE > data_section_end) {
            break;
        }

        /* Skip 2-byte entry header constant (0x0D, 0x00) */
        offset += 2u;

        /* Data length */
        entry->entry_data_length = le16(data + offset);
        offset += 2u;
        var_data_len = entry->entry_data_length;

        /* Type ID */
        entry->type_id = data[offset];
        offset += 1u;

        /* 8-byte name */
        memcpy(entry->name, data + offset, 8u);
        entry->name[8] = '\0';
        offset += 8u;

        /* Version */
        entry->version = data[offset];
        offset += 1u;

        /* Flags */
        entry->flags = data[offset];
        offset += 1u;

        /* Second data length field */
        entry->data_length2 = le16(data + offset);
        offset += 2u;

        /* Variable data starts here */
        var_data_start = offset;

        /* Validate we have enough data */
        if (var_data_start + var_data_len > data_section_end) {
            break;
        }

        /* For programs, extract the 2-byte size prefix + machine code */
        if (entry->type_id == EZ80_8XP_TYPE_PROGRAM ||
            entry->type_id == EZ80_8XP_TYPE_PROT_PROGRAM ||
            entry->type_id == EZ80_8XP_TYPE_APPVAR) {
            if (var_data_len >= 2u) {
                entry->program_size = le16(data + var_data_start);
                entry->program_data = data + var_data_start + 2u;
                entry->program_data_size = (var_data_len >= 2u)
                    ? (uint32_t)(var_data_len - 2u) : 0u;
            } else {
                entry->program_size = 0u;
                entry->program_data = data + var_data_start;
                entry->program_data_size = 0u;
            }
        } else {
            /* Non-program types: data is the raw variable data */
            entry->program_size = 0u;
            entry->program_data = data + var_data_start;
            entry->program_data_size = var_data_len;
        }

        offset = var_data_start + (size_t)var_data_len;
        var_count++;
    }

    out->var_entry_count = var_count;

    if (var_count == 0u) {
        return EZ80_ERR_ROM_FORMAT;
    }

    return EZ80_OK;
}

ez80_status_t ez80_8xp_load_file(const char *path,
                                  uint8_t *buffer,
                                  size_t buffer_capacity,
                                  size_t *out_bytes_read)
{
    FILE *f;
    long file_size;
    size_t nread;

    if (!path || !buffer || !out_bytes_read) return EZ80_ERR_INVALID_ARG;
    *out_bytes_read = 0u;

    f = fopen(path, "rb");
    if (!f) return EZ80_ERR_ROM_FORMAT;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return EZ80_ERR_ROM_FORMAT;
    }
    file_size = ftell(f);
    if (file_size < 0 || (size_t)file_size > buffer_capacity) {
        fclose(f);
        return (file_size < 0) ? EZ80_ERR_ROM_FORMAT : EZ80_ERR_OUT_OF_BOUNDS;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return EZ80_ERR_ROM_FORMAT;
    }

    nread = fread(buffer, 1u, (size_t)file_size, f);
    if (ferror(f) || (long)nread != file_size) {
        fclose(f);
        return EZ80_ERR_ROM_FORMAT;
    }
    fclose(f);

    *out_bytes_read = nread;
    return EZ80_OK;
}

ez80_status_t ez80_8xp_load_into_emulator(ez80_emulator_t *emu,
                                           const ez80_8xp_parsed_t *parsed,
                                           uint32_t var_index,
                                           bool archived)
{
    const ez80_8xp_var_entry_t *entry;
    uint8_t os_type;

    if (!emu || !parsed) return EZ80_ERR_INVALID_ARG;
    if (var_index >= parsed->var_entry_count) return EZ80_ERR_INVALID_ARG;

    entry = &parsed->entries[var_index];

    /* Map .8xp type to emulator OS var type */
    switch (entry->type_id) {
    case EZ80_8XP_TYPE_PROGRAM:
    case EZ80_8XP_TYPE_PROT_PROGRAM:
        os_type = EZ80_OS_VAR_PROGRAM;
        break;
    case EZ80_8XP_TYPE_APPVAR:
        os_type = EZ80_OS_VAR_STRING;  /* AppVars stored as string type in var table */
        break;
    case EZ80_8XP_TYPE_REAL:
        os_type = EZ80_OS_VAR_REAL;
        break;
    case EZ80_8XP_TYPE_MATRIX:
        os_type = EZ80_OS_VAR_MATRIX;
        break;
    case EZ80_8XP_TYPE_REAL_LIST:
        os_type = EZ80_OS_VAR_LIST;
        break;
    case EZ80_8XP_TYPE_STRING:
        os_type = EZ80_OS_VAR_STRING;
        break;
    case EZ80_8XP_TYPE_COMPLEX:
        os_type = EZ80_OS_VAR_COMPLEX;
        break;
    default:
        os_type = EZ80_OS_VAR_PROGRAM;
        break;
    }

    /* For programs, load the machine code (without the 2-byte size prefix).
     * For other types, load the raw variable data. */
    if (entry->program_data && entry->program_data_size > 0u) {
        return ez80_emulator_os_var_write(emu,
                                          entry->name,
                                          os_type,
                                          entry->program_data,
                                          (size_t)entry->program_data_size,
                                          archived);
    }

    return EZ80_ERR_INVALID_ARG;
}

const char *ez80_8xp_type_name(uint8_t type_id)
{
    switch (type_id) {
    case EZ80_8XP_TYPE_REAL:          return "Real";
    case EZ80_8XP_TYPE_REAL_LIST:     return "RealList";
    case EZ80_8XP_TYPE_MATRIX:        return "Matrix";
    case EZ80_8XP_TYPE_EQUATION:      return "Equation";
    case EZ80_8XP_TYPE_STRING:        return "String";
    case EZ80_8XP_TYPE_PROGRAM:       return "Program";
    case EZ80_8XP_TYPE_PROT_PROGRAM:  return "ProtectedProgram";
    case EZ80_8XP_TYPE_PICTURE:       return "Picture";
    case EZ80_8XP_TYPE_GDB:           return "GraphDB";
    case EZ80_8XP_TYPE_COMPLEX:       return "Complex";
    case EZ80_8XP_TYPE_COMPLEX_LIST:  return "ComplexList";
    case EZ80_8XP_TYPE_APPVAR:        return "AppVar";
    case EZ80_8XP_TYPE_GROUP:         return "Group";
    default:                          return "Unknown";
    }
}
