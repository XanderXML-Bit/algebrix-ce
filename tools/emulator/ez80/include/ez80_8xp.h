/* ez80_8xp.h -- TI-84 Plus CE .8xp file format parser
 *
 * Parses the binary .8xp format used by TI-84 Plus CE programs:
 *   [55-byte header] [variable entries...] [2-byte checksum]
 *
 * Each variable entry:
 *   [2-byte data length] [1-byte type] [8-byte name] [1-byte version]
 *   [1-byte flags] [2-byte data length again] [data...]
 *
 * The program data inside the variable entry starts with a 2-byte size
 * prefix (little-endian) followed by the actual machine code.
 */

#ifndef EZ80_8XP_H
#define EZ80_8XP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ez80_emulator.h"

/* .8xp file header is 55 bytes */
#define EZ80_8XP_HEADER_SIZE 55u

/* Variable entry header (before data) is 17 bytes */
#define EZ80_8XP_VAR_ENTRY_HEADER_SIZE 17u

/* Minimum valid .8xp file: header + var entry header + 2-byte size + 2-byte checksum */
#define EZ80_8XP_MIN_FILE_SIZE (EZ80_8XP_HEADER_SIZE + EZ80_8XP_VAR_ENTRY_HEADER_SIZE + 2u + 2u)

/* 8-byte signature at offset 0: "**TI83F*" */
#define EZ80_8XP_SIGNATURE "**TI83F*"
#define EZ80_8XP_SIGNATURE_LEN 8u

/* Further signature at offset 8: 0x1A 0x0A 0x00 */
#define EZ80_8XP_SIG_BYTE_8 0x1Au
#define EZ80_8XP_SIG_BYTE_9 0x0Au
#define EZ80_8XP_SIG_BYTE_10 0x00u

/* Variable type codes */
#define EZ80_8XP_TYPE_REAL 0x00u
#define EZ80_8XP_TYPE_REAL_LIST 0x01u
#define EZ80_8XP_TYPE_MATRIX 0x02u
#define EZ80_8XP_TYPE_EQUATION 0x03u
#define EZ80_8XP_TYPE_STRING 0x04u
#define EZ80_8XP_TYPE_PROGRAM 0x05u
#define EZ80_8XP_TYPE_PROT_PROGRAM 0x06u
#define EZ80_8XP_TYPE_PICTURE 0x07u
#define EZ80_8XP_TYPE_GDB 0x08u
#define EZ80_8XP_TYPE_COMPLEX 0x0Cu
#define EZ80_8XP_TYPE_COMPLEX_LIST 0x0Du
#define EZ80_8XP_TYPE_APPVAR 0x15u
#define EZ80_8XP_TYPE_GROUP 0x17u

/* Flag bits in variable entry */
#define EZ80_8XP_FLAG_ARCHIVED 0x80u

/* Maximum program data size we'll accept (4MB flash) */
#define EZ80_8XP_MAX_DATA_SIZE (4u * 1024u * 1024u)

/* Maximum number of variable entries in a single .8xp file */
#define EZ80_8XP_MAX_VAR_ENTRIES 16u

/* Parsed .8xp file header */
typedef struct {
    char signature[9];       /* "**TI83F*\0" */
    uint8_t sig_extra[3];    /* 0x1A, 0x0A, 0x00 */
    char comment[42];        /* 42-byte comment field (offset 11) */
    uint16_t data_length;    /* total length of variable data section (offset 53) */
} ez80_8xp_header_t;

/* Single variable entry from .8xp file */
typedef struct {
    uint16_t entry_data_length;  /* 2 bytes: length of data + name header */
    uint8_t type_id;             /* 1 byte: variable type */
    char name[9];                /* 8 bytes + null terminator */
    uint8_t version;             /* 1 byte: version / flags */
    uint8_t flags;               /* 1 byte: bit 7 = archived */
    uint16_t data_length2;       /* 2 bytes: repeat of data length */
    uint16_t program_size;       /* 2-byte size prefix inside program data */
    const uint8_t *program_data; /* pointer into original buffer (after size prefix) */
    uint32_t program_data_size;  /* actual machine code byte count */
} ez80_8xp_var_entry_t;

/* Fully parsed .8xp file */
typedef struct {
    ez80_8xp_header_t header;
    uint32_t var_entry_count;
    ez80_8xp_var_entry_t entries[EZ80_8XP_MAX_VAR_ENTRIES];
    uint16_t checksum_stored;    /* checksum from file */
    uint16_t checksum_computed;  /* checksum we computed */
    bool checksum_valid;
    uint32_t total_file_size;
} ez80_8xp_parsed_t;

/* Parse a .8xp file from a memory buffer.
 * The buffer must remain valid as long as parsed->entries[].program_data
 * pointers are used (they point into the buffer). */
ez80_status_t ez80_8xp_parse(const uint8_t *data,
                              size_t size,
                              ez80_8xp_parsed_t *out);

/* Load a .8xp file from disk and parse it. Caller provides the buffer.
 * Returns the number of bytes read in *out_bytes_read. */
ez80_status_t ez80_8xp_load_file(const char *path,
                                  uint8_t *buffer,
                                  size_t buffer_capacity,
                                  size_t *out_bytes_read);

/* Load a parsed .8xp program into the emulator.
 * Copies program data into emulator memory (archive or RAM)
 * and registers it as an OS variable. */
ez80_status_t ez80_8xp_load_into_emulator(ez80_emulator_t *emu,
                                           const ez80_8xp_parsed_t *parsed,
                                           uint32_t var_index,
                                           bool archived);

/* Get a human-readable type name for a variable type ID. */
const char *ez80_8xp_type_name(uint8_t type_id);

#endif /* EZ80_8XP_H */
