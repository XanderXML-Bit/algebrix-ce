#include "../include/ez80_decode.h"

#include <stdio.h>
#include <string.h>

static const char *g_reg8_names[8] = {
    "B", "C", "D", "E", "H", "L", "(HL)", "A"
};

static const char *g_reg16_names[4] = {
    "BC", "DE", "HL", "SP"
};

static const char *g_pushpop_names[4] = {
    "BC", "DE", "HL", "AF"
};

static const char *g_alu_names[8] = {
    "ADD", "ADC", "SUB", "SBC", "AND", "XOR", "OR", "CP"
};

static const char *g_cond_names[8] = {
    "NZ", "Z", "NC", "C", "PO", "PE", "P", "M"
};

static const char *g_cb_rot_names[8] = {
    "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"
};

static void copy_token(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0u) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1u);
    dst[cap - 1u] = '\0';
}

static uint16_t read_u16(const uint8_t *stream)
{
    return (uint16_t)stream[0] | ((uint16_t)stream[1] << 8);
}

static void format_index_disp(char *dst, size_t cap, const char *index_name, int8_t disp)
{
    if (!dst || cap == 0u || !index_name) return;
    (void)snprintf(dst, cap, "(%s%+d)", index_name, (int)disp);
}

static ez80_status_t decode_cb_prefix(const uint8_t *stream,
                                      size_t stream_size,
                                      ez80_instruction_t *out)
{
    uint8_t op;
    uint8_t x;
    uint8_t y;
    uint8_t z;

    if (stream_size < 2u) {
        copy_token(out->mnemonic, sizeof(out->mnemonic), "CB");
        out->operands[0] = '\0';
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    op = stream[1];
    out->bytes[1] = op;
    out->length = 2u;
    out->cycles = 8u;

    x = (uint8_t)(op >> 6);
    y = (uint8_t)((op >> 3) & 0x07u);
    z = (uint8_t)(op & 0x07u);

    switch (x) {
    case 0u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), g_cb_rot_names[y]);
        copy_token(out->operands, sizeof(out->operands), g_reg8_names[z]);
        out->cycles = (z == 6u) ? 15u : 8u;
        return EZ80_OK;

    case 1u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "BIT");
        (void)snprintf(out->operands, sizeof(out->operands), "%u,%s",
                       (unsigned)y, g_reg8_names[z]);
        out->cycles = (z == 6u) ? 12u : 8u;
        return EZ80_OK;

    case 2u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RES");
        (void)snprintf(out->operands, sizeof(out->operands), "%u,%s",
                       (unsigned)y, g_reg8_names[z]);
        out->cycles = (z == 6u) ? 15u : 8u;
        return EZ80_OK;

    default:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "SET");
        (void)snprintf(out->operands, sizeof(out->operands), "%u,%s",
                       (unsigned)y, g_reg8_names[z]);
        out->cycles = (z == 6u) ? 15u : 8u;
        return EZ80_OK;
    }
}

