# Emulator vs Hardware Parity

This note documents the current gaps between the custom eZ80 emulator under `tools/emulator/ez80/` and real TI-84 Plus CE hardware. The emulator is useful for deterministic regression work, archive/RAM swap simulation, and IPC-flow validation, but it is still a scaffold plus OS-simulation layer rather than a cycle-accurate CE model.

## Keypad behavior

- The emulator models keys as direct row/column bit changes in an 8x8 matrix. `ez80_emulator_inject_key()` clears a single bit in `hw.keypad_rows[row]` and `ez80_emulator_release_key()` restores it.
- IRQ behavior is synthetic. A keypad interrupt is raised immediately on a clean press transition when keypad IRQs are enabled.
- There is no debounce model, scan latency, ghosting model, hold-repeat behavior, or TI-OS keyboard service routine timing.
- The agent/API key code is a packed row/column byte, not a TI-OS `sk_key_t` value.

Practical impact:
- Key-driven UI tests are deterministic.
- Timing-sensitive keypad bugs can still differ on hardware.

## Display rendering

- The emulator keeps separate software buffers for the homescreen text layer, graph buffer, and LCD framebuffer.
- Homescreen text is rendered from `g_home_screen`, not from decoded glyph drawing into LCD memory.
- `GRAPH_RENDER_TO_LCD` converts the graph buffer to grayscale RGB565 pixels; it does not emulate the full CE LCD pipeline or TI-OS text renderer.
- LCD ASCII export is a heuristic downsample unless a homescreen overlay is active, in which case the emulator returns the stored text characters directly.
- LCD controller commands, write windows, and pipeline status exist, but the model is simplified and optimized for inspection rather than hardware-faithful raster timing.

Practical impact:
- Text and graph assertions are stable.
- Pixel-perfect or timing-dependent LCD behavior can diverge from hardware.

## Memory layout and program loading

- The emulator uses a flat 4 MB flash array and 256 KB RAM array.
- TI-OS program loading is simulated with fixed pools:
  - RAM program pool: `EZ80_RAM_BASE + 0x20000`, size `0x18000` bytes
  - Archive pool: `0x3C0000`, size `0x040000` bytes
- Archive-to-RAM and RAM-to-archive transitions are modeled as bounded copies plus CRC checks.
- Active-program state is tracked explicitly in emulator MMIO/OS state rather than inferred from real TI-OS internals.
- The `.8xp` loader maps AppVars into the emulator variable table as string-type slots for convenience.

Practical impact:
- Multi-program swap validation is strong enough for CalcEngine packaging checks.
- Real VAT layout, allocator pressure, and TI-OS swap edge cases are only approximated.

## Timer and RTC behavior

- Timer advancement is driven by emulator instruction-cycle accounting, not wall clock.
- Timer 0 and Timer 1 are simplified increment/reload counters with prescalers `1/4/16/64`.
- RTC time advances at a fixed `32768` emulator cycles per second and starts from a canned midday value.
- RTC alarm, watchdog, and sleep/wake logic are modeled, but they are simplified control-flow tools rather than hardware-calibrated peripherals.
- Interrupt service is deterministic and tied to instruction boundaries plus explicit halted-CPU idle stepping.

Practical impact:
- Regression runs are repeatable.
- Absolute timing, jitter, drift, and TI ASIC edge behavior still require hardware confirmation.

## Bottom line

The emulator is currently reliable for:

- split-program archive/RAM swap validation
- IPC request/response flow
- deterministic keypad-driven regression scripts
- homescreen/graph output inspection
- package-size and hardware-constraint checks

It is not yet proof of full hardware parity for:

- keypad timing nuances
- LCD controller fidelity
- true TI-OS memory-management behavior
- real-time timer/RTC characteristics
