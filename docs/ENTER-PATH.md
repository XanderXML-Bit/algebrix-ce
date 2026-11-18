# Enter Path

## Main result screen behavior

In the legacy graphical UI (`src/main.c` + `src/ui.c`), pressing plain `ENTER` on a result screen does not close CalcEngine, return to a menu, or branch back to another program.

The current result-screen behavior is:

- if checkpoint reveal mode is active, `ENTER` reveals the next checkpoint
- else if step reveal mode is active, `ENTER` reveals the next step
- else `ENTER` reruns `process_input()` on the existing input buffer

That behavior is now exposed through `ui_result_enter_action()` so desktop tests can assert it directly. There is no result-screen `ENTER` action that exits the UI.

## Related hardware risk that was actually present

The real split-architecture risk was not the result-screen `ENTER` key path. It was nested module forwarding in `src/split/module_forward.c`.

Forwarding modules such as `CEALG`, `CECINT`, `CECLIM`, `CECPOL`, `CEASLV`, `CECISM`, `CECIAD`, and `CEISTR` launched child programs as if control would always return synchronously after `os_RunPrgm()`. The IPC contract already documents that this is not true on real hardware unless a re-entry callback is used.

The forwarding path now uses the same re-entry callback model as the launcher, and forwarding modules detect callback-driven reentry so child responses can unwind cleanly back to `CALCENG` without depending on an invalid post-launch return path.