static ez80_status_t decode_ed_prefix(const uint8_t *stream,
                                      size_t stream_size,
                                      ez80_instruction_t *out)
{
    uint8_t op;

    if (stream_size < 2u) {
        copy_token(out->mnemonic, sizeof(out->mnemonic), "ED");
        out->operands[0] = '\0';
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    op = stream[1];
    out->bytes[1] = op;
    out->length = 2u;
    out->cycles = 8u;

    switch (op) {

    /* --- eZ80: IN0 r,(n) — 3-byte instructions --- */
    case 0x00u:
    case 0x08u:
    case 0x10u:
    case 0x18u:
    case 0x20u:
    case 0x28u:
    case 0x38u: {
        uint8_t r = (uint8_t)((op >> 3) & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "IN0");
        out->cycles = 12u;
        if (stream_size < 3u) {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,(n8)", g_reg8_names[r]);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "%s,(0x%02X)", g_reg8_names[r], stream[2]);
        return EZ80_OK;
    }

    case 0x30u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "IY,(HL)");
        out->cycles = 10u;
        return EZ80_OK;

    /* --- eZ80: OUT0 (n),r — 3-byte instructions --- */
    case 0x01u:
    case 0x09u:
    case 0x11u:
    case 0x19u:
    case 0x21u:
    case 0x29u:
    case 0x39u: {
        uint8_t r = (uint8_t)((op >> 3) & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OUT0");
        out->cycles = 12u;
        if (stream_size < 3u) {
            (void)snprintf(out->operands, sizeof(out->operands), "(n8),%s", g_reg8_names[r]);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "(0x%02X),%s", stream[2], g_reg8_names[r]);
        return EZ80_OK;
    }

    /* Undocumented alias retained for compatibility. */
    case 0x31u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OUT0");
        out->cycles = 12u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "(n8),0");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "(0x%02X),0", stream[2]);
        return EZ80_OK;

    /* --- eZ80: LEA rr,IX+d / LEA rr,IY+d --- */
    case 0x02u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "BC,IX+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "BC,IX%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    case 0x03u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "BC,IY+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "BC,IY%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    case 0x12u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "DE,IX+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "DE,IX%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    case 0x13u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "DE,IY+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "DE,IY%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    case 0x22u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "HL,IX+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "HL,IX%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    case 0x23u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "HL,IY+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "HL,IY%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    /* eZ80 pair load/store through (HL) */
    case 0x06u:
    case 0x07u:
    case 0x16u:
    case 0x17u:
    case 0x26u:
    case 0x27u: {
        uint8_t ss = (uint8_t)(op >> 4);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        (void)snprintf(out->operands, sizeof(out->operands), "%s,(HL)", g_reg16_names[ss]);
        out->cycles = 10u;
        return EZ80_OK;
    }

    case 0x0Eu:
    case 0x0Fu:
    case 0x1Eu:
    case 0x1Fu:
    case 0x2Eu:
    case 0x2Fu: {
        uint8_t ss = (uint8_t)(op >> 4);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        (void)snprintf(out->operands, sizeof(out->operands), "(HL),%s", g_reg16_names[ss]);
        out->cycles = 10u;
        return EZ80_OK;
    }

    case 0x37u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "IX,(HL)");
        out->cycles = 10u;
        return EZ80_OK;

    case 0x3Eu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "(HL),IX");
        out->cycles = 10u;
        return EZ80_OK;

    case 0x3Fu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "(HL),IY");
        out->cycles = 10u;
        return EZ80_OK;

    /* Legacy aliases retained for backward-compatibility with prior scaffolds. */
    case 0x0Au:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "BC,IY+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "BC,IY%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    case 0x0Bu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "DE,IY+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "DE,IY%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    case 0x32u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "IX,IX+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "IX,IX%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    case 0x33u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "IY,IY+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "IY,IY%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    case 0x3Au:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "HL,IY+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "HL,IY%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    case 0x3Bu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "IY,IY+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "IY,IY%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    /* --- eZ80: TST A,r — ED 04+r*8 --- */
    case 0x04u:
    case 0x0Cu:
    case 0x14u:
    case 0x1Cu:
    case 0x24u:
    case 0x2Cu:
    case 0x3Cu: {
        uint8_t r = (uint8_t)((op >> 3) & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "TST");
        (void)snprintf(out->operands, sizeof(out->operands), "A,%s", g_reg8_names[r]);
        out->cycles = 8u;
        return EZ80_OK;
    }

    /* TST A,(HL) — ED 34 */
    case 0x34u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "TST");
        copy_token(out->operands, sizeof(out->operands), "A,(HL)");
        out->cycles = 11u;
        return EZ80_OK;

    /* --- eZ80: TST A,n (ED 64) --- */
    case 0x64u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "TST");
        out->cycles = 11u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "A,n8");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "A,0x%02X", stream[2]);
        return EZ80_OK;

    /* --- eZ80: PEA IX+d (ED 65) --- */
    case 0x65u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "PEA");
        out->cycles = 15u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "IX+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "IX%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    /* --- eZ80: PEA IY+d (ED 66) --- */
    case 0x66u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "PEA");
        out->cycles = 15u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "IY+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "IY%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    /* --- eZ80: TSTIO n (ED 74) --- */
    case 0x74u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "TSTIO");
        out->cycles = 12u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "n8");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "0x%02X", stream[2]);
        return EZ80_OK;

    /* --- eZ80: SLP (ED 76) --- */
    case 0x76u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "SLP");
        out->operands[0] = '\0';
        out->cycles = 8u;
        return EZ80_OK;

    /* Undocumented ED-prefixed NOP aliases */
    case 0x77u:
    case 0x7Fu:
    case 0xFFu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "NOP");
        out->operands[0] = '\0';
        out->cycles = 8u;
        return EZ80_OK;

    /*
     * Compatibility no-op aliases for currently undefined ED slots in this
     * scaffold. Keep decode/execute aligned and deterministic.
     */
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
        copy_token(out->mnemonic, sizeof(out->mnemonic), "NOP");
        out->operands[0] = '\0';
        out->cycles = 8u;
        return EZ80_OK;

    /* --- eZ80: LEA IX,IY+d (ED 54) --- */
    case 0x54u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "IX,IY+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "IX,IY%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    /* --- eZ80: LEA IY,IX+d (ED 55) --- */
    case 0x55u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LEA");
        out->cycles = 6u;
        if (stream_size < 3u) {
            copy_token(out->operands, sizeof(out->operands), "IY,IX+d");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "IY,IX%+d", (int)(int8_t)stream[2]);
        return EZ80_OK;

    /* NEG (canonical) */
    case 0x44u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "NEG");
        out->operands[0] = '\0';
        out->cycles = 8u;
        return EZ80_OK;

    case 0x45u:
    case 0x75u:
    case 0x5Du:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RETN");
        out->operands[0] = '\0';
        out->cycles = 14u;
        return EZ80_OK;

    case 0x4Du:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RETI");
        out->operands[0] = '\0';
        out->cycles = 14u;
        return EZ80_OK;

    case 0x47u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "I,A");
        out->cycles = 9u;
        return EZ80_OK;

    case 0x4Fu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "R,A");
        out->cycles = 9u;
        return EZ80_OK;

    case 0x57u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "A,I");
        out->cycles = 9u;
        return EZ80_OK;

    case 0x5Fu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "A,R");
        out->cycles = 9u;
        return EZ80_OK;

    case 0x46u:
    case 0x4Eu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "IM");
        copy_token(out->operands, sizeof(out->operands), "0");
        out->cycles = 8u;
        return EZ80_OK;

    case 0x56u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "IM");
        copy_token(out->operands, sizeof(out->operands), "1");
        out->cycles = 8u;
        return EZ80_OK;

    case 0x5Eu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "IM");
        copy_token(out->operands, sizeof(out->operands), "2");
        out->cycles = 8u;
        return EZ80_OK;

    case 0xA0u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LDI");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0xA8u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LDD");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0xB0u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LDIR");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xB8u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LDDR");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xA1u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "CPI");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0xA9u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "CPD");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0xB1u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "CPIR");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xB9u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "CPDR");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xA2u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INI");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0xAAu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "IND");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0xB2u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INIR");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xBAu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INDR");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xA3u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OUTI");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0xABu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OUTD");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0xB3u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OUTIR");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xBBu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OUTDR");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0x40u:
    case 0x48u:
    case 0x50u:
    case 0x58u:
    case 0x60u:
    case 0x68u:
    case 0x78u: {
        uint8_t r = (uint8_t)((op >> 3) & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "IN");
        (void)snprintf(out->operands, sizeof(out->operands), "%s,(C)", g_reg8_names[r]);
        out->cycles = 12u;
        return EZ80_OK;
    }

    case 0x70u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "IN");
        copy_token(out->operands, sizeof(out->operands), "F,(C)");
        out->cycles = 12u;
        return EZ80_OK;

    case 0x41u:
    case 0x49u:
    case 0x51u:
    case 0x59u:
    case 0x61u:
    case 0x69u:
    case 0x79u: {
        uint8_t r = (uint8_t)((op >> 3) & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OUT");
        (void)snprintf(out->operands, sizeof(out->operands), "(C),%s", g_reg8_names[r]);
        out->cycles = 12u;
        return EZ80_OK;
    }

    case 0x71u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OUT");
        copy_token(out->operands, sizeof(out->operands), "(C),0");
        out->cycles = 12u;
        return EZ80_OK;

    case 0x42u:
    case 0x52u:
    case 0x62u:
    case 0x72u: {
        uint8_t ss = (uint8_t)((op >> 4) & 0x03u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "SBC");
        (void)snprintf(out->operands, sizeof(out->operands), "HL,%s", g_reg16_names[ss]);
        out->cycles = 15u;
        return EZ80_OK;
    }

    case 0x43u:
    case 0x53u:
    case 0x63u:
    case 0x73u: {
        uint8_t ss = (uint8_t)((op >> 4) & 0x03u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        out->cycles = 20u;
        if (stream_size < 4u) {
            (void)snprintf(out->operands, sizeof(out->operands), "(nn),%s", g_reg16_names[ss]);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->bytes[3] = stream[3];
        out->length = 4u;
        (void)snprintf(out->operands, sizeof(out->operands), "(0x%04X),%s",
                       read_u16(&stream[2]), g_reg16_names[ss]);
        return EZ80_OK;
    }

    case 0x4Au:
    case 0x5Au:
    case 0x6Au:
    case 0x7Au: {
        uint8_t ss = (uint8_t)((op >> 4) & 0x03u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "ADC");
        (void)snprintf(out->operands, sizeof(out->operands), "HL,%s", g_reg16_names[ss]);
        out->cycles = 15u;
        return EZ80_OK;
    }

    case 0x4Bu:
    case 0x5Bu:
    case 0x6Bu:
    case 0x7Bu: {
        uint8_t ss = (uint8_t)((op >> 4) & 0x03u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        out->cycles = 20u;
        if (stream_size < 4u) {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,(nn)", g_reg16_names[ss]);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->bytes[3] = stream[3];
        out->length = 4u;
        (void)snprintf(out->operands, sizeof(out->operands), "%s,(0x%04X)",
                       g_reg16_names[ss], read_u16(&stream[2]));
        return EZ80_OK;
    }

    case 0x4Cu:
    case 0x5Cu:
    case 0x6Cu:
    case 0x7Cu: {
        uint8_t ss = (uint8_t)((op >> 4) & 0x03u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "MLT");
        copy_token(out->operands, sizeof(out->operands), g_reg16_names[ss]);
        out->cycles = 6u;
        return EZ80_OK;
    }

    case 0x6Du:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "MB,A");
        out->cycles = 9u;
        return EZ80_OK;

    case 0x6Eu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "A,MB");
        out->cycles = 9u;
        return EZ80_OK;

    case 0xC7u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "I,HL");
        out->cycles = 9u;
        return EZ80_OK;

    case 0xD7u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "HL,I");
        out->cycles = 9u;
        return EZ80_OK;

    case 0x67u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RRD");
        out->operands[0] = '\0';
        out->cycles = 18u;
        return EZ80_OK;

    case 0x7Du:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "STMIX");
        out->operands[0] = '\0';
        out->cycles = 8u;
        return EZ80_OK;

    case 0x7Eu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RSMIX");
        out->operands[0] = '\0';
        out->cycles = 8u;
        return EZ80_OK;

    case 0x6Fu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RLD");
        out->operands[0] = '\0';
        out->cycles = 18u;
        return EZ80_OK;

    /* --- eZ80: extended block I/O --- */
    case 0x80u:
    case 0x82u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INIM");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0x81u:
    case 0x83u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OTIM");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0x86u:
    case 0x84u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INI2");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0x87u:
    case 0x85u:
    case 0xA6u:
    case 0xA7u:
    case 0xA5u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OTI2");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0xA4u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OUTI2");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0x88u:
    case 0x8Au:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INDM");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0x89u:
    case 0x8Bu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OTDM");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0x8Eu:
    case 0x8Cu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "IND2");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0x8Fu:
    case 0x8Du:
    case 0xAEu:
    case 0xAFu:
    case 0xADu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OTD2");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0xACu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OUTD2");
        out->operands[0] = '\0';
        out->cycles = 16u;
        return EZ80_OK;

    case 0x90u:
    case 0x92u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INIMR");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0x91u:
    case 0x93u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OTIMR");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0x96u:
    case 0x94u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INI2R");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0x97u:
    case 0x95u:
    case 0xB6u:
    case 0xB7u:
    case 0xB5u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OTI2R");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xB4u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OTI2R");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0x98u:
    case 0x9Au:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INDMR");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0x99u:
    case 0x9Bu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OTDMR");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0x9Eu:
    case 0x9Cu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "IND2R");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0x9Fu:
    case 0x9Du:
    case 0xBEu:
    case 0xBFu:
    case 0xBDu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OTD2R");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xBCu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OTD2R");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xC2u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INIRX");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xC3u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OTIRX");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xCAu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INDRX");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    case 0xCBu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "OTDRX");
        out->operands[0] = '\0';
        out->cycles = 21u;
        return EZ80_OK;

    default:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "UNIMPL");
        out->operands[0] = '\0';
        return EZ80_ERR_NOT_IMPLEMENTED;
    }
}

