# CalcEngine Installation Guide

How to install CalcEngine on your TI-84 Plus CE calculator.

---

## Requirements

- **Calculator**: TI-84 Plus CE or TI-84 Plus CE-T (color edition with eZ80 processor)
- **Computer**: Windows, macOS, or Linux
- **USB cable**: Mini-USB to USB-A (comes with your calculator)
- **Transfer software**: TI Connect CE (official) or TiLP (open-source alternative)

### OS Compatibility

| Calculator OS | CalcEngine Works? | Notes |
|---------------|-------------------|-------|
| OS 5.0 - 5.4 | Yes | No jailbreak needed |
| OS 5.5 | Yes | Jailbreak required (arTIfiCE) |
| OS 5.6 - 5.7 | Yes | Jailbreak required (arTIfiCE) |
| OS 5.8+ | Yes | Jailbreak required (arTIfiCE v2 currently supports this line) |

See `docs/OS-VERSION-TEST-MATRIX.md` for detailed expectations and compatibility notes.

### How to Check Your OS Version

1. Press `[2nd]` then `[+]` (MEM) on your calculator
2. Select **1: About**
3. Your OS version is shown (e.g., "5.5.1")

---

## Step 1: Download CalcEngine

Download the latest CalcEngine release files from the [GitHub Releases page](https://github.com/XanderXML-Bit/algebrix-ce/releases).

Important:
- Small builds ship as a single `.8xp`.
- Oversized builds ship as a launcher `.8xp` plus numbered chunk files
  (`.8xp.0.8xv`, `.8xp.1.8xv`, ...). You must transfer all of them.

---

## Step 2: Install Transfer Software

### Option A: TI Connect CE (Recommended)

1. Go to [education.ti.com/software](https://education.ti.com/en/products/computer-software/ti-connect-ce-sw)
2. Download TI Connect CE for your operating system
3. Install and launch the application

### Option B: TiLP (Linux / Open Source)

```bash
# Debian/Ubuntu
sudo apt-get install tilp2

# Fedora
sudo dnf install tilp2

# Arch
sudo pacman -S tilp
```

---

## Step 3: Jailbreak Your Calculator (If Needed)

**If your OS is 5.0 - 5.4, skip this step.** Your calculator already allows running C programs.

**If your OS is 5.5 or newer**, you need to install a jailbreak. The current recommended tool is **arTIfiCE**.

### What Jailbreaking Does

- Restores the ability to run unsigned ASM and C programs
- Does NOT void your warranty (software-only modification)
- Does NOT permanently modify your calculator
- Can be undone by a RAM reset or OS update

### Jailbreak Instructions (arTIfiCE)

1. **Download arTIfiCE** from [yvantt.github.io/arTIfiCE](https://yvantt.github.io/arTIfiCE/)
2. **Connect your calculator** to your computer via USB
3. **Transfer the arTIfiCE files** to your calculator using TI Connect CE:
   - Send the `Cabri Jr` app (`.8ek` file) to your calculator
   - Send the `arTIfiCE` program (`.8xv` file) to your calculator
4. **On the calculator**:
   - Press `[apps]` and select **Cabri Jr**
   - When it prompts to open a file, press any key to exit
   - The jailbreak is now active
5. **Verify**: You should now be able to run C programs

**Important**: The jailbreak is temporary — it resets when you perform a RAM clear or remove batteries. You'll need to re-run the Cabri Jr step after each reset.

### If Compatibility Changes in Future OS Updates

As of 2026-02-19, arTIfiCE v2 documents support for TI-84 Plus CE OS 5.3+ (including current 5.8.x).  
If TI ships a newer OS that breaks jailbreak compatibility, you may need to downgrade:

1. **Check compatibility** at [cemetech.net/downloads](https://www.cemetech.net/downloads/ti84pce) for the latest jailbreak tools
2. **Download an older OS** from TI's website
3. **Send the OS file** via TI Connect CE (use "OS" transfer mode, not program transfer)
4. **Warning**: OS downgrade erases all programs and data. Back up your files first!

### Safety Notes

- **No bricking risk**: The jailbreak is software-only. A RAM clear always restores the calculator to working state.
- **Testing policies**: If your school/test requires clearing the calculator, a RAM clear will remove CalcEngine and the jailbreak.
- **Updates**: Be cautious updating your TI OS — newer versions may block the jailbreak until a new exploit is found.

---

## Step 4: Transfer CalcEngine to Your Calculator

1. **Connect** your calculator to your computer with the USB cable
2. **Open TI Connect CE** (or TiLP)
3. **Click "Send"** (or drag-and-drop the file)
4. **Select all CalcEngine files** from the release:
   - Launcher: `CALCENG*.8xp`
   - Chunk AppVars (if present): `CALCENG*.8xp.*.8xv`
5. **Wait** for the transfer to complete (a few seconds)

### Transfer Tips

- If the calculator isn't detected, try a different USB cable or port
- Make sure the calculator is on and at the home screen
- On Linux, you may need to set udev rules for the TI device

---

## Step 5: Run CalcEngine

1. On your calculator, press `[prgm]`
2. Select **CALCENGN** from the program list
3. Press `[enter]` to run

CalcEngine will display its header and you can start typing expressions.

### Storage: RAM vs Archive

- **RAM** (default): Faster to launch but erased by RAM clear
- **Archive**: Survives RAM clear but must be unarchived to run

To archive: `[2nd]` `[+]` (MEM) > **2: Mem Mgmt** > **7: Prgm** > select CALCENGN > `[enter]` to toggle

To unarchive: Same process — toggle back to RAM before running.

---

## Troubleshooting

### "ERR: INVALID" when trying to run
- Your calculator needs a jailbreak. See Step 3.

### Calculator not detected by computer
- Try a different USB cable (some charge-only cables don't transfer data)
- Make sure the calculator is turned on
- Reinstall TI Connect CE drivers

### Program disappeared after RAM clear
- This is normal. Re-transfer the `.8xp` file from your computer.
- Consider archiving the program for persistence (see Storage section above).

### "ERR: MEMORY" when running
- This error means TI-OS cannot allocate enough RAM for runtime execution.
- For oversized CE toolchain builds, the launcher reconstructs program pages in
  RAM before jump. If total payload is too large, launch fails even when files
  are transferred correctly.
- Ensure all launcher/chunk files were transferred. Missing chunks can also
  produce launch failures.

### Program runs slowly
- Complex expressions with many nested operations may take a moment on the eZ80 processor. This is normal.
- If the calculator appears frozen, wait a few seconds. Press `[on]` to force-quit if needed.

---

## Discreet Usage (School Considerations)

### Hiding CalcEngine

CalcEngine appears as `CALCENGN` in your program list. To minimize visibility:

1. **Archive the program**: Archived programs don't appear in the main `[prgm]` list unless you press `[2nd]` in the Mem Mgmt screen. Archive via `[2nd]` `[+]` (MEM) > **2: Mem Mgmt** > **7: Prgm** > select CALCENGN > `[enter]` to toggle to archive.
2. **Unarchive before use**: You must unarchive to run. Same steps — toggle back to RAM. After use, re-archive to hide again.
3. **Rename**: Some transfer tools let you rename the .8xp file before sending. A nondescript name like `NOTES` is less conspicuous than `CALCENGN`.

### RAM Clear Behavior

Schools and standardized tests often require a RAM clear before tests:

| Storage Location | Survives RAM Clear? | Notes |
|------------------|--------------------|----|
| **RAM** (default) | No | Program is deleted |
| **Archive** | Yes | Program persists but must be unarchived to run |
| **Jailbreak** | No | arTIfiCE must be re-activated after RAM clear |

- A **RAM clear** (`[2nd]` `[+]` > **7: Reset** > **1: All RAM**) removes all programs in RAM, variables, and the jailbreak state.
- Archived programs survive a RAM clear. However, you still need to re-jailbreak (run Cabri Jr again) to execute C programs.
- A **full memory reset** (`[2nd]` `[+]` > **7: Reset** > **2: All Memory**) erases everything including Archive. Only use this to completely wipe the calculator.

### Archive vs RAM Trade-offs

| Factor | RAM | Archive |
|--------|-----|---------|
| Launch speed | Instant | Must unarchive first (~2 sec) |
| RAM clear survival | No | Yes |
| Visibility in prgm menu | Always shown | Hidden unless you look in Mem Mgmt |
| Memory used | ~28KB RAM | ~28KB Archive (saves RAM) |

**Recommended strategy**: Keep CalcEngine in Archive normally. Unarchive when you need it, use it, then re-archive when done. This keeps it hidden and survives RAM clears.

### Important Notes

- CalcEngine is intended as a **learning and homework tool**. Using unauthorized aids during tests violates academic integrity policies.
- Always check your school's and testing organization's calculator policies before tests.
- When in doubt, perform a RAM clear before a test to ensure compliance.

---

## Uninstalling

1. Press `[2nd]` `[+]` (MEM)
2. Select **2: Mem Mgmt/Del**
3. Select **7: Prgm**
4. Scroll to **CALCENGN**
5. Press `[del]` to delete

This does not undo the jailbreak. To remove the jailbreak, perform a RAM clear: `[2nd]` `[+]` (MEM) > **7: Reset** > **1: All RAM**.
