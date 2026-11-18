# CalcEngine User Manual

**Version 1.0.0**
**For TI-84 Plus CE**
**Zendren LLC**

---

## Table of Contents

1. [Getting Started](#getting-started)
2. [Input Syntax](#input-syntax)
3. [Keyboard Reference](#keyboard-reference)
4. [Calculus](#calculus)
5. [Algebra](#algebra)
6. [Linear Algebra](#linear-algebra)
7. [Statistics](#statistics)
8. [Finance](#finance)
9. [Probability Distributions](#probability-distributions)
10. [Other Features](#other-features)
11. [Settings](#settings)
12. [Menu System](#menu-system)
13. [Step-by-Step Work](#step-by-step-work)
14. [Answer Format](#answer-format)
15. [Troubleshooting](#troubleshooting)

---

## Getting Started

### First Launch

When you run CalcEngine for the first time, a 3-page tutorial walks you through the basics. Use the **Left** and **Right** arrow keys to navigate between pages, or press **ENTER** to advance. Press **CLEAR** at any time to skip the tutorial and start using CalcEngine immediately.

### Basic Operation

CalcEngine works like a command line: you type a mathematical expression, press **ENTER**, and the result appears on screen.

1. Type an expression using the keypad (e.g., `2+3*4`).
2. Press **ENTER** to evaluate.
3. The answer appears in blue at the top of the work area.
4. If steps are enabled, they appear below the answer.
5. Press **CLEAR** once to return to input mode (your expression is preserved).
6. Press **CLEAR** again to erase the input and start fresh.

### How Results Are Written

Result lines use only the parentheses the math needs, and leave out `*` after a number:

| Input | Result |
|-------|--------|
| `d/dx(x^3+2x)` | `d/dX = 3X^2+2` |
| `d/dx(sin(x)*x^2)` | `d/dX = cos(X)*X^2+sin(X)*(2X)` |
| `factor(x^2-1,x)` | `= (X-1)*(X+1)` |
| `d/dx(ln(x^2+1))` | `d/dX = 2X/(X^2+1)` |

- `*` is omitted only when a number is a product's left factor. Examples include `2X`, `2sin(X)`, and `2(X+1)`.
- `*` stays elsewhere, including `X*sin(X)`, `2*3`, and `X/2*sqrt(X)`. It also stays before `e`, `e^`, and `exp(` to avoid scientific-notation confusion.
- Powers of powers keep their parentheses, `X^(2^3)` and `(X^2)^3`, because the TI-OS and Algebrix group `^` differently.
- Grouping stays unchanged: `X+(Y+Z)` keeps its parentheses. A negative term can appear as `X+(-0.166667X^3)`.
- Basic integrals and partial-fraction integrals retain fully parenthesised results under the current module-format policy.
- A complete scalar expression after `=` (such as `3X^2+2`) is valid Algebrix input. Typed back in, it gives the same answer, up to the digits shown.
- A symbolic expression cut by its formatting buffer ends in `...`. Other result fields have separate display limits.
- Show-steps lines keep the fully parenthesised form, for example `= (2*x)`. The exception is a step that repeats part of the answer, such as a gradient component or a division remainder, which is written the way the answer is.

### Seeing a Long Result (CALCENG Launcher)

The CALCENG home screen is 26 characters wide. A long expression or result continues on the next rows, up to 3 rows each. For example, `d/dx(sin(x)*x^2)` continues from `d/dX = cos(X)*X^2+sin(X)*(` to `2X)`.

- If an expression or result needs more than 3 rows, its third row ends in `…`. Press **2nd** + **ENTER** to recall the full expression.
- Older entries that no longer fit on the screen are left off whole, never cut off at the top. (Any entry, old or new, still shows `…` when a field needs more than 3 rows.)
- On the input line, `…` after `>` means the start of what you typed has scrolled off. `…` in the last column means more text follows to the right of the cursor.

When the newest result has more than the home screen shows (an `intstrategy(...)` ranking, steps, or a line over 3 rows), the row under it says how many lines there are, for example `UP=full result (21 ln)`.

- Press **UP**, or **ENTER** with an empty input line, to open the newest result in full. It is wrapped at 26 columns, with a header such as `Result 1-8/21`.
- In the viewer, **UP** and **DOWN** scroll one line, and **LEFT** and **RIGHT** move one page of 8 lines.
- **CLEAR**, **ENTER** or **MODE** goes back to the home screen. What you had typed on the input line is kept.
- The viewer shows the newest result only. After CALCENG has been quit and started again, it shows the stored first line (up to 63 characters).

### When an Input Has a Syntax Error (CALCENG Launcher)

If ENTER cannot read what you typed, for example `+` on its own or `2*(`, the input stays editable and `Syntax error` appears just below it. The submitted expression and its error are also saved together in history, so the error remains after you edit or clear the input or quit CALCENG. Consecutive identical submissions share one history entry, as with successful inputs. **Ans is not changed.**

- For a failed input starting with `d(`, such as `d(x^3,x)`, the error includes `d/dx(expr)`: use `d/dx(x^3)` instead.
- **UP** opens the newest submission's error, not the previous successful answer. After a syntax error, older history entries keep only their stored first result line; the newest pairs that fit remain on the home screen. **2nd+ENTER** recalls earlier expressions for editing.

- Fix the input and press **ENTER** again. The temporary message below the input goes away at the next key you press; the history copy stays.
- If Algebrix cannot read a character or a word, such as `AB` or a lone `.`, the cursor moves to it (to the start of the word). For other errors, such as a missing `)`, the cursor stays where it was.
- **CLEAR** erases the input line, as usual.

### When a Module Cannot Run (CALCENG Launcher)

Sometimes CALCENG reads your input but cannot get an answer from the module that should evaluate it. The error is saved in history beside the expression, like any other answer, so it stays after later inputs and after CALCENG is quit and started again. **Ans and the MODE settings are not changed.** UP shows the error with the header `Error`.

| Error | Meaning | CALCENG afterwards |
|---|---|---|
| `Module missing` | The module program for this input is not on the calculator. | Exits. Transfer the full package, then start CALCENG again |
| `Program launch failed` | TI-OS could not start the module, for example for lack of free RAM. | Exits |
| `IPC write failed`, `IPC state save failed` | CALCENG could not write its request or state AppVar, for example for lack of free RAM. | Exits |
| `Module <name> returned no IPC response` | The module returned without an answer. | Stays open |
| `Module <name> returned invalid IPC data` | The module's answer could not be read. | Stays open |

If an `IPC ...` error appears because RAM is full, CALCENG may also be unable to save that error. Free some RAM, then start CALCENG again.

### Quick Examples

Try these expressions to get familiar with CalcEngine:

| Input | Result |
|-------|--------|
| `2+3*4` | `= 14` |
| `sin(pi/6)` | `= 0.5` |
| `sqrt(144)` | `= 12` |
| `3^4` | `= 81` |
| `ln(e)` | `= 1` |
| `5!` | `= 120` |

### Status Bar

The bottom of the screen shows:
- **[2ND]** or **[A]** when the 2nd or ALPHA modifier is active.
- **RAD** or **DEG** indicating the current angle mode.
- **N:x/512** showing how many of the 512 AST nodes are in use.
- **2G=help** as a reminder that 2nd+GRAPH opens the help screen.
- A battery indicator on the far right.

---

## Input Syntax

### Numbers

Type numbers using the digit keys and the decimal point.

- Integers: `42`, `0`, `1000`
- Decimals: `3.14`, `0.5`, `.25`
- Negative numbers: Use the **(-)**  key (negate) to type `(-3)`, or use the minus key for subtraction: `0-3`
- Scientific notation is not typed directly; use multiplication and power: `1.5*10^6`

### Variables

CalcEngine supports single-letter variables: `a` through `z`. Press the **X,T,0,n** key to type `x` directly. To type other variables, press **ALPHA** and then the appropriate key (see the Keyboard Reference section).

A letter has a value only after you store one with `->` (see [Store Operator](#store-operator)), as on the TI-84 home screen: `5->X`, then `X^2` gives `= 25`.

An expression with a letter that holds no value, such as `x^2*sin(x)+x`, has no numeric value, and the calculator shows `X has no value`. Press **UP** to see what you can do with the expression instead: `d/dX(...)`, `int(...,X)`, `solve(...=0,X)`, `factor(...)` or `expand(...)`, or store a value with `5->X`.

### Operators

| Operator | Key | Example |
|----------|-----|---------|
| Addition | **+** | `3+5` |
| Subtraction | **-** | `10-4` |
| Multiplication | **x** (multiply key) | `3*7` |
| Division | **/** (divide key) | `15/4` |
| Exponent | **^** | `2^10` |
| Factorial | Type `!` via the menu | `5!` |

### Implicit Multiplication

CalcEngine understands implicit multiplication. You do not need to type the `*` symbol in many situations:

| You type | CalcEngine reads |
|----------|-----------------|
| `2x` | `2*x` |
| `3sin(x)` | `3*sin(x)` |
| `2pi` | `2*pi` |
| `(x+1)(x-1)` | `(x+1)*(x-1)` |
| `2(3+4)` | `2*(3+4)` |

### Parentheses

Use **(** and **)** to group expressions. CalcEngine features auto-close: when you type `(`, a matching `)` is automatically inserted. When you type `)` and the cursor is already at a `)`, it skips over it instead of adding a duplicate.

### Constants

| Constant | How to Type | Value |
|----------|------------|-------|
| pi | **2nd** + **.** | 3.14159... |
| e (Euler's number) | **2nd** + **(-)** | 2.71828... |
| infinity | Type `inf` | Used in limits and series |

### Previous Answer

Press **2nd** + **,** to insert `ans`, which represents the result of your last computation. This lets you chain calculations:

```
5+3           => 8
ans*2         => 16
sqrt(ans)     => 4
```

### Store Operator

Press **2nd** + **ENTER** to insert `->` (store). This stores a number in a letter, or a list in one of the list variables L1 through L6:

```
5->X          => = 5
X^2           => = 25
X+1->X        => = 6
{1,2,3,4,5}->L1
```

Stored letters:

- **Which letters:** A to Z, except E (the constant e) and, in complex mode, I. Lowercase and uppercase are the same letter.
- **Where they apply:** arithmetic entries use stored values, such as `X^2`, `2X+1`, or `sqrt(X+4)`. A root calculus or algebra command keeps letters symbolic, including its bounds. For example, `d/dx(x^2)` remains `2*X`. Commands inside arithmetic entries can use stored values. Local counters take precedence, so `0+sum(X,X,1,3)` leaves the stored X unchanged.
- **How long they last:** values survive quitting and restarting CalcEngine until you overwrite or clear them. Algebrix keeps them in its CE_STATE AppVar, separate from TI-OS variables. A value stored through TI-OS `STO→` is not visible here.
- **Clearing them:** press **MODE**, move down to **Stored vars** (it shows how many letters hold a value), press **RIGHT** to show `CLR`, then **ENTER**.
  If saving fails, the launcher reports `Settings save failed` and restores all settings and values from before the edit. Retry the edit.
- **Failed entries:** an evaluation error or failed response write keeps the previous stored values. A failed state save invalidates the response.
- **Depth limit:** ordinary arithmetic, and a store of an ordinary number, stops at 16 evaluator levels. A store whose value has a list or complex part, or that is a single command such as `sum(...)`, `int(...)` or `lim(...)`, runs in the lists and complex worker and stops at eight levels. Both report `Eval recursion limit`. Shorten the expression and try again.
- **Tiny values:** a store of an ordinary number is checked like any other entry. A value below about 1.17549e-38 (for example `1E-30*1E-30->X`) shows `Tiny value unsupported`, and X keeps its previous value. A value already stored that small is refused when used, including by a `sum(` or `int(` that reuses its letter (in either case) as the variable (they read the stored value to restore it afterwards). To clear it, store a new value such as `5->T`, or use MODE → Stored vars → CLR. Known limitation: a store whose value has a list or complex part, or that is a single command such as `sum(...)`, `int(...)` or `lim(...)`, is computed by the lists and complex worker, which does not make this check, so such a value can be stored as 0.
- **Upgrading:** transfer the launcher and all modules together. IPC version 6 resets older saved history, settings, and Ans to defaults.

---

## Keyboard Reference

CalcEngine maps the TI-84 Plus CE keys to mathematical functions. Many keys have a secondary function accessed by pressing **2nd** first.

### Top Row (Graph Keys)

| Key | Normal | 2nd + Key |
|-----|--------|-----------|
| **Y=** | Enter graph equation entry | — |
| **WINDOW** | Viewport settings (xmin, xmax, ymin, ymax) | — |
| **ZOOM** | Zoom presets / zoom controls | — |
| **TRACE** | Toggle trace mode on graph. On the CE launcher home screen, opens the full-result viewer like UP: the newest result, then its steps when MODE Show work is ON | — |
| **GRAPH** | Draw / refresh graph | Opens help screen |

These keys control the graphing engine. Press **Y=** to prepare a function for graphing, type the expression, then press **GRAPH** to draw it. Use **WINDOW** to adjust the viewport and **ZOOM** for zoom presets. Access all math functions through the function menu (**2nd+CLEAR**).

### Function Keys

| Key | Normal | 2nd + Key |
|-----|--------|-----------|
| **SIN** | `sin(` | `asin(` (inverse sine) |
| **COS** | `cos(` | `acos(` (inverse cosine) |
| **TAN** | `tan(` | `atan(` (inverse tangent) |
| **LN** | `ln(` (natural log) | `e^(` (exponential) |
| **LOG** | `log(` (base-10 log) | `10^(` (power of 10) |

### Math Keys

| Key | Normal | 2nd + Key |
|-----|--------|-----------|
| **x-squared** | `^2` (square) | `sqrt(` (square root) |
| **x-inverse** | `^(-1)` (reciprocal) | `abs(` (absolute value) |
| **MATH** | `nderiv(` (nth derivative) | `solve(` (solve equation) |

### Special Keys

| Key | Normal | 2nd + Key |
|-----|--------|-----------|
| **.** (decimal) | `.` | `pi` |
| **(-)** (negate) | `(-` (negative sign) | `e` (Euler's constant) |
| **,** (comma) | `,` | `ans` (previous answer) |
| **ENTER** | Evaluate / confirm | `->` (store) |
| **CLEAR** | Clear input (1st press) / back to input (from result) / cancel computation | Function menu (2nd+CLEAR) |
| **MODE** | Settings / format cycle | Quit (2nd+MODE) |
| **DEL** | Backspace (delete character) | Forward delete (2nd+DEL) |

### Navigation Keys

| Key | Function |
|-----|----------|
| **UP** | Scroll steps up / move within MathPrint fractions or exponents / browse history fallback |
| **DOWN** | Scroll steps down / move within MathPrint fractions or exponents / browse history fallback |
| **LEFT** | Move cursor left / previous page (in menus) |
| **RIGHT** | Move cursor right / next page (in menus) |

### ALPHA Mode

Press **ALPHA** to activate letter mode. The next key you press will produce a letter instead of its normal function. The ALPHA indicator **[A]** appears in the status bar.

ALPHA follows the green letters printed above the keys on the TI-84 Plus CE, so you can type a command such as `lim`, `solve` or `mean` by reading the keycaps. Letters appear in capitals on screen; command names are not case-sensitive, so `LIM(` and `lim(` are the same.

| Key row | Keys (after ALPHA) | Letters |
|---------|--------------------|---------|
| 1 | MATH, APPS, PRGM | A, B, C |
| 2 | x⁻¹, SIN, COS, TAN, ^ | D, E, F, G, H |
| 3 | x², `,`, `(`, `)`, ÷ | I, J, K, L, M |
| 4 | LOG, 7, 8, 9, × | N, O, P, Q, R |
| 5 | LN, 4, 5, 6, − | S, T, U, V, W |
| 6 | STO→, 1, 2 | X, Y, Z |
| 7 | 0 | space |

The keycaps also print θ (ALPHA+3), `"` (ALPHA++), `:` (ALPHA+.) and `?` (ALPHA+(−)). Algebrix has no use for these characters, so those key presses do nothing.

Examples: `lim` is ALPHA+`)`, ALPHA+x², ALPHA+÷. `solve` is ALPHA+LN, ALPHA+7, ALPHA+`)`, ALPHA+6, ALPHA+SIN.

The **X,T,0,n** key always types `x` without needing ALPHA.

---

## Macros & User Shortcuts

CalcEngine supports programmable expression shortcuts.

- Define a shortcut: `macro(name, body)`
- Define a function-style macro: `macro(name(x), body)` (up to 3 args)
- Remove a macro: `unmacro(name)`
- List macros: `macros()`

Examples:
- `macro(g,9.8)`
- `macro(sq(x),x*x)`
- `2*g` -> `19.6`
- `sq(3+1)` -> `16`

Macro expansion is bounded and recursive definitions are rejected.

---

## Calculus

### Solve first-order differential equations

Use `diffeq(rhs,x,y)` for the equation `dy/dx = rhs`. Use `odelinear(rhs,x,y)` to request the linear method directly.
The desktop parser also accepts `ode_linear`. Use `odelinear` on the calculator because its letter keys cannot enter an underscore.

The CE worker solves `diffeq(x-y,x,y)` with the integrating factor `exp(x)`.
Its answer is `y = (exp(x)*(x-1) + C)/(exp(x))`, equivalent to `y = x-1+C*exp(-x)`.
The result viewer shows the full answer when the home screen wraps it.

The bounded fallback handles constant, nonzero numeric `P` and recognized affine `Q` in `y' + P*y = Q`.
Recognized forms include `x`, `a*x`, `x+b`, and `a*x+b`, with numeric slope `a`.
This fallback does not enable recursive integration by parts or general substitution in the CE worker.
Other cases still depend on its existing integration rules.
For example, `odelinear(x^2-y,x,y)` can report that it cannot integrate `mu*Q`, although an elementary solution exists.
This message describes a solver limitation, not proof that the integral is nonelementary.

A very small constant coefficient of `y` keeps its sign, and the integrating factor keeps its variable. For example, `odelinear(1e-30*y+x,x,y)` uses the integrating factor `exp(-1e-30x)`.
Calculator numbers are 32-bit floats, so a constant such as `1/(1e-30)^2` is too large to store and stays an unevaluated quotient in the answer.
Other displayed numbers are float values cut to six digits; in this answer the calculator shows the value of `1/(-1e-30)` as `-9.99999e+29`. Evaluating the answer at ordinary `x` values can lose all precision, because it subtracts very large numbers.
This describes constant coefficients of `y` only. It is not a general guarantee for every expression with very small numbers.

### Derivatives

Compute the symbolic derivative of any expression.

**Syntax:** `d/dx(expression)`

**Natural shorthand:** `d/dx expression`

**Menu:** Available in **Calculus** page (2nd+CLEAR)

| Input | Result |
|-------|--------|
| `d/dx(x^3+2x)` | `3*x^2+2` |
| `d/dx(sin(x))` | `cos(x)` |
| `d/dx(e^x)` | `e^x` |
| `d/dx(ln(x))` | `1/x` |
| `d/dx(x^2*sin(x))` | `2*x*sin(x)+x^2*cos(x)` |

A small constant term that you type in a sum or difference is kept. For example, `d/dx(ln(x+5e-11))` gives `1/(x+5e-11)`, which is defined at `x = 0`.
Only a constant term that is exactly `0` is dropped from a sum or difference.
This rule is about constant terms. When like terms are combined and their coefficients add up to less than `1e-10` in size, the combined term can still become `0`: `d/dx(1e-11x^2+1e-11x^2)` gives `0`.
Constants that the calculator computes are 32-bit float values and can leave small rounding residues, such as `1.86265e-09x` for `d/dx(x^2*(0.1*0.1-0.01)+x)`. Because tiny constant terms are now kept, a residue can be very small: `d/dx(x^2+(1e-5*1e-5-1e-10)x)` gives `2x+(-6.93889e-18)`.

**Nth derivative:** Use `nderiv(expression, variable, n)` to compute the nth derivative.

| Input | Result |
|-------|--------|
| `nderiv(x^4,x,2)` | `12*x^2` (second derivative) |
| `nderiv(sin(x),x,3)` | `-cos(x)` (third derivative) |

**Shortcut:** Press **MATH** to insert `nderiv(`

### Integrals

Compute both indefinite and definite integrals symbolically.

**Indefinite:** `int(expression, variable)`

**Natural shorthand:** `int(expression)dvariable` (example: `int(x^2+1)dx`)

**Definite:** `int(expression, variable, lower, upper)`

**Menu:** Available in **Calculus** page (2nd+CLEAR)

| Input | Result |
|-------|--------|
| `int(x^2,x)` | `x^3/3 + C` |
| `int(sin(x),x)` | `-cos(x) + C` |
| `int(1/x,x)` | `ln(x) + C` |
| `int(x^2,x,0,3)` | `= 9` (the desktop build adds the detail line `F(x) = x^3/3`) |
| `int(e^x,x,0,1)` | `= 1.71828` |
| `int(e^(x^2),x,0,1)` | `= 1.46265  (numeric)` |

Definite integrals with no elementary antiderivative are evaluated numerically and labelled `(numeric)`. When the numeric grids disagree, or the result is mostly cancellation, the calculator answers `Integral failed: no convergence` instead of printing an unreliable value. Like any fixed-grid method (the TI-84's `fnInt` included), the numeric path samples a few hundred points, so a spike narrower than the spacing between them can go unseen; integrate over a small interval close around such a feature (for example from c-0.001 to c+0.001) and add the rest separately.

CalcEngine supports multiple integration techniques:
- Power rule
- Trigonometric integrals
- u-substitution
- Integration by parts
- Trig substitution
- Partial fractions
- Tabular integration (repeated IBP)

When a symbolic antiderivative cannot be found for a definite integral, CalcEngine falls back to numerical integration (Simpson's rule) and labels the result as `(numeric)`.

### Limits

Evaluate limits symbolically, including L'Hopital's Rule for indeterminate forms.

**Syntax:** `lim(expression, variable, point)` or `lim(expression, variable, point, direction)`

**Menu:** Available in **Calculus** page (2nd+CLEAR)

The direction argument is optional:
- Omit for two-sided limit
- `+` for right-hand limit
- `-` for left-hand limit

| Input | Result |
|-------|--------|
| `lim(sin(x)/x,x,0)` | `= 1` |
| `lim(1/x,x,0,+)` | `= +inf` |
| `lim(1/x,x,0,-)` | `= -inf` |
| `lim(1/x,x,0)` | `= DNE (does not exist)` |
| `lim((e^x-1)/x,x,0)` | `= 1` |

Possible results:
- A finite number
- `+inf` or `-inf`
- `DNE (does not exist)`

### Continuity Analysis

Classify the continuity of a function at a point.

**Syntax:** `continuity(expression, variable, point)`

| Input | Result |
|-------|--------|
| `continuity(x^2,x,2)` | `Continuous` |
| `continuity(1/x,x,0)` | `Infinite discontinuity` |
| `continuity(sin(x)/x,x,0)` | `Removable discontinuity` |

Result types: Continuous, Removable discontinuity, Jump discontinuity, Infinite discontinuity, Essential discontinuity.

### Summation

Evaluate finite and infinite sums.

**Syntax:** `sum(expression, variable, lower, upper)`

**Menu:** Available in **Calculus** page (2nd+CLEAR)

| Input | Result |
|-------|--------|
| `sum(n,n,1,100)` | `= 5050` |
| `sum(1/n^2,n,1,100)` | `= 1.63498` |
| `sum(1/n^2,n,1,inf)` | `CONVERGES (p-series)` |
| `sum(1/n,n,1,inf)` | `DIVERGES (p-series, p=1)` |

When the upper bound is infinity, CalcEngine automatically runs a convergence test wizard instead of trying to compute a numeric sum.

### Product Notation

Evaluate finite products.

**Syntax:** `prod(expression, variable, lower, upper)`

| Input | Result |
|-------|--------|
| `prod(n,n,1,5)` | `= 120` (5!) |
| `prod(2n,n,1,4)` | `= 384` |

### Convergence Tests

Test whether an infinite series converges or diverges.

**Syntax:** `converge(expression, variable)`

**Menu:** Available in **Calculus** page (2nd+CLEAR)

CalcEngine runs a battery of tests automatically:
- **Nth-term test** (divergence test)
- **Geometric series test**
- **p-series test**
- **Ratio test**
- **Root test**
- **Alternating series test**

| Input | Result |
|-------|--------|
| `converge(1/n^2,n)` | `CONVERGES (p-series)` |
| `converge(1/n,n)` | `DIVERGES (p-series, p=1)` |
| `converge(n!/n^n,n)` | `CONVERGES (ratio test)` |
| `converge((-1)^n/n,n)` | `CONVERGES (alternating)` |

Results include the name of the test that determined convergence or divergence, and an estimated partial sum when available.

### Taylor Series

Generate a Taylor polynomial approximation.

**Syntax:** `taylor(expression, variable, center, degree)`

**Menu:** Available in **Calculus** page (2nd+CLEAR)

| Input | Result |
|-------|--------|
| `taylor(sin(x),x,0,5)` | `x - x^3/6 + x^5/120` |
| `taylor(e^x,x,0,4)` | `1 + x + x^2/2 + x^3/6 + x^4/24` |
| `taylor(cos(x),x,0,6)` | `1 - x^2/2 + x^4/24 - x^6/720` |
| `taylor(ln(1+x),x,0,4)` | `x - x^2/2 + x^3/3 - x^4/4` |

The center defaults to 0 and degree defaults to 5 if omitted.

### Fourier Series

Generate a Fourier series approximation.

**Syntax:** `fourier(expression, variable, n_terms)`

**Menu:** Available in **Calculus** page (2nd+CLEAR)

| Input | Result |
|-------|--------|
| `fourier(x,x,3)` | Fourier approximation with 3 harmonics |
| `fourier(x^2,x,5)` | Fourier approximation with 5 harmonics |

Coefficients are computed numerically on the interval [-pi, pi]. The number of terms defaults to 5 if omitted.

### Tangent Lines

Find the equation of the tangent line to a function at a given point.

**Syntax:** `tangent(expression, variable, point)`

| Input | Result |
|-------|--------|
| `tangent(x^2,x,3)` | `y = 6*x - 9` |
| `tangent(sin(x),x,0)` | `y = 1*x` |
| `tangent(x^3,x,1)` | `y = 3*x - 2` |

Steps show f(a), f'(x), f'(a) (the slope), and the intercept calculation.

### Arc Length

Compute the arc length of a curve over an interval.

**Syntax:** `arclen(expression, variable, a, b)`

For parametric curves: `arclen(x_expr, y_expr, variable, a, b)`

| Input | Result |
|-------|--------|
| `arclen(x^2,x,0,1)` | Arc length of y=x^2 from x=0 to x=1 |

### Surface Area of Revolution

Compute the surface area when a curve is revolved about the x-axis.

**Syntax:** `surfarea(expression, variable, a, b)`

| Input | Result |
|-------|--------|
| `surfarea(x^2,x,0,1)` | Surface area of y=x^2 revolved, from 0 to 1 |

### Volumes of Revolution

CalcEngine supports three methods for computing volumes of solids of revolution:

**Disk method:** `disk(expression, variable, a, b)`
- Computes V = pi * integral of [f(x)]^2 dx

**Washer method:** `washer(outer, inner, variable, a, b)`
- Computes V = pi * integral of ([R(x)]^2 - [r(x)]^2) dx

**Shell method:** `shell(expression, variable, a, b)`
- Computes V = 2*pi * integral of x*f(x) dx

| Input | Result |
|-------|--------|
| `disk(x,x,0,2)` | Volume of y=x revolved (disk) |
| `washer(x,x^2,x,0,1)` | Volume between y=x and y=x^2 |
| `shell(x^2,x,0,1)` | Volume of y=x^2 (shell method) |

### Related Rates

Set up the chain rule derivative with respect to time for related rates problems.

**Syntax:** `relrate(expression)`

| Input | Result |
|-------|--------|
| `relrate(x^2+y^2)` | `d/dt = (2*x)*dx/dt + (2*y)*dy/dt` |

### Sequence Limit

Evaluate the limit of a sequence as n approaches infinity.

**Syntax:** `seqlim(expression, variable)`

---

## Algebra

### Solving Equations

CalcEngine can solve equations in multiple ways.

#### Auto-Solve (Just Type It)

The simplest way: type an equation with an `=` sign and press ENTER. CalcEngine detects the variable and solves automatically.

| Input | Result |
|-------|--------|
| `2x+3=7` | `x = 2` |
| `x^2-4=0` | `x = 2, -2` |
| `3x+7=22` | `x = 5` |
| `x^2-5x+6=0` | `x = 2, 3` |

#### Explicit Solve

Use `solve()` when you want to specify the variable explicitly.

**Syntax:** `solve(equation, variable)`

**Shortcut:** Press **2nd** + **MATH** to insert `solve(`

| Input | Result |
|-------|--------|
| `solve(x^2-4=0,x)` | `x = 2, -2` |
| `solve(x^3-8=0,x)` | `x = 2` |
| `solve(sin(x)=0.5,x)` | Solutions for x |

CalcEngine handles:
- Linear equations
- Quadratic equations (including complex roots when complex mode is on)
- Higher-degree polynomials via rational root theorem
- Trigonometric equations
- Newton's method as a fallback for transcendental equations

### Factoring

Factor polynomial expressions into linear and quadratic factors.

**Syntax:** `factor(expression)`

**Menu:** Available in **Algebra** page (2nd+CLEAR)

| Input | Result |
|-------|--------|
| `factor(x^2-1)` | `(x+1)*(x-1)` |
| `factor(x^2-5x+6)` | `(x-2)*(x-3)` |
| `factor(x^3-8)` | `(x-2)*(x^2+2x+4)` |
| `factor(2x^2+7x+3)` | `(2x+1)*(x+3)` |

### Expanding

Expand products, powers, and distribute terms.

**Syntax:** `expand(expression)`

**Menu:** Available in **Algebra** page (2nd+CLEAR)

| Input | Result |
|-------|--------|
| `expand((x+1)^2)` | `x^2+2x+1` |
| `expand((x+1)^3)` | `x^3+3x^2+3x+1` |
| `expand((2x-1)^4)` | `16x^4-32x^3+24x^2-8x+1` |
| `expand((x+1)(x-1))` | `x^2-1` |

### Completing the Square

Convert a quadratic expression to vertex form.

**Syntax:** `csquare(expression, variable)`

| Input | Result |
|-------|--------|
| `csquare(x^2+6x+2,x)` | `(x+3)^2 - 7` |
| `csquare(x^2-4x+1,x)` | `(x-2)^2 - 3` |
| `csquare(2x^2+8x+3,x)` | `2(x+2)^2 - 5` |

### Partial Fractions

Decompose a rational expression into partial fractions.

**Syntax:** `pfrac(expression, variable)`

| Input | Result |
|-------|--------|
| `pfrac(1/(x^2-1),x)` | `1/(2(x-1)) - 1/(2(x+1))` |
| `pfrac(x/(x^2+3x+2),x)` | Partial fraction decomposition |

### Descartes' Rule of Signs

Count the possible number of positive and negative real roots.

**Syntax:** `descartes(expression, variable)`

| Input | Result |
|-------|--------|
| `descartes(x^3-6x^2+11x-6,x)` | Sign change analysis |

### Polynomial Division

When you divide two polynomial expressions with `/`, CalcEngine automatically performs polynomial long division.

| Input | Result |
|-------|--------|
| `(x^3-1)/(x-1)` | `Q = x^2+x+1` |
| `(x^2+3x+5)/(x+1)` | `Q = x+2`, `R = 3` |

You can also use the explicit function: `polydiv(dividend, divisor, variable)`

### Systems of Equations

Solve systems of 2 or 3 linear equations using Cramer's Rule.

**Syntax:** `sys(a1,b1,c1, a2,b2,c2)` for a 2-equation system:
- a1*x + b1*y = c1
- a2*x + b2*y = c2

| Input | Result |
|-------|--------|
| `sys(2,1,5, 1,-1,1)` | `x = 2, y = 1` |
| `sys(1,1,3, 2,-1,0)` | `x = 1, y = 2` |

### Nonlinear Systems

Solve 2-equation nonlinear systems via Newton's method.

**Syntax:** `nlsolve(eq1, eq2, var1, var2)`

| Input | Result |
|-------|--------|
| `nlsolve(x^2+y^2-25, x-y, x, y)` | Intersection points |

### Triangle Solver

Solve triangles given sides and/or angles using the Law of Sines and Law of Cosines.

**Syntax:** `tri(a, b, c, A, B, C)` where a,b,c are sides and A,B,C are angles in degrees. Use 0 for unknown values.

| Input | Meaning | Result |
|-------|---------|--------|
| `tri(3,4,5,0,0,0)` | SSS: all sides known | Computes all angles |
| `tri(5,0,0,90,30,0)` | AAS: angle-angle-side | Computes missing parts |

### Inequality Solving

CalcEngine can solve linear and quadratic inequalities with `<`, `>`, `<=`, `>=` operators.

### Line from Two Points

**Syntax:** `line(x1, y1, x2, y2)`

| Input | Result |
|-------|--------|
| `line(1,2,3,6)` | `y = 2x + 0` |
| `line(0,1,2,5)` | `y = 2x + 1` |

### Line from Slope and Point

**Syntax:** `linept(slope, x1, y1)`

| Input | Result |
|-------|--------|
| `linept(3,1,5)` | `y = 3x + 2` |

### Parabola from Vertex and Point

**Syntax:** `parabola(vx, vy, px, py)` where (vx,vy) is the vertex and (px,py) is a point on the parabola.

| Input | Result |
|-------|--------|
| `parabola(0,0,1,1)` | `y = 1(x-0)^2 + 0` |

### Coordinate Geometry

| Function | Syntax | Description |
|----------|--------|-------------|
| Midpoint | `midpoint(x1,y1,x2,y2)` | Midpoint between two points |
| Distance | `distance(x1,y1,x2,y2)` | Distance between two points |
| Slope | `slope(x1,y1,x2,y2)` | Slope between two points |

| Input | Result |
|-------|--------|
| `midpoint(0,0,4,6)` | `Midpoint = (2, 3)` |
| `distance(0,0,3,4)` | `Distance = 5` |
| `slope(1,2,3,6)` | `Slope = 2` |

### Arithmetic and Geometric Series

**Arithmetic sum:** `arithsum(first_term, common_difference, n_terms)`

**Geometric sum:** `geomsum(first_term, common_ratio, n_terms)`

| Input | Result |
|-------|--------|
| `arithsum(1,2,10)` | Sum of 1+3+5+7+...+19 |
| `geomsum(1,0.5,10)` | Sum of 1 + 0.5 + 0.25 + ... |

### Decimal to Fraction

**Syntax:** `tofrac(decimal_value)`

| Input | Result |
|-------|--------|
| `tofrac(0.333)` | `1/3` |
| `tofrac(0.375)` | `3/8` |
| `tofrac(0.142857)` | `1/7` |

---

## Linear Algebra

### Entering Matrices

Matrices are entered using `mat()` with semicolons separating rows. The keypad has no `;` key: on the calculator, press **APPS** to type it (ALPHA+APPS still types `B`).

**Syntax:** `mat(a, b; c, d)` for a 2x2 matrix, `mat(a,b,c; d,e,f; g,h,i)` for 3x3, etc.

Matrices can be up to 6x6 in size.

| Input | Meaning |
|-------|---------|
| `mat(1,2;3,4)` | 2x2 matrix: [[1,2],[3,4]] |
| `mat(1,0,0;0,1,0;0,0,1)` | 3x3 identity matrix |

### Determinant

**Syntax:** `det(mat(...))`

| Input | Result |
|-------|--------|
| `det(mat(1,2;3,4))` | `det = -2` |
| `det(mat(2,3;1,4))` | `det = 5` |

### Matrix Inverse

**Syntax:** `inv(mat(...))`

| Input | Result |
|-------|--------|
| `inv(mat(1,2;3,4))` | Inverse matrix |

Returns an error if the matrix is singular (determinant is zero).

### Row Reduced Echelon Form (RREF)

**Syntax:** `rref(mat(...))`

| Input | Result |
|-------|--------|
| `rref(mat(1,2,3;4,5,6;7,8,10))` | Reduced row echelon form |

Use RREF to solve systems of linear equations by augmenting the coefficient matrix with the constant column.

### Transpose

**Syntax:** `transpose(mat(...))`

| Input | Result |
|-------|--------|
| `transpose(mat(1,2,3;4,5,6))` | 3x2 matrix: [[1,4],[2,5],[3,6]] |

### Eigenvalues and Eigenvectors

CalcEngine finds the real eigenvalues of a square matrix up to 3x3. They are
listed with multiplicity, largest first, with one eigenvector per eigenvalue.
Each eigenvector is scaled so that its largest component has magnitude 1. On
the calculator, both functions run in the CEMAT module.

| Input | Result |
|-------|--------|
| `eigenval(mat(2,1;1,2))` | `lambda = {3, 1}` |
| `eigenvec(mat(4,1;2,3))` | `v1(lambda=5)=[1,1]`, `v2(lambda=2)=[-0.5,1]` |
| `eigenval(mat(2,1,0;1,2,0;0,0,3))` | `lambda = {3, 3, 1}` |
| `eigenval(mat(0,-1,0;1,0,0;0,0,2))` | `lambda = {2} + complex pair` |
| `eigenval(mat(0,-1;1,0))` | `No real eigenvalues` |

- **Argument:** it must be a `mat(...)` or `mat2(...)` literal. For example,
  `eigenval(inv(...))` answers `Expected mat(...) argument`.
- **Output layout:** `eigenvec` puts each vector on its own line. The home
  screen shows only the first line; press UP or TRACE to see the others.
- **Defective matrices:** a repeated eigenvalue can have fewer independent
  eigenvectors than its multiplicity. The missing ones are shown as
  `same as v1`, for example `eigenvec(mat2(1,1,0,1))` answers
  `v1(lambda=1)=[1,0]` and `v2(lambda=1)=same as v1`.
- **Very small or large eigenvalues** are shown with an exponent and 5
  significant digits, for example `3e-8`, `1.2346e-4` or `5e30`. Vector
  components are scaled so the largest is 1, and are shown with up to 6
  decimals.
- **Entering a matrix on the calculator:** the keypad has no `;` key, so you
  can only enter a 2x2 matrix there. Type it row by row as `mat2(a,b,c,d)`,
  for example `eigenval(mat2(2,1,1,2))`. The 3x3 examples above work only in
  the desktop build.
- **Precision:** the calculator uses 32-bit floats, so results are accurate to
  about 6 digits. Triangular matrices give their diagonal exactly.
  - **Eigenvalues much smaller than the entries:** an eigenvalue is accurate
    to a few millionths of the largest entry, not of its own size, and one
    that close to zero can be shown as 0. A 2x2 whose determinant is 0 to
    within rounding shows an exact 0 (unless its entries are so far apart,
    such as 1e-20 next to 1e20, that the products underflow). When both
    eigenvalues of a 2x2 are small, the smaller one can be shown as 0 up to
    about 1e-4 of the largest entry.
  - **Close eigenvalues:** two eigenvalues closer than about 0.3% of the
    spread of the matrix entries can be shown as one repeated value, with
    `not found` for its eigenvectors. For three eigenvalues that are all
    that close to each other, the limit is about 1%.
  - **Nearly real complex pairs:** a complex pair whose imaginary part is that
    small can be shown as a repeated real value.
  - **Large, strongly skewed matrices** (large entries but small eigenvalues)
    reach these limits sooner, and a repeated eigenvalue can then come out as
    two slightly different values.
  - **Unchecked eigenvectors:** an eigenvector that fails the `A v = lambda v`
    check is shown as `not found`.
- **Limits:** an entry larger than 1e37 answers `Eigen: entry out of range`.

### LU Decomposition

CalcEngine can decompose a square matrix A into lower triangular L and upper triangular U factors such that A = LU.

### Dot Product and Cross Product

Vectors are entered as single-row or single-column matrices.

| Input | Result |
|-------|--------|
| `dot(mat(1,2,3),mat(4,5,6))` | Dot product |
| `cross(mat(1,2,3),mat(4,5,6))` | Cross product (3D vectors only) |

### Rank and Nullity

CalcEngine can compute the rank and nullity of a matrix.

---

## Statistics

### Descriptive Statistics

CalcEngine provides the standard suite of statistical functions. All take a comma-separated list of numbers.

| Function | Syntax | Description |
|----------|--------|-------------|
| Mean | `mean(x1,x2,...)` | Arithmetic mean |
| Median | `median(x1,x2,...)` | Middle value (sorts data) |
| Mode | `mode(x1,x2,...)` | Most frequent value |
| Std Dev | `stdev(x1,x2,...)` | Sample standard deviation |
| Variance | `var(x1,x2,...)` | Sample variance |

| Input | Result |
|-------|--------|
| `mean(1,2,3,4,5)` | `mean = 3` |
| `median(1,3,5,7,9)` | `median = 5` |
| `stdev(2,4,4,4,5,5,7,9)` | `stdev = 2` |

### Regression

CalcEngine supports multiple regression models. Data is entered as paired x,y values.

**Linear regression:** `linreg(x1,y1,x2,y2,...)`

| Input | Result |
|-------|--------|
| `linreg(1,2,2,4,3,5,4,8)` | `y = 1.9x + -0.1`, `r = 0.9847` |

**Other regression types:**

| Function | Syntax | Model |
|----------|--------|-------|
| Quadratic | `quadreg(x1,y1,...)` | y = ax^2 + bx + c |
| Exponential | `expreg(x1,y1,...)` | y = a*b^x |
| Power | `pwrreg(x1,y1,...)` | y = a*x^b |
| Logarithmic | `lnreg(x1,y1,...)` | y = a + b*ln(x) |

### Combinatorics

| Function | Syntax | Description |
|----------|--------|-------------|
| nCr | `ncr(n,r)` | Combinations: n! / (r!(n-r)!) |
| nPr | `npr(n,r)` | Permutations: n! / (n-r)! |

| Input | Result |
|-------|--------|
| `ncr(10,3)` | `ncr = 120` |
| `npr(10,3)` | `npr = 720` |

### GCD and LCM

| Input | Result |
|-------|--------|
| `gcd(24,36)` | `gcd = 12` |
| `lcm(12,18)` | `lcm = 36` |

### 2-Variable Statistics

**Covariance:** `covar(x1,y1,x2,y2,...)`

**Full 2-var stats:** `2varstats(x1,y1,x2,y2,...)` returns means, standard deviations, correlation, and more.

### Hypothesis Tests

CalcEngine provides a full suite of hypothesis tests. All tests report a test statistic and p-value, with step-by-step work.

#### Z-Test (One Sample)

**Syntax:** `ztest(x_bar, mu0, sigma, n)`

Tests H0: mu = mu0 against H1: mu != mu0 (two-sided).

| Input | Result |
|-------|--------|
| `ztest(52,50,5,30)` | z-statistic and p-value |

#### T-Test (One Sample)

**Syntax:** `ttest(x_bar, mu0, s, n)`

| Input | Result |
|-------|--------|
| `ttest(68,65,3.5,25)` | t-statistic, p-value, df |

#### Two-Sample Z-Test

**Syntax:** `2sampztest(x_bar1, sigma1, n1, x_bar2, sigma2, n2)`

#### Two-Sample T-Test

**Syntax:** `2sampttest(x_bar1, s1, n1, x_bar2, s2, n2)`

#### One-Proportion Z-Test

**Syntax:** `1propztest(successes, n, p0)`

#### Two-Proportion Z-Test

**Syntax:** `2propztest(x1, n1, x2, n2)`

#### Chi-Squared Test

**Syntax:** `chi2test(observed_values, expected_values)`

### Confidence Intervals

#### Z-Interval

**Syntax:** `zinterval(x_bar, sigma, n, confidence_level)`

| Input | Result |
|-------|--------|
| `zinterval(50,5,30,0.95)` | Lower bound, upper bound, margin of error |

#### T-Interval

**Syntax:** `tinterval(x_bar, s, n, confidence_level)`

#### One-Proportion Z-Interval

**Syntax:** `1propzint(successes, n, confidence_level)`

#### Two-Proportion Z-Interval

**Syntax:** `2propzint(x1, n1, x2, n2, confidence_level)`

#### Two-Sample Z-Interval

**Syntax:** `2sampzint(x_bar1, sigma1, n1, x_bar2, sigma2, n2, confidence_level)`

#### Two-Sample T-Interval

**Syntax:** `2samptint(x_bar1, s1, n1, x_bar2, s2, n2, confidence_level)`

---

## Finance

CalcEngine includes a complete TVM (Time Value of Money) solver and amortization functions.

### TVM Functions

All TVM functions use the standard financial convention where cash inflows are positive and cash outflows are negative.

#### Present Value

**Syntax:** `pv(rate, nper, pmt, fv)`

Computes the present value of an investment or loan.

| Input | Result |
|-------|--------|
| `pv(0.05,10,100,0)` | Present value of $100/period for 10 periods at 5% |
| `pv(0.08,10,-500,0)` | Present value of annuity |

#### Future Value

**Syntax:** `fv(rate, nper, pmt, pv)`

Computes the future value of an investment or loan.

| Input | Result |
|-------|--------|
| `fv(0.05,10,100,0)` | Future value of $100/period for 10 periods at 5% |
| `fv(0.06,20,-200,0)` | Future value of savings plan |

#### Payment

**Syntax:** `pmt(rate, nper, pv, fv)`

Computes the periodic payment amount.

| Input | Result |
|-------|--------|
| `pmt(0.05,10,1000,0)` | Payment on a $1000 loan at 5% for 10 periods |
| `pmt(0.005,360,200000,0)` | Monthly mortgage payment (0.5%/month, 30 years) |

#### Number of Periods

**Syntax:** `nper(rate, pmt, pv, fv)`

| Input | Result |
|-------|--------|
| `nper(0.05,100,1000,0)` | Number of periods to pay off $1000 at 5% with $100 payments |

#### Interest Rate

**Syntax:** `rate(nper, pmt, pv, fv)`

Solves for the periodic interest rate using Newton's method.

| Input | Result |
|-------|--------|
| `rate(10,-100,1000,0)` | Rate for $1000 loan, $100/period, 10 periods |

### Amortization

#### Balance

**Syntax:** `bal(payment_number, rate, pv, pmt)`

Computes the remaining balance after a given number of payments.

#### Sum of Principal

**Syntax:** `sumprn(pmt1, pmt2, rate, pv, pmt)`

Computes the total principal paid between payment numbers pmt1 and pmt2.

#### Sum of Interest

**Syntax:** `sumint(pmt1, pmt2, rate, pv, pmt)`

Computes the total interest paid between payment numbers pmt1 and pmt2.

### Interest Rate Conversion

#### Nominal Rate

**Syntax:** `nom(effective_rate, compounding_periods)`

Converts an effective annual rate to a nominal rate.

| Input | Result |
|-------|--------|
| `nom(0.05,12)` | Nominal rate equivalent to 5% effective with monthly compounding |

#### Effective Rate

**Syntax:** `eff(nominal_rate, compounding_periods)`

Converts a nominal rate to an effective annual rate.

| Input | Result |
|-------|--------|
| `eff(0.05,12)` | Effective rate for 5% nominal with monthly compounding |

### Payment Timing

The payment timing (BEGIN or END of period) is controlled in Settings. The default is END mode. Toggle this in the Settings menu under "Payment mode" (visible as the `pmt_at_begin` setting).

---

## Probability Distributions

CalcEngine supports a wide range of probability distributions.

### Normal Distribution

| Function | Syntax | Description |
|----------|--------|-------------|
| Normal PDF | `normalpdf(x, mu, sigma)` | Probability density at x |
| Normal CDF | `normalcdf(lo, hi, mu, sigma)` | P(lo < X < hi) |
| Inverse Normal | `invnorm(area, mu, sigma)` | Value at given cumulative area |

| Input | Result |
|-------|--------|
| `normalcdf(-1,1,0,1)` | `= 0.6827` (P within 1 std dev) |
| `normalcdf(-1.96,1.96,0,1)` | `= 0.95` (P within ~2 std dev) |
| `invnorm(0.975,0,1)` | `= 1.96` |
| `invnorm(0.5,100,15)` | `= 100` (median of N(100,15)) |

### Binomial Distribution

| Function | Syntax | Description |
|----------|--------|-------------|
| Binomial PDF | `binompdf(n, p, k)` | P(X = k) |
| Binomial CDF | `binomcdf(n, p, k)` | P(X <= k) |

| Input | Result |
|-------|--------|
| `binompdf(10,0.5,5)` | P(X=5) for 10 trials, p=0.5 |
| `binomcdf(10,0.5,5)` | P(X<=5) for 10 trials, p=0.5 |

### Poisson Distribution

| Function | Syntax | Description |
|----------|--------|-------------|
| Poisson PDF | `poissonpdf(lambda, k)` | P(X = k) |
| Poisson CDF | `poissoncdf(lambda, k)` | P(X <= k) |

| Input | Result |
|-------|--------|
| `poissonpdf(3,2)` | P(X=2) for lambda=3 |
| `poissoncdf(5,3)` | P(X<=3) for lambda=5 |

### Geometric Distribution

| Function | Syntax | Description |
|----------|--------|-------------|
| Geometric PDF | `geometpdf(p, k)` | P(X = k) (first success on trial k) |
| Geometric CDF | `geometcdf(p, k)` | P(X <= k) |

| Input | Result |
|-------|--------|
| `geometpdf(0.3,4)` | P(first success on trial 4) |
| `geometcdf(0.3,4)` | P(first success by trial 4) |

### Student's t-Distribution

| Function | Syntax | Description |
|----------|--------|-------------|
| t CDF | `tcdf(lo, hi, df)` | P(lo < T < hi) with df degrees of freedom |
| Inverse t | `invt(area, df)` | t-value at given cumulative area |

| Input | Result |
|-------|--------|
| `tcdf(-2,2,10)` | P(-2 < T < 2) with 10 df |
| `invt(0.975,20)` | Critical t-value for 95% two-sided, 20 df |

### Chi-Squared Distribution

| Function | Syntax | Description |
|----------|--------|-------------|
| Chi2 CDF | `chi2cdf(lo, hi, df)` | P(lo < X < hi) |
| Inverse Chi2 | `invchi2(area, df)` | Chi-squared value at given area |

| Input | Result |
|-------|--------|
| `chi2cdf(0,3.84,1)` | P(X < 3.84) with 1 df |

### F-Distribution

| Function | Syntax | Description |
|----------|--------|-------------|
| F PDF | `fpdf(x, df1, df2)` | F density at x |
| F CDF | `fcdf(lo, hi, df1, df2)` | P(lo < F < hi) |
| Inverse F | `invf(area, df1, df2)` | F-value at given area |

---

## Other Features

### Complex Numbers

Enable complex mode in Settings (change Complex from OFF to Rect or Polar).

Type complex numbers using `i` for the imaginary unit:

| Input | Result |
|-------|--------|
| `(3+2i)*(1-i)` | Complex product |
| `(2+3i)^2` | Complex square |
| `sqrt(-4)` | `2i` (when complex mode is on) |

**Complex functions:**

| Function | Syntax | Description |
|----------|--------|-------------|
| Real part | `real(z)` | Real component |
| Imaginary part | `imag(z)` | Imaginary component |
| Conjugate | `conj(z)` | Complex conjugate |
| Angle | `angle(z)` | Argument (angle) of z |
| Absolute value | `abs(z)` | Modulus of z |

### Polar and Rectangular Conversion

| Function | Syntax | Description |
|----------|--------|-------------|
| Polar to Rectangular | `polar(r, theta)` | Convert (r, theta) to (x, y) |
| Rectangular to Polar | `rect(x, y)` | Convert (x, y) to (r, theta) |

| Input | Result |
|-------|--------|
| `polar(5,pi/6)` | `x = 4.33013`, `y = 2.5` |
| `rect(3,4)` | `r = 5`, `theta = 0.9273` |

### Lists (L1 through L6)

CalcEngine provides six named list variables (L1 through L6) for storing and manipulating data.

**Creating lists:**

```
{1,2,3,4,5}->L1
```

**Accessing elements:** `L1(3)` returns the 3rd element (1-indexed).

**List functions:**

| Function | Syntax | Description |
|----------|--------|-------------|
| Cumulative sum | `cumsum(L1)` | Running total |
| Differences | `deltalist(L1)` | Consecutive differences |
| Sort ascending | `sorta(L1)` | Sort low to high |
| Sort descending | `sortd(L1)` | Sort high to low |
| Dimension | `dim(L1)` | Number of elements |
| Sequence | `seq(expr, var, start, end, step)` | Generate a list |

**Element-wise arithmetic:** Lists support `+`, `-`, `*`, `/`, and `^` with other lists or scalars.

### Piecewise Functions

Define piecewise functions for evaluation.

**Syntax:** `piecewise(expr1, cond1, expr2, cond2, ..., default)`

### Unit Circle

Look up exact trigonometric values for standard angles.

**Syntax:** `unitcirc(angle)`

The angle is in the current angle mode (radians by default). Standard angles include 0, pi/6, pi/4, pi/3, pi/2, and their equivalents in all four quadrants.

| Input | Result |
|-------|--------|
| `unitcirc(pi/6)` | `sin=1/2, cos=sqrt(3)/2, tan=sqrt(3)/3` |
| `unitcirc(pi/4)` | `sin=sqrt(2)/2, cos=sqrt(2)/2, tan=1` |

### Previous Answer (ans)

The result of your last computation is stored in `ans`. Press **2nd** + **,** to insert it.

| Input | Result |
|-------|--------|
| `5*8` | `= 40` |
| `ans+10` | `= 50` |
| `sqrt(ans)` | `= 7.07107` |

### Store (->)

Store a number in a letter (`5->X`), or a list in L1 through L6, using the store operator `->` (see [Store Operator](#store-operator)). Press **2nd** + **ENTER** to type `->`.

```
{10,20,30}->L1
mean(L1)          => 20
```

### Recursive Sequences

Evaluate a recursive sequence: s(0) = init, s(n) = f(s(n-1)).

**Syntax:** `recseq(expression, variable, initial_value, num_terms)`

| Input | Result |
|-------|--------|
| `recseq(x/2+1,x,10,5)` | Computes 5 terms starting from 10 |

### Numerical Methods

CalcEngine provides three root-finding numerical methods:

**Newton's method:** `newton(expression, variable, initial_guess)`

Iteratively refines a guess using the derivative.

**Bisection method:** `bisect(expression, variable, a, b)`

Finds a root in the interval [a,b] by repeatedly halving.

**Secant method:** `secant(expression, variable, x0, x1)`

Similar to Newton's method but uses a secant line instead of the derivative.

| Input | Result |
|-------|--------|
| `newton(x^2-2,x,1)` | `= 1.41421` (approximation of sqrt(2)) |
| `bisect(x^3-x-1,x,1,2)` | Root between 1 and 2 |

### Number Base Conversions

| Function | Syntax | Description |
|----------|--------|-------------|
| Decimal | `dec(value)` | Convert to decimal |
| Hexadecimal | `hex(value)` | Convert to hexadecimal |
| Binary | `bin(value)` | Convert to binary |
| Octal | `oct(value)` | Convert to octal |

### Additional Math Functions

| Function | Syntax | Description |
|----------|--------|-------------|
| Floor | `floor(x)` | Round down to integer |
| Ceiling | `ceil(x)` | Round up to integer |
| Round | `round(x)` | Round to nearest integer |
| Integer part | `ipart(x)` | Integer part |
| Fractional part | `fpart(x)` | Fractional part |
| Sign | `sign(x)` | -1, 0, or 1 |
| Modulo | `mod(a,b)` | Remainder of a/b |
| Random integer | `randint(lo,hi)` | Random integer in [lo,hi] |

### Hyperbolic Functions

CalcEngine supports all six hyperbolic functions and their inverses:

`sinh(`, `cosh(`, `tanh(`, `sech(`, `csch(`, `coth(`

`asinh(`, `acosh(`, `atanh(`

### Reciprocal Trig Functions

`sec(`, `csc(`, `cot(`, `asec(`, `acsc(`, `acot(`

### Multivariable Calculus

| Function | Syntax | Description |
|----------|--------|-------------|
| Partial derivative | `pdiff(expr, var1, var2, ...)` | Mixed partial derivatives |
| Gradient | `gradient(expr, var1, var2)` | Gradient vector |
| Double integral | `dblint(expr, x, xlo, xhi, y, ylo, yhi)` | Double integral |
| Critical points | `critical(expr, var)` | Find critical points |

---

## Settings

Press **MODE** to open the Settings menu. Use **UP/DOWN** to navigate between settings and **ENTER** to toggle each one. Press **MODE** or **CLEAR** to close settings.

### Available Settings

| Setting | Options | Default | Description |
|---------|---------|---------|-------------|
| **Angle** | Radians / Degrees | Radians | Angle measurement for trig functions |
| **Steps** | ON / OFF | ON | Show step-by-step work |
| **Precision** | 1-9 digits | 6 | Number of significant digits displayed |
| **Complex** | OFF / Rect / Polar | OFF | Complex number mode |
| **Step detail** | All / Compact | All | Level of detail in step-by-step work |
| **Stored vars** | count / CLR | — | How many letters hold a value; RIGHT then ENTER clears them all |

### Angle Mode

- **Radians** (default): `sin(pi/2) = 1`, angles in radians.
- **Degrees**: `sin(90) = 1`, angles in degrees.

The current mode is shown in the status bar as **RAD** or **DEG**.

### Steps

- **ON** (default): Every computation that supports it will display step-by-step work below the answer.
- **OFF**: Only the final answer is shown.

### Precision

Controls how many significant digits are shown in results. Range: 1 to 9. Default: 6. Press ENTER repeatedly to cycle through values.

### Complex Mode

- **OFF** (default): All computations are real-valued. `sqrt(-1)` produces an error.
- **Rect**: Complex results displayed as `a + bi`.
- **Polar**: Complex results displayed in polar form.

### Step Detail

- **All** (default): Shows every intermediate step.
- **Compact**: Shows only key steps, reducing clutter for long computations.

---

## Menu System

CalcEngine has a layered menu system accessed through **2nd** + **CLEAR**. Pressing **2nd** + **CLEAR** repeatedly cycles through three views: the function menu, templates, and examples.

### Function Menu (2nd+CLEAR)

The main function menu has **8 category pages**. Use **LEFT/RIGHT** to switch pages and **UP/DOWN** to select a function. Press **ENTER** to insert the selected function into the input line. The menu opens to the **Recents** page if it has entries, otherwise to **Calculus**.

**Pages:**

1. **RECENTS** (up to 10 items): Your most recently used functions, most recent first. Duplicates are moved to the top instead of added again.
2. **CALCULUS** (8 items): d/dx, int, definite int, lim, sum, taylor, tangent, nderiv
3. **ALGEBRA** (8 items): solve, factor, expand, csquare, pfrac, tofrac, nlsolve, descartes
4. **STATISTICS** (6 items): mean, stdev, median, linreg, ncr, npr
5. **LINEAR ALGEBRA** (5 items): mat, det, inv, rref, transpose
6. **PROBABILITY** (6 items): normalcdf, invnorm, binompdf, binomcdf, tcdf, chi2cdf
7. **FINANCE** (7 items): pv, fv, pmt, nper, rate, nom, eff
8. **OTHER** (6 items): polar, rect, arclen, unitcirc, line, recseq

### Templates (2nd+CLEAR again)

Press **2nd** + **CLEAR** while the function menu is open to switch to the template browser. Templates are pre-filled expressions you can insert and modify.

**Pages:**

1. **CALC TEMPLATES** (8 items): Pre-built derivative, integral, limit, sum, Taylor, tangent, and convergence expressions
2. **ALGEBRA TEMPLATES** (8 items): Solve quadratic, factor, expand, complete square, partial fractions, systems, triangle
3. **STAT TEMPLATES** (8 items): Mean, std dev, linear regression, normal CDF, inverse normal, binomial, nCr, z-test
4. **FINANCE TEMPLATES** (7 items): Present value, future value, payment, periods, rate, nominal, effective
5. **QUICK TEMPLATES** (6 items): Matrix determinant, matrix inverse, unit circle, to fraction, complex, GCD

Use **UP/DOWN** to select a template and **ENTER** to fill it into the input line.

### Examples (2nd+CLEAR a third time)

Press **2nd** + **CLEAR** while in the template browser to switch to the example browser. Examples are real math problems with their CalcEngine input pre-filled.

**Pages:**

1. **CALCULUS EXAMPLES** (8 problems): Derivatives, integrals, limits, convergence, Taylor, tangent lines
2. **ALGEBRA EXAMPLES** (8 problems): Solving equations, factoring, expanding, systems, triangles, unit circle
3. **OTHER EXAMPLES** (8 problems): Normal CDF, matrix determinant, regression, finance, combinatorics, fractions, complex numbers

Select an example and press **ENTER** to load it into the input. Then press **ENTER** again to evaluate.

### Navigating Menus

| Key | Action in Menu |
|-----|---------------|
| **UP/DOWN** | Move cursor between items |
| **LEFT/RIGHT** | Switch between pages |
| **ENTER** | Insert selected item / load example |
| **CLEAR** | Close menu and return to input |

---

## Step-by-Step Work

CalcEngine shows detailed step-by-step solutions for calculus, algebra, and many other operations when Steps are turned ON in Settings.

### How Steps Are Displayed

After evaluating an expression that generates steps:

1. The **answer** appears in blue at the top of the work area.
2. A **"Steps: (N total)"** header appears in green below the answer.
3. Individual steps are listed below, with alternating black and gray text for readability.

### Reveal Mode (Step-by-Step Navigation)

When steps are available, CalcEngine enters **reveal mode** automatically:

1. Only the first step is shown initially.
2. Press **ENTER** to reveal the next step.
3. The header shows your progress: **"Steps: (2/7)"** means 2 of 7 steps revealed.
4. A blue prompt at the bottom reads **"[ENTER] next step"**.
5. Once all steps are shown, the prompt changes to **"All steps shown."** in green.
6. Press **CLEAR** at any time to exit reveal mode and show all steps at once.

### Scrolling Steps

If there are more steps than fit on screen:

- Use **UP/DOWN** arrow keys to scroll through the step list.
- An orange **^** appears at the top right when you can scroll up.
- An orange **v** appears at the bottom right when you can scroll down.

### Step Detail Level

In Settings, you can choose between:
- **All**: Every intermediate simplification and rule application is shown.
- **Compact**: Only the key transformation steps are shown, reducing the total number of steps.

---

## Answer Format

CalcEngine displays numerical results in three formats. When viewing a result, press **MODE** to cycle through them.

### Decimal (Default)

Results are shown with up to 6 significant digits (configurable in Settings).

```
= 3.14159
= 0.333333
= 1234.56
```

### Fraction

Results are converted to the nearest simple fraction using the continued fraction algorithm. The denominator is limited to 10000 for practical display.

```
= 1/3
= 22/7
= 3/8
```

If no suitable fraction is found, a note is shown: `= 0.333333 (no exact fraction)`

### Scientific Notation

Results are shown in standard scientific notation.

```
= 3.141593e+00
= 1.000000e+06
= 5.000000e-04
```

### Cycling Formats

Press **MODE** when viewing a result to cycle: Decimal -> Fraction -> Scientific -> Decimal -> ...

This only works when the most recent result was a numeric value. Symbolic results (like derivative expressions) do not support format cycling.

### Pretty Math Display

Result and step text is automatically rendered in a math-friendly form:

- `sqrt(...)` displays as `√(...)`
- exponents like `x^2` display with superscripts (`x²`)
- indexed variables like `a_1` display with subscripts (`a₁`)
- bounded forms render in LaTeX style: `int(f,x,a,b)` -> `∫_{a}^{b} f dx`, `sum(expr,n,1,10)` -> `∑_{n=1}^{10} expr`, `prod(expr,k,1,m)` -> `∏_{k=1}^{m} expr`
- inline fractions use the fraction slash (`a⁄b`) when spacing permits

---

## Troubleshooting

### Common Errors

#### "Check your input for typos or missing operators"

**Cause:** The tokenizer could not recognize part of your input.
**Fix:** Check for misspelled function names, missing parentheses, or unsupported characters.

#### "Could not understand expression"

**Cause:** The parser could not build a valid expression tree from your tokens.
**Fix:** Ensure your expression has balanced parentheses and correct syntax. Check that function arguments are separated by commas.

#### "Cannot divide by zero"

**Cause:** An expression evaluated to a division by zero.
**Fix:** Check your expression for denominators that could be zero.

#### "Value outside function domain (e.g. sqrt of negative)"

**Cause:** A function received an argument outside its valid domain.
**Fix:** Common causes include `sqrt(negative)` (enable complex mode in Settings), `ln(0)`, or `asin(2)`.

#### "Result too large"

**Cause:** The computation produced a number that exceeds the range of 32-bit floating point.
**Fix:** Simplify your expression or use smaller numbers.

#### "Expression too long (max 128 tokens)"

**Cause:** Your expression has more than 128 tokens (numbers, operators, functions, etc.).
**Fix:** Break your computation into smaller steps, using `ans` to chain results.

#### "Cannot find symbolic antiderivative"

**Cause:** The integration engine could not find a closed-form antiderivative.
**Fix:** For definite integrals, CalcEngine will fall back to numerical integration. For indefinite integrals, the expression may not have an elementary antiderivative.

#### "Cannot differentiate this expression"

**Cause:** The differentiation engine encountered an expression it cannot handle.
**Fix:** Check that your expression uses supported functions and operators.

#### "Matrix is singular (no inverse)"

**Cause:** The matrix has a determinant of zero and cannot be inverted.
**Fix:** Verify your matrix entries. Singular matrices do not have inverses.

#### "Limit needs a point"

**Cause:** The `lim()` function requires at least three arguments.
**Fix:** Use the syntax `lim(expression, variable, point)`.

### Tips for Avoiding Errors

1. **Use parentheses generously.** When in doubt, add parentheses to clarify grouping: `sin(x)^2` vs `sin(x^2)`.

2. **Check angle mode.** If `sin(90)` is not giving you 1, switch to Degrees mode in Settings.

3. **Watch the node counter.** The status bar shows `N:x/512`. If you are near 512, your expression is very complex. Simplify or break it into parts.

4. **Use the menu system.** If you are unsure of the syntax for a function, press **2nd** + **CLEAR** to browse the function menu. Each item shows the function name and its purpose.

5. **Context-sensitive hints.** As you type a function name, a hint appears in the work area showing the expected syntax and arguments.

6. **Use templates.** Press **2nd** + **CLEAR** twice to access pre-built templates that you can modify with your own values.

7. **History recall.** Press **UP** in input mode to recall previous expressions. Press **DOWN** to go forward. Up to 16 recent expressions are saved. History is saved to an AppVar when you exit and restored on next launch.

8. **Cancel long computations.** If a calculation is taking too long (integration, large sums/products, series convergence tests), press **CLEAR** to cancel. CalcEngine checks for the CLEAR key periodically during long loops and will return you to the input screen.

9. **Progress indicator.** When a computation takes more than a moment, "Computing..." appears on screen to let you know the calculator is working. This clears automatically when the result is ready.

### CLEAR Key Behavior

The **CLEAR** key follows TI-84 conventions — it clears content before exiting:

- **From result/error screen:** Returns to input mode (expression preserved for editing).
- **With text in input:** Clears the input buffer.
- **Empty input:** No-op (does nothing).
- **In menus/settings/help:** Exits the menu.
- **In graph equation entry:** First press clears the equation, second press exits graph entry.
- **During computation:** Cancels the current calculation.

### Memory Limitations

CalcEngine uses a fixed pool of 512 AST nodes. Very complex expressions (deeply nested functions, large polynomials, long Taylor series) can exhaust this pool. If you see unexpected errors on complex inputs:

- Break the computation into smaller steps.
- Use `ans` to chain intermediate results.
- Reduce Taylor series degree or matrix size.
- The node counter in the status bar (`N:x/512`) shows current usage.

### Precision Limitations

CalcEngine uses 32-bit floating point arithmetic, which provides approximately 7 significant decimal digits. This means:

- Very large or very small numbers may lose precision.
- Comparisons of nearly equal numbers may be unreliable.
- Results are displayed with configurable precision (default 6 digits), but internal precision is fixed at ~7 digits.
- Tolerance-based comparisons (1e-6) are used internally to handle floating-point rounding.

---

*CalcEngine v1.0.0 -- Zendren LLC*
*For the TI-84 Plus CE*