static ez80_status_t decode_ddfd_cb_prefix(const uint8_t *stream,
                                           size_t stream_size,
                                           ez80_instruction_t *out,
                                           const char *index_name)
{
    uint8_t cb_op;
    uint8_t x;
    uint8_t y;
    uint8_t z;
    int8_t disp;
    char disp_text[32];

    if (stream_size < 4u) {
        copy_token(out->mnemonic, sizeof(out->mnemonic), "CB");
        copy_token(out->operands, sizeof(out->operands), index_name);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    disp = (int8_t)stream[2];
    cb_op = stream[3];
    x = (uint8_t)(cb_op >> 6);
    y = (uint8_t)((cb_op >> 3) & 0x07u);
    z = (uint8_t)(cb_op & 0x07u);

    out->bytes[2] = stream[2];
    out->bytes[3] = stream[3];
    out->length = 4u;
    format_index_disp(disp_text, sizeof(disp_text), index_name, disp);

    switch (x) {
    case 0u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), g_cb_rot_names[y]);
        if (z == 6u) {
            copy_token(out->operands, sizeof(out->operands), disp_text);
        } else {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,%s",
                           disp_text, g_reg8_names[z]);
        }
        out->cycles = 23u;
        return EZ80_OK;

    case 1u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "BIT");
        (void)snprintf(out->operands, sizeof(out->operands), "%u,%s",
                       (unsigned)y, disp_text);
        out->cycles = 20u;
        return EZ80_OK;

    case 2u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RES");
        if (z == 6u) {
            (void)snprintf(out->operands, sizeof(out->operands), "%u,%s",
                           (unsigned)y, disp_text);
        } else {
            (void)snprintf(out->operands, sizeof(out->operands), "%u,%s,%s",
                           (unsigned)y, disp_text, g_reg8_names[z]);
        }
        out->cycles = 23u;
        return EZ80_OK;

    default:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "SET");
        if (z == 6u) {
            (void)snprintf(out->operands, sizeof(out->operands), "%u,%s",
                           (unsigned)y, disp_text);
        } else {
            (void)snprintf(out->operands, sizeof(out->operands), "%u,%s,%s",
                           (unsigned)y, disp_text, g_reg8_names[z]);
        }
        out->cycles = 23u;
        return EZ80_OK;
    }
}

