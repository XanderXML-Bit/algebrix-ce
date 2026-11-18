# Changelog

## 2026-04-12

### Pass 75: Second-Order ODEs, Eigenvalues/Eigenvectors, Implicit Curve Graphing

**Second-order linear ODE solver** (`dsolve2(a,b,c,f,x)`)
- Solves ay'' + by' + cy = f(x) via characteristic equation
- Three cases: two distinct real roots (C1*e^r1x + C2*e^r2x), repeated root (C1 + C2*x)*e^rx, complex conjugate roots e^ax*(C1*cos(bx) + C2*sin(bx))
- Constant forcing function (particular solution via undetermined coefficients)
- Step-by-step output showing discriminant, roots, and general solution form

**Eigenvalue and eigenvector computation**
- `eigenval(matrix)` — computes all real eigenvalues up to 3x3 via characteristic polynomial
- `eigenvec(matrix)` — computes eigenvectors via null space of (A - lambda*I)
- Formatted output showing lambda values and corresponding eigenvectors

**Implicit curve graphing**
- `graphimpl(f,x,y[,xmin,xmax])` — plots the zero contour of f(x,y)=0
- Numerical contour tracing with configurable x/y window

**Menu and UI**
- ODEs & Graphs page: 11 items (added dsolve2, graphimpl)
- Linear Algebra page: 7 items (added eigenval, eigenvec)
- Calculus page: 25 items (dsolve2 listed as alternative ODE entry point)

### Pass 74: Vector Calculus, Laplace Transforms, Unit Conversion, User Functions, Bookmarks

**Vector calculus** (`dot`, `cross`, `vgrad`, `vdiv`, `vcurl`)
- Dot product of arbitrary-length equal vectors
- Cross product of 3D vectors
- Gradient field: partial derivatives of f w.r.t. each variable
- Divergence: symbolic sum of partial derivatives
- Curl: symbolic curl vector for 3D vector fields

**Laplace transforms** (`laplace`, `ilaplace`)
- Forward transform via pattern table: 1, t^n, e^at, sin(bt), cos(bt), t*sin/cos, sinh, cosh
- Inverse transform via partial fractions + reverse table lookup

**Polar calculus** (`polararea`, `polararclen`)
- Polar area: half-integral of r^2 d-theta from a to b
- Polar arc length: integral of sqrt(r^2 + r'^2) d-theta

**First-order ODE auto-solver** (`dsolve(rhs,x,y)`)
- Automatically tries separable, linear, and exact strategies
- Returns solution with method annotation

**Unit conversion** (`convert(value, from, to)`)
- 30+ unit categories: length (m/km/mi/ft/in/cm), mass (kg/g/lb/oz), temperature (C/F/K), speed (m/s, km/h, mph), area, volume, and more

**User-defined functions** (`def f(x)=expr`)
- Define custom single-variable functions by name
- Persistent across sessions (saved to AppVar)
- `def list` shows all defined functions
- `def del name` removes a function
- User functions auto-expand in expressions

**Bookmarks** (`bm name` to save, menu to recall)
- Save any expression by name
- Bookmark tab in the function menu for recall
- Persistent storage (AppVar-backed)

### Pass 74/75 Build Fixes (2026-04-12)
- Fixed `processor.c` TI cross-compilation: `pop` variable moved unconditional, `find_var_in` guard expanded, all new feature handlers wrapped in `#ifndef HW_BUILD_SPLIT` (handled by main.c before IPC dispatch in split builds)
- Enabled zx0 compression for CECLIC (51.7KB -> 24.6KB), all 36 split modules now within 50KB limit
- Desktop test suite: 12,281 passing, 0 failing

## 2026-03-20

### CEmu Comprehensive Coverage
- Extended `tests/emulator/autotest.comprehensive.json` with real-ROM hashes for menu-tab navigation, Show Work toggle persistence, symbol insertion, Recently Used population, and long-expression Enter-path coverage.
- Kept the comprehensive suite stable after the new UI flows by restoring `Show Work` to `ON` before the downstream CAS and continuity checks.

### CRC Cataloging
- Upgraded `tools/emulator/run-comprehensive-autotest.ps1` to emit canonical CRC catalog artifacts (`autotest.comprehensive.crc-catalog.json` and `.md`) from the baselined suite.
- Added PowerShell regression coverage for the comprehensive autotest runner so baseline capture and verify mode both assert the catalog artifacts and hash counts.

## 2026-03-06

### Settings Menu Architecture
- Replaced the old split menu/settings flow with a tabbed main menu spanning `Symbols & Functions`, `Recently Used`, and `Settings`.
- Moved show-work, solve-mode, method-selection, and mirrored standard-mode toggles into the Settings tab.
- Added menu-tab key handling (`2nd` + left/right) and desktop regression coverage for tab rendering and tab navigation key mapping.

### Show Work Modes
- Corrected Full Auto so completed work renders immediately as a scrollable step list instead of drip-feeding one line per Enter press.
- Added checkpoint-result helpers so Step-by-Step mode can surface operation states, not just raw line ranges.
- Added a checkpoint-only renderer for `Show Work = OFF`, plus `MODE` fast-forward handling for finishing the remaining solve session.

### Method Selection
- Fixed forced partial-fractions integration so the settings menu now routes to a real partial-fractions strategy instead of incorrectly reusing integration by parts.
- Ranked integration methods by actual applicability probes, allowing Auto mode to recommend cleaner methods and flag impossible forced choices before execution.
- Upgraded invalid-method errors to tell the user to switch to Auto and enumerate other applicable methods when a forced strategy does not fit the current integral.

### Calculation History Stack
- Replaced the ad hoc input-only history ring in `main.c` with the shared history stack so persisted entries now keep both the original input and the current result snapshot.
- Added result-aware re-entry text handling: Up-arrow recall now prefers the stored result, strips a leading `=`, and falls back to the original input when no result string exists.
- Changed quit/clear/edit transitions to archive the active solve state before returning to the input line, including mid-solve checkpoint sessions.

### Scrollable Option Menus
- Added a dedicated method-options prompt for step-by-step integrals so Auto mode can surface concrete strategy choices before the solve begins.
- Made the decision prompt modal with `Up`/`Down` scrolling, `Enter` to solve, and `Clear`/`Quit`/`Mode` to back out without committing.
- Added a TI-84-scale scrollable renderer plus desktop coverage for both solver-backed method reports and off-screen option scrolling.

### Step Checkpoints
- Added explicit checkpoint result snapshots so operation checkpoints no longer depend on whichever raw step line happened to appear last in a range.
- Populated checkpoint summaries with meaningful math-state outputs across derivative, tangent, integral, limit, Taylor, expand, factor, and solve handlers, including definite-integral bound evaluation.
- Extended desktop regressions to cover explicit checkpoint results, formatted checkpoint snapshots, and rendered operation-level checkpoint summaries.
