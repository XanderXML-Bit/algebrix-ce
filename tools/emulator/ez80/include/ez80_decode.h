#ifndef EZ80_DECODE_H
#define EZ80_DECODE_H

#include <stddef.h>
#include <stdint.h>

#include "ez80_emulator.h"

ez80_status_t ez80_decode_instruction(const uint8_t *stream,
                                      size_t stream_size,
                                      ez80_instruction_t *out);

ez80_status_t ez80_decode_instruction_adl(const uint8_t *stream,
                                          size_t stream_size,
                                          bool adl_mode,
                                          ez80_instruction_t *out);

#endif /* EZ80_DECODE_H */