static ez80_status_t decode_ddfd_prefix(const uint8_t *stream,
                                        size_t stream_size,
                                        ez80_instruction_t *out,
                                        const char *index_name)
{
    ez80_instruction_t inner;
    uint8_t op;

    if (stream_size < 2u) {
        copy_token(out->mnemonic, sizeof(out->mnemonic), "PREFIX");
        copy_token(out->operands, sizeof(out->operands), index_name);
        return EZ80_ERR_OUT_OF_BOUNDS;
    }

    op = stream[1];
    out->bytes[1] = op;
    out->length = 2u;

    if (op == 0xCBu) {
        return decode_ddfd_cb_prefix(stream, stream_size, out, index_name);
    }

    /* DD/FD prefixes are ignored before ED on eZ80/Z80 compatibility paths. */
    if (op == 0xEDu) {
        ez80_status_t inner_status;
        size_t i;

        if (stream_size < 3u) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), "ED");
            out->operands[0] = '\0';
            return EZ80_ERR_OUT_OF_BOUNDS;
        }

        inner_status = ez80_decode_instruction(&stream[1], stream_size - 1u, &inner);
        if (inner_status == EZ80_ERR_NOT_IMPLEMENTED) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), "UNIMPL");
            out->operands[0] = '\0';
            return EZ80_ERR_NOT_IMPLEMENTED;
        }

        copy_token(out->mnemonic, sizeof(out->mnemonic), inner.mnemonic);
        copy_token(out->operands, sizeof(out->operands), inner.operands);

        out->length = (uint8_t)(inner.length + 1u);
        out->cycles = (uint8_t)(inner.cycles + 4u);
        out->bytes[1] = 0xEDu;
        for (i = 1u; i < inner.length && (i + 1u) < sizeof(out->bytes); i++) {
            out->bytes[i + 1u] = inner.bytes[i];
        }
        return inner_status;
    }

    switch (op) {
    case 0x21u:
        out->cycles = 14u;
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        if (stream_size < 4u) {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,nn", index_name);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->bytes[3] = stream[3];
        out->length = 4u;
        (void)snprintf(out->operands, sizeof(out->operands), "%s,0x%04X",
                       index_name, read_u16(&stream[2]));
        return EZ80_OK;

    case 0x22u:
        out->cycles = 20u;
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        if (stream_size < 4u) {
            copy_token(out->operands, sizeof(out->operands), "(nn),rr");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->bytes[3] = stream[3];
        out->length = 4u;
        (void)snprintf(out->operands, sizeof(out->operands), "(0x%04X),%s",
                       read_u16(&stream[2]), index_name);
        return EZ80_OK;

    case 0x2Au:
        out->cycles = 20u;
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        if (stream_size < 4u) {
            copy_token(out->operands, sizeof(out->operands), "rr,(nn)");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[2] = stream[2];
        out->bytes[3] = stream[3];
        out->length = 4u;
        (void)snprintf(out->operands, sizeof(out->operands), "%s,(0x%04X)",
                       index_name, read_u16(&stream[2]));
        return EZ80_OK;

    case 0x23u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INC");
        copy_token(out->operands, sizeof(out->operands), index_name);
        out->cycles = 10u;
        return EZ80_OK;

    case 0x2Bu:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "DEC");
        copy_token(out->operands, sizeof(out->operands), index_name);
        out->cycles = 10u;
        return EZ80_OK;

    case 0x09u:
    case 0x19u:
    case 0x29u:
    case 0x39u: {
        uint8_t ss = (uint8_t)((op >> 4) & 0x03u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "ADD");
        /* When ss==2, DD/FD prefix replaces HL with the index register */
        (void)snprintf(out->operands, sizeof(out->operands), "%s,%s",
                       index_name, (ss == 2u) ? index_name : g_reg16_names[ss]);
        out->cycles = 15u;
        return EZ80_OK;
    }

    case 0xE1u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "POP");
        copy_token(out->operands, sizeof(out->operands), index_name);
        out->cycles = 14u;
        return EZ80_OK;

    case 0xE3u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "EX");
        (void)snprintf(out->operands, sizeof(out->operands), "(SP),%s", index_name);
        out->cycles = 23u;
        return EZ80_OK;

    case 0xE5u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "PUSH");
        copy_token(out->operands, sizeof(out->operands), index_name);
        out->cycles = 15u;
        return EZ80_OK;

    case 0xE9u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "JP");
        (void)snprintf(out->operands, sizeof(out->operands), "(%s)", index_name);
        out->cycles = 8u;
        return EZ80_OK;

    case 0xF9u:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        (void)snprintf(out->operands, sizeof(out->operands), "SP,%s", index_name);
        out->cycles = 10u;
        return EZ80_OK;

    default:
        break;
    }

    if (op == 0x07u || op == 0x17u || op == 0x27u ||
        op == 0x0Fu || op == 0x1Fu || op == 0x2Fu ||
        op == 0x31u || op == 0x37u || op == 0x3Eu || op == 0x3Fu) {
        const char *other_index = (strcmp(index_name, "IX") == 0) ? "IY" : "IX";
        char disp_text[32];
        if (stream_size < 3u) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
            if (op == 0x07u || op == 0x17u || op == 0x27u) {
                uint8_t ss = (uint8_t)(op >> 4);
                (void)snprintf(out->operands, sizeof(out->operands), "%s,(%s+d)", g_reg16_names[ss], index_name);
            } else if (op == 0x0Fu || op == 0x1Fu || op == 0x2Fu) {
                uint8_t ss = (uint8_t)(op >> 4);
                (void)snprintf(out->operands, sizeof(out->operands), "(%s+d),%s", index_name, g_reg16_names[ss]);
            } else if (op == 0x31u) {
                (void)snprintf(out->operands, sizeof(out->operands), "%s,(%s+d)", index_name, index_name);
            } else if (op == 0x37u) {
                (void)snprintf(out->operands, sizeof(out->operands), "%s,(%s+d)", other_index, index_name);
            } else if (op == 0x3Eu) {
                (void)snprintf(out->operands, sizeof(out->operands), "(%s+d),%s", index_name, other_index);
            } else {
                (void)snprintf(out->operands, sizeof(out->operands), "(%s+d),%s", index_name, index_name);
            }
            out->cycles = 19u;
            return EZ80_ERR_OUT_OF_BOUNDS;
        }

        out->bytes[2] = stream[2];
        out->length = 3u;
        format_index_disp(disp_text, sizeof(disp_text), index_name, (int8_t)stream[2]);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        if (op == 0x07u || op == 0x17u || op == 0x27u) {
            uint8_t ss = (uint8_t)(op >> 4);
            (void)snprintf(out->operands, sizeof(out->operands), "%s,%s", g_reg16_names[ss], disp_text);
        } else if (op == 0x0Fu || op == 0x1Fu || op == 0x2Fu) {
            uint8_t ss = (uint8_t)(op >> 4);
            (void)snprintf(out->operands, sizeof(out->operands), "%s,%s", disp_text, g_reg16_names[ss]);
        } else if (op == 0x31u) {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,%s", index_name, disp_text);
        } else if (op == 0x37u) {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,%s", other_index, disp_text);
        } else if (op == 0x3Eu) {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,%s", disp_text, other_index);
        } else {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,%s", disp_text, index_name);
        }
        out->cycles = 19u;
        return EZ80_OK;
    }

    /* --- IXH/IXL (IYH/IYL) undocumented half-register decode --- */
    {
        char ixh_name[4];
        char ixl_name[4];
        const char *ix_reg8_names[8];
        size_t i;

        (void)snprintf(ixh_name, sizeof(ixh_name), "%sH", index_name);
        (void)snprintf(ixl_name, sizeof(ixl_name), "%sL", index_name);
        for (i = 0u; i < 8u; i++) ix_reg8_names[i] = g_reg8_names[i];
        ix_reg8_names[4] = ixh_name;
        ix_reg8_names[5] = ixl_name;

        /* INC IXH / DEC IXH */
        if (op == 0x24u || op == 0x25u) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), (op == 0x24u) ? "INC" : "DEC");
            copy_token(out->operands, sizeof(out->operands), ixh_name);
            out->cycles = 8u;
            return EZ80_OK;
        }

        /* LD IXH,n */
        if (op == 0x26u) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
            out->cycles = 11u;
            if (stream_size < 3u) {
                (void)snprintf(out->operands, sizeof(out->operands), "%s,n8", ixh_name);
                return EZ80_ERR_OUT_OF_BOUNDS;
            }
            out->bytes[2] = stream[2];
            out->length = 3u;
            (void)snprintf(out->operands, sizeof(out->operands), "%s,0x%02X", ixh_name, stream[2]);
            return EZ80_OK;
        }

        /* INC IXL / DEC IXL */
        if (op == 0x2Cu || op == 0x2Du) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), (op == 0x2Cu) ? "INC" : "DEC");
            copy_token(out->operands, sizeof(out->operands), ixl_name);
            out->cycles = 8u;
            return EZ80_OK;
        }

        /* LD IXL,n */
        if (op == 0x2Eu) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
            out->cycles = 11u;
            if (stream_size < 3u) {
                (void)snprintf(out->operands, sizeof(out->operands), "%s,n8", ixl_name);
                return EZ80_ERR_OUT_OF_BOUNDS;
            }
            out->bytes[2] = stream[2];
            out->length = 3u;
            (void)snprintf(out->operands, sizeof(out->operands), "%s,0x%02X", ixl_name, stream[2]);
            return EZ80_OK;
        }

        /* LD r,IXH / LD r,IXL / LD IXH,r / LD IXL,r (0x40-0x6F, 0x7C, 0x7D) */
        if ((op >= 0x40u && op <= 0x6Fu) || op == 0x7Cu || op == 0x7Du) {
            uint8_t dst = (uint8_t)((op >> 3) & 0x07u);
            uint8_t src = (uint8_t)(op & 0x07u);
            if (src != 6u && dst != 6u && (src == 4u || src == 5u || dst == 4u || dst == 5u)) {
                copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
                (void)snprintf(out->operands, sizeof(out->operands), "%s,%s",
                               ix_reg8_names[dst], ix_reg8_names[src]);
                out->cycles = 8u;
                return EZ80_OK;
            }
        }

        /* ALU A,IXH / ALU A,IXL (0x84/85,0x8C/8D,...,0xBC/BD) */
        if (op >= 0x80u && op <= 0xBFu) {
            uint8_t src = (uint8_t)(op & 0x07u);
            if (src == 4u || src == 5u) {
                uint8_t group = (uint8_t)((op >> 3) & 0x07u);
                copy_token(out->mnemonic, sizeof(out->mnemonic), g_alu_names[group]);
                if (group >= 4u) {
                    copy_token(out->operands, sizeof(out->operands), ix_reg8_names[src]);
                } else {
                    (void)snprintf(out->operands, sizeof(out->operands), "A,%s", ix_reg8_names[src]);
                }
                out->cycles = 8u;
                return EZ80_OK;
            }
        }
    }

    if (op == 0x34u || op == 0x35u || op == 0x36u ||
        op == 0x46u || op == 0x4Eu || op == 0x56u || op == 0x5Eu ||
        op == 0x66u || op == 0x6Eu || op == 0x7Eu ||
        op == 0x70u || op == 0x71u || op == 0x72u || op == 0x73u ||
        op == 0x74u || op == 0x75u || op == 0x77u ||
        op == 0x86u || op == 0x8Eu || op == 0x96u || op == 0x9Eu ||
        op == 0xA6u || op == 0xAEu || op == 0xB6u || op == 0xBEu) {
        int8_t disp;
        char disp_text[32];
        char disp_placeholder[20];

        if (stream_size < 3u) {
            (void)snprintf(disp_placeholder, sizeof(disp_placeholder), "(%s+d)", index_name);

            if (op == 0x34u || op == 0x35u) {
                copy_token(out->mnemonic, sizeof(out->mnemonic), (op == 0x34u) ? "INC" : "DEC");
                copy_token(out->operands, sizeof(out->operands), disp_placeholder);
                out->cycles = 23u;
            } else if (op == 0x36u) {
                copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
                (void)snprintf(out->operands, sizeof(out->operands), "%s,n8", disp_placeholder);
                out->cycles = 19u;
            } else if (op >= 0x70u && op <= 0x77u && op != 0x76u) {
                uint8_t src = (uint8_t)(op & 0x07u);
                copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
                (void)snprintf(out->operands, sizeof(out->operands), "%s,%s",
                               disp_placeholder, g_reg8_names[src]);
                out->cycles = 19u;
            } else if (op == 0x46u || op == 0x4Eu || op == 0x56u || op == 0x5Eu ||
                       op == 0x66u || op == 0x6Eu || op == 0x7Eu) {
                uint8_t dst = (uint8_t)((op >> 3) & 0x07u);
                copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
                (void)snprintf(out->operands, sizeof(out->operands), "%s,%s",
                               g_reg8_names[dst], disp_placeholder);
                out->cycles = 19u;
            } else {
                uint8_t group = (uint8_t)((op >> 3) & 0x07u);
                copy_token(out->mnemonic, sizeof(out->mnemonic), g_alu_names[group]);
                if (group >= 4u) {
                    copy_token(out->operands, sizeof(out->operands), disp_placeholder);
                } else {
                    (void)snprintf(out->operands, sizeof(out->operands), "A,%s", disp_placeholder);
                }
                out->cycles = 19u;
            }
            return EZ80_ERR_OUT_OF_BOUNDS;
        }

        disp = (int8_t)stream[2];
        out->bytes[2] = stream[2];
        out->length = 3u;
        format_index_disp(disp_text, sizeof(disp_text), index_name, disp);

        if (op == 0x34u || op == 0x35u) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), (op == 0x34u) ? "INC" : "DEC");
            copy_token(out->operands, sizeof(out->operands), disp_text);
            out->cycles = 23u;
            return EZ80_OK;
        }

        if (op == 0x36u) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
            out->cycles = 19u;
            if (stream_size < 4u) {
                (void)snprintf(out->operands, sizeof(out->operands), "%s,n8", disp_text);
                return EZ80_ERR_OUT_OF_BOUNDS;
            }
            out->bytes[3] = stream[3];
            out->length = 4u;
            (void)snprintf(out->operands, sizeof(out->operands), "%s,0x%02X",
                           disp_text, stream[3]);
            return EZ80_OK;
        }

        if (op >= 0x70u && op <= 0x77u && op != 0x76u) {
            uint8_t src = (uint8_t)(op & 0x07u);
            copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
            (void)snprintf(out->operands, sizeof(out->operands), "%s,%s",
                           disp_text, g_reg8_names[src]);
            out->cycles = 19u;
            return EZ80_OK;
        }

        if (op == 0x46u || op == 0x4Eu || op == 0x56u || op == 0x5Eu ||
            op == 0x66u || op == 0x6Eu || op == 0x7Eu) {
            uint8_t dst = (uint8_t)((op >> 3) & 0x07u);
            copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
            (void)snprintf(out->operands, sizeof(out->operands), "%s,%s",
                           g_reg8_names[dst], disp_text);
            out->cycles = 19u;
            return EZ80_OK;
        }

        if (op == 0x86u || op == 0x8Eu || op == 0x96u || op == 0x9Eu ||
            op == 0xA6u || op == 0xAEu || op == 0xB6u || op == 0xBEu) {
            uint8_t group = (uint8_t)((op >> 3) & 0x07u);
            copy_token(out->mnemonic, sizeof(out->mnemonic), g_alu_names[group]);
            if (group >= 4u) {
                copy_token(out->operands, sizeof(out->operands), disp_text);
            } else {
                (void)snprintf(out->operands, sizeof(out->operands), "A,%s", disp_text);
            }
            out->cycles = 19u;
            return EZ80_OK;
        }
    }

    {
        ez80_instruction_t inner;
        ez80_status_t inner_status = ez80_decode_instruction(&stream[1], stream_size - 1u, &inner);
        if (inner_status == EZ80_OK || inner_status == EZ80_ERR_OUT_OF_BOUNDS) {
            size_t i;
            copy_token(out->mnemonic, sizeof(out->mnemonic), inner.mnemonic);
            copy_token(out->operands, sizeof(out->operands), inner.operands);
            out->cycles = (uint8_t)(inner.cycles + 4u);
            out->length = (uint8_t)(inner.length + 1u);
            for (i = 1u; i < inner.length && (i + 1u) < sizeof(out->bytes); i++) {
                out->bytes[i + 1u] = inner.bytes[i];
            }
            return inner_status;
        }
    }

    copy_token(out->mnemonic, sizeof(out->mnemonic), "UNIMPL");
    out->operands[0] = '\0';
    return EZ80_ERR_NOT_IMPLEMENTED;
}

ez80_status_t ez80_decode_instruction(const uint8_t *stream,
                                      size_t stream_size,
                                      ez80_instruction_t *out)
{
    if (!stream || !out) return EZ80_ERR_INVALID_ARG;
    if (stream_size == 0) return EZ80_ERR_OUT_OF_BOUNDS;

    memset(out, 0, sizeof(*out));
    out->opcode = stream[0];
    out->bytes[0] = stream[0];
    out->length = 1u;
    out->cycles = 0u;

    if (stream[0] == 0xCBu) {
        return decode_cb_prefix(stream, stream_size, out);
    }
    if (stream[0] == 0xEDu) {
        return decode_ed_prefix(stream, stream_size, out);
    }
    if (stream[0] == 0xDDu) {
        return decode_ddfd_prefix(stream, stream_size, out, "IX");
    }
    if (stream[0] == 0xFDu) {
        return decode_ddfd_prefix(stream, stream_size, out, "IY");
    }

    switch (stream[0]) {
    case 0x00:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "NOP");
        out->operands[0] = '\0';
        out->cycles = 4u;
        return EZ80_OK;

    case 0x08:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "EX");
        copy_token(out->operands, sizeof(out->operands), "AF,AF'");
        out->cycles = 4u;
        return EZ80_OK;

    case 0x10:
        if (stream_size < 2u) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), "DJNZ");
            copy_token(out->operands, sizeof(out->operands), "e");
            out->cycles = 13u;
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[1] = stream[1];
        out->length = 2u;
        out->cycles = 13u;
        copy_token(out->mnemonic, sizeof(out->mnemonic), "DJNZ");
        (void)snprintf(out->operands, sizeof(out->operands), "%+d",
                       (int)((int8_t)stream[1]));
        return EZ80_OK;

    case 0x76:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "HALT");
        out->operands[0] = '\0';
        out->cycles = 4u;
        return EZ80_OK;

    case 0x07:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RLCA");
        out->operands[0] = '\0';
        out->cycles = 4u;
        return EZ80_OK;

    case 0x0F:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RRCA");
        out->operands[0] = '\0';
        out->cycles = 4u;
        return EZ80_OK;

    case 0x17:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RLA");
        out->operands[0] = '\0';
        out->cycles = 4u;
        return EZ80_OK;

    case 0x1F:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RRA");
        out->operands[0] = '\0';
        out->cycles = 4u;
        return EZ80_OK;

    case 0x27:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "DAA");
        out->operands[0] = '\0';
        out->cycles = 4u;
        return EZ80_OK;

    case 0x2F:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "CPL");
        out->operands[0] = '\0';
        out->cycles = 4u;
        return EZ80_OK;

    case 0x37:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "SCF");
        out->operands[0] = '\0';
        out->cycles = 4u;
        return EZ80_OK;

    case 0x3F:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "CCF");
        out->operands[0] = '\0';
        out->cycles = 4u;
        return EZ80_OK;

    case 0x3E:
        if (stream_size < 2u) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
            copy_token(out->operands, sizeof(out->operands), "A,n8");
            out->cycles = 7u;
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[1] = stream[1];
        out->length = 2u;
        out->cycles = 7u;
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        (void)snprintf(out->operands, sizeof(out->operands), "A,0x%02X", stream[1]);
        return EZ80_OK;

    case 0x18:
        if (stream_size < 2u) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), "JR");
            copy_token(out->operands, sizeof(out->operands), "e");
            out->cycles = 12u;
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[1] = stream[1];
        out->length = 2u;
        out->cycles = 12u;
        copy_token(out->mnemonic, sizeof(out->mnemonic), "JR");
        (void)snprintf(out->operands, sizeof(out->operands), "%+d",
                       (int)((int8_t)stream[1]));
        return EZ80_OK;

    case 0xC3:
        if (stream_size < 3u) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), "JP");
            copy_token(out->operands, sizeof(out->operands), "nn");
            out->cycles = 10u;
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[1] = stream[1];
        out->bytes[2] = stream[2];
        out->length = 3u;
        out->cycles = 10u;
        copy_token(out->mnemonic, sizeof(out->mnemonic), "JP");
        (void)snprintf(out->operands, sizeof(out->operands), "0x%04X", read_u16(&stream[1]));
        return EZ80_OK;

    case 0xC9:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RET");
        out->operands[0] = '\0';
        out->cycles = 10u;
        return EZ80_OK;

    case 0xE9:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "JP");
        copy_token(out->operands, sizeof(out->operands), "(HL)");
        out->cycles = 4u;
        return EZ80_OK;

    case 0xD9:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "EXX");
        out->operands[0] = '\0';
        out->cycles = 4u;
        return EZ80_OK;

    case 0xE3:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "EX");
        copy_token(out->operands, sizeof(out->operands), "(SP),HL");
        out->cycles = 19u;
        return EZ80_OK;

    case 0xEB:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "EX");
        copy_token(out->operands, sizeof(out->operands), "DE,HL");
        out->cycles = 4u;
        return EZ80_OK;

    case 0xF9:
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        copy_token(out->operands, sizeof(out->operands), "SP,HL");
        out->cycles = 6u;
        return EZ80_OK;

    default:
        break;
    }

    if (stream[0] >= 0x40u && stream[0] <= 0x7Fu && stream[0] != 0x76u) {
        uint8_t dst = (uint8_t)((stream[0] >> 3) & 0x07u);
        uint8_t src = (uint8_t)(stream[0] & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        (void)snprintf(out->operands, sizeof(out->operands), "%s,%s", g_reg8_names[dst], g_reg8_names[src]);
        out->cycles = (dst == 6u || src == 6u) ? 7u : 4u;
        return EZ80_OK;
    }

    if ((stream[0] == 0x06u) || (stream[0] == 0x0Eu) || (stream[0] == 0x16u) ||
        (stream[0] == 0x1Eu) || (stream[0] == 0x26u) || (stream[0] == 0x2Eu) ||
        (stream[0] == 0x36u) || (stream[0] == 0x3Eu)) {
        uint8_t reg = (uint8_t)((stream[0] >> 3) & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        out->cycles = (reg == 6u) ? 10u : 7u;
        if (stream_size < 2u) {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,n8", g_reg8_names[reg]);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[1] = stream[1];
        out->length = 2u;
        (void)snprintf(out->operands, sizeof(out->operands), "%s,0x%02X", g_reg8_names[reg], stream[1]);
        return EZ80_OK;
    }

    if ((stream[0] == 0x01u) || (stream[0] == 0x11u) || (stream[0] == 0x21u) || (stream[0] == 0x31u)) {
        uint8_t rr = (uint8_t)((stream[0] >> 4) & 0x03u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        out->cycles = 10u;
        if (stream_size < 3u) {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,nn", g_reg16_names[rr]);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[1] = stream[1];
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "%s,0x%04X", g_reg16_names[rr], read_u16(&stream[1]));
        return EZ80_OK;
    }

    if ((stream[0] & 0xC7u) == 0x04u) {
        uint8_t reg = (uint8_t)((stream[0] >> 3) & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "INC");
        copy_token(out->operands, sizeof(out->operands), g_reg8_names[reg]);
        out->cycles = (reg == 6u) ? 11u : 4u;
        return EZ80_OK;
    }

    if ((stream[0] & 0xC7u) == 0x05u) {
        uint8_t reg = (uint8_t)((stream[0] >> 3) & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "DEC");
        copy_token(out->operands, sizeof(out->operands), g_reg8_names[reg]);
        out->cycles = (reg == 6u) ? 11u : 4u;
        return EZ80_OK;
    }

    if (stream[0] >= 0x80u && stream[0] <= 0xBFu) {
        uint8_t group = (uint8_t)((stream[0] >> 3) & 0x07u);
        uint8_t src = (uint8_t)(stream[0] & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), g_alu_names[group]);
        if (group >= 4u) {
            copy_token(out->operands, sizeof(out->operands), g_reg8_names[src]);
        } else {
            (void)snprintf(out->operands, sizeof(out->operands), "A,%s", g_reg8_names[src]);
        }
        out->cycles = (src == 6u) ? 7u : 4u;
        return EZ80_OK;
    }

    if (stream[0] == 0xC6u || stream[0] == 0xCEu || stream[0] == 0xD6u || stream[0] == 0xDEu ||
        stream[0] == 0xE6u || stream[0] == 0xEEu || stream[0] == 0xF6u || stream[0] == 0xFEu) {
        uint8_t group;
        const char *mnemonic;
        out->cycles = 7u;
        if (stream_size < 2u) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), "ALU");
            copy_token(out->operands, sizeof(out->operands), "n8");
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[1] = stream[1];
        out->length = 2u;

        switch (stream[0]) {
        case 0xC6u: group = 0u; break;
        case 0xCEu: group = 1u; break;
        case 0xD6u: group = 2u; break;
        case 0xDEu: group = 3u; break;
        case 0xE6u: group = 4u; break;
        case 0xEEu: group = 5u; break;
        case 0xF6u: group = 6u; break;
        default: group = 7u; break;
        }
        mnemonic = g_alu_names[group];
        copy_token(out->mnemonic, sizeof(out->mnemonic), mnemonic);
        if (group >= 4u) {
            (void)snprintf(out->operands, sizeof(out->operands), "0x%02X", stream[1]);
        } else {
            (void)snprintf(out->operands, sizeof(out->operands), "A,0x%02X", stream[1]);
        }
        return EZ80_OK;
    }

    if ((stream[0] == 0x32u) || (stream[0] == 0x3Au) || (stream[0] == 0x22u) ||
        (stream[0] == 0x2Au) || (stream[0] == 0xCDu)) {
        const char *operands = NULL;
        uint8_t cycles = 0u;

        switch (stream[0]) {
        case 0x32u: operands = "(0x%04X),A"; cycles = 13u; break;
        case 0x3Au: operands = "A,(0x%04X)"; cycles = 13u; break;
        case 0x22u: operands = "(0x%04X),HL"; cycles = 16u; break;
        case 0x2Au: operands = "HL,(0x%04X)"; cycles = 16u; break;
        case 0xCDu: operands = "0x%04X"; cycles = 17u; break;
        default: break;
        }

        copy_token(out->mnemonic, sizeof(out->mnemonic), (stream[0] == 0xCDu) ? "CALL" : "LD");
        out->cycles = cycles;
        if (stream_size < 3u) {
            if (stream[0] == 0xCDu) {
                copy_token(out->operands, sizeof(out->operands), "nn");
            } else {
                copy_token(out->operands, sizeof(out->operands), "nn");
            }
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[1] = stream[1];
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), operands, read_u16(&stream[1]));
        return EZ80_OK;
    }

    if ((stream[0] == 0x20u) || (stream[0] == 0x28u) || (stream[0] == 0x30u) || (stream[0] == 0x38u)) {
        uint8_t cond = (uint8_t)((stream[0] >> 3) & 0x03u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "JR");
        out->cycles = 12u;
        if (stream_size < 2u) {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,e", g_cond_names[cond]);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[1] = stream[1];
        out->length = 2u;
        (void)snprintf(out->operands, sizeof(out->operands), "%s,%+d",
                       g_cond_names[cond], (int)((int8_t)stream[1]));
        return EZ80_OK;
    }

    if ((stream[0] & 0xC7u) == 0xC0u) {
        uint8_t cond = (uint8_t)((stream[0] >> 3) & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RET");
        copy_token(out->operands, sizeof(out->operands), g_cond_names[cond]);
        out->cycles = 11u;
        return EZ80_OK;
    }

    if ((stream[0] & 0xC7u) == 0xC2u) {
        uint8_t cond = (uint8_t)((stream[0] >> 3) & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "JP");
        out->cycles = 10u;
        if (stream_size < 3u) {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,nn", g_cond_names[cond]);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[1] = stream[1];
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "%s,0x%04X",
                       g_cond_names[cond], read_u16(&stream[1]));
        return EZ80_OK;
    }

    if ((stream[0] & 0xC7u) == 0xC4u) {
        uint8_t cond = (uint8_t)((stream[0] >> 3) & 0x07u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "CALL");
        out->cycles = 17u;
        if (stream_size < 3u) {
            (void)snprintf(out->operands, sizeof(out->operands), "%s,nn", g_cond_names[cond]);
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[1] = stream[1];
        out->bytes[2] = stream[2];
        out->length = 3u;
        (void)snprintf(out->operands, sizeof(out->operands), "%s,0x%04X",
                       g_cond_names[cond], read_u16(&stream[1]));
        return EZ80_OK;
    }

    if ((stream[0] & 0xC7u) == 0xC7u) {
        uint8_t vector = (uint8_t)(stream[0] & 0x38u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RST");
        (void)snprintf(out->operands, sizeof(out->operands), "0x%02X", vector);
        out->cycles = 11u;
        return EZ80_OK;
    }

    if (stream[0] == 0xF3u || stream[0] == 0xFBu) {
        copy_token(out->mnemonic, sizeof(out->mnemonic), (stream[0] == 0xF3u) ? "DI" : "EI");
        out->operands[0] = '\0';
        out->cycles = 4u;
        return EZ80_OK;
    }

    if ((stream[0] == 0x02u) || (stream[0] == 0x12u) || (stream[0] == 0x0Au) || (stream[0] == 0x1Au)) {
        copy_token(out->mnemonic, sizeof(out->mnemonic), "LD");
        out->cycles = 7u;
        switch (stream[0]) {
        case 0x02u: copy_token(out->operands, sizeof(out->operands), "(BC),A"); break;
        case 0x12u: copy_token(out->operands, sizeof(out->operands), "(DE),A"); break;
        case 0x0Au: copy_token(out->operands, sizeof(out->operands), "A,(BC)"); break;
        case 0x1Au: copy_token(out->operands, sizeof(out->operands), "A,(DE)"); break;
        default: break;
        }
        return EZ80_OK;
    }

    if (stream[0] == 0xC9u) {
        copy_token(out->mnemonic, sizeof(out->mnemonic), "RET");
        out->operands[0] = '\0';
        out->cycles = 10u;
        return EZ80_OK;
    }

    if ((stream[0] == 0xC5u) || (stream[0] == 0xD5u) || (stream[0] == 0xE5u) || (stream[0] == 0xF5u)) {
        uint8_t qq = (uint8_t)((stream[0] >> 4) & 0x03u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "PUSH");
        copy_token(out->operands, sizeof(out->operands), g_pushpop_names[qq]);
        out->cycles = 11u;
        return EZ80_OK;
    }

    if ((stream[0] == 0xC1u) || (stream[0] == 0xD1u) || (stream[0] == 0xE1u) || (stream[0] == 0xF1u)) {
        uint8_t qq = (uint8_t)((stream[0] >> 4) & 0x03u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "POP");
        copy_token(out->operands, sizeof(out->operands), g_pushpop_names[qq]);
        out->cycles = 10u;
        return EZ80_OK;
    }

    if ((stream[0] == 0x09u) || (stream[0] == 0x19u) || (stream[0] == 0x29u) || (stream[0] == 0x39u)) {
        uint8_t ss = (uint8_t)((stream[0] >> 4) & 0x03u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), "ADD");
        out->cycles = 11u;
        (void)snprintf(out->operands, sizeof(out->operands), "HL,%s", g_reg16_names[ss]);
        return EZ80_OK;
    }

    if ((stream[0] == 0x03u) || (stream[0] == 0x13u) || (stream[0] == 0x23u) || (stream[0] == 0x33u) ||
        (stream[0] == 0x0Bu) || (stream[0] == 0x1Bu) || (stream[0] == 0x2Bu) || (stream[0] == 0x3Bu)) {
        uint8_t ss = (uint8_t)((stream[0] >> 4) & 0x03u);
        copy_token(out->mnemonic, sizeof(out->mnemonic), ((stream[0] & 0x08u) != 0u) ? "DEC" : "INC");
        copy_token(out->operands, sizeof(out->operands), g_reg16_names[ss]);
        out->cycles = 6u;
        return EZ80_OK;
    }

    if (stream[0] == 0xD3u || stream[0] == 0xDBu) {
        if (stream_size < 2u) {
            copy_token(out->mnemonic, sizeof(out->mnemonic), (stream[0] == 0xD3u) ? "OUT" : "IN");
            copy_token(out->operands, sizeof(out->operands), (stream[0] == 0xD3u) ? "(n8),A" : "A,(n8)");
            out->cycles = 11u;
            return EZ80_ERR_OUT_OF_BOUNDS;
        }
        out->bytes[1] = stream[1];
        out->length = 2u;
        out->cycles = 11u;
        copy_token(out->mnemonic, sizeof(out->mnemonic), (stream[0] == 0xD3u) ? "OUT" : "IN");
        if (stream[0] == 0xD3u) {
            (void)snprintf(out->operands, sizeof(out->operands), "(0x%02X),A", stream[1]);
        } else {
            (void)snprintf(out->operands, sizeof(out->operands), "A,(0x%02X)", stream[1]);
        }
        return EZ80_OK;
    }

    copy_token(out->mnemonic, sizeof(out->mnemonic), "UNIMPL");
    out->operands[0] = '\0';
    return EZ80_ERR_NOT_IMPLEMENTED;
}

/* ---------------------------------------------------------------------------
 * ADL-aware instruction decoder.
 *
 * In ADL mode the eZ80 widens every 16-bit immediate / address operand to
 * 24 bits (3 bytes instead of 2).  This wrapper decodes in Z80-compat mode
 * first, then adjusts instruction length and copies the extra byte for every
 * affected opcode family when adl_mode is true.
 *
 * Affected main opcodes (Z80 length 3 -> ADL length 4):
 *   LD rr,nn  (01/11/21/31)
 *   LD (nn),A / LD A,(nn)  (32/3A)
 *   LD (nn),HL / LD HL,(nn)  (22/2A)
 *   JP nn  (C3)
 *   JP cc,nn  (C2/CA/D2/DA/E2/EA/F2/FA)
 *   CALL nn  (CD)
 *   CALL cc,nn  (C4/CC/D4/DC/E4/EC/F4/FC)
 *
 * Affected ED-prefix opcodes (Z80 length 4 -> ADL length 5):
 *   LD (nn),ss  (ED 43/53/63/73)
 *   LD ss,(nn)  (ED 4B/5B/6B/7B)
 *
 * Affected DD/FD-prefix opcodes (Z80 length 4 -> ADL length 5):
 *   LD IX,nn  (DD 21)
 *   LD (nn),IX  (DD 22)
 *   LD IX,(nn)  (DD 2A)
 * -----------------------------------------------------------------------*/

static uint32_t read_u24(const uint8_t *stream)
{
    return (uint32_t)stream[0]
         | ((uint32_t)stream[1] << 8)
         | ((uint32_t)stream[2] << 16);
}

static bool main_opcode_has_imm16(uint8_t op)
{
    if (op == 0x01u || op == 0x11u || op == 0x21u || op == 0x31u) return true;
    if (op == 0x32u || op == 0x3Au || op == 0x22u || op == 0x2Au) return true;
    if (op == 0xC3u) return true;
    if (op == 0xCDu) return true;
    if ((op & 0xC7u) == 0xC2u) return true;
    if ((op & 0xC7u) == 0xC4u) return true;
    return false;
}

static bool ed_opcode_has_imm16(uint8_t ed_op)
{
    if (ed_op == 0x43u || ed_op == 0x53u || ed_op == 0x63u || ed_op == 0x73u) return true;
    if (ed_op == 0x4Bu || ed_op == 0x5Bu || ed_op == 0x6Bu || ed_op == 0x7Bu) return true;
    return false;
}

static bool ddfd_opcode_has_imm16(uint8_t ddfd_op)
{
    if (ddfd_op == 0x21u) return true;
    if (ddfd_op == 0x22u) return true;
    if (ddfd_op == 0x2Au) return true;
    return false;
}

ez80_status_t ez80_decode_instruction_adl(const uint8_t *stream,
                                          size_t stream_size,
                                          bool adl_mode,
                                          ez80_instruction_t *out)
{
    ez80_status_t status;

    if (!adl_mode) {
        return ez80_decode_instruction(stream, stream_size, out);
    }

    status = ez80_decode_instruction(stream, stream_size, out);
    if (status != EZ80_OK) return status;

    if (out->opcode != 0xCBu && out->opcode != 0xEDu &&
        out->opcode != 0xDDu && out->opcode != 0xFDu) {
        if (main_opcode_has_imm16(out->opcode)) {
            if (stream_size < (size_t)(out->length + 1u)) {
                return EZ80_ERR_OUT_OF_BOUNDS;
            }
            out->bytes[out->length] = stream[out->length];
            out->length = (uint8_t)(out->length + 1u);
        }
        return EZ80_OK;
    }

    if (out->opcode == 0xEDu && out->length >= 2u) {
        uint8_t ed_op = out->bytes[1];
        if (ed_opcode_has_imm16(ed_op)) {
            if (stream_size < (size_t)(out->length + 1u)) {
                return EZ80_ERR_OUT_OF_BOUNDS;
            }
            out->bytes[out->length] = stream[out->length];
            out->length = (uint8_t)(out->length + 1u);
        }
        return EZ80_OK;
    }

    if ((out->opcode == 0xDDu || out->opcode == 0xFDu) && out->length >= 2u) {
        uint8_t ddfd_op = out->bytes[1];
        if (ddfd_opcode_has_imm16(ddfd_op)) {
            if (stream_size < (size_t)(out->length + 1u)) {
                return EZ80_ERR_OUT_OF_BOUNDS;
            }
            out->bytes[out->length] = stream[out->length];
            out->length = (uint8_t)(out->length + 1u);
        }
        return EZ80_OK;
    }

    return EZ80_OK;
}
