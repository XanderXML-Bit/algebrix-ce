# Audit result-string consumers

## Keep internal formatting stable

This change adds display-only printing. `ast_to_str()` retains the canonical format used by steps, checkpoints, and internal text.

The source audit records 124 calls outside `src/ast.c`. Of these, 98 remain canonical, 23 use `ast_to_display_str()`, and three use `ast_to_display_term()`.

Display calls cover these result contexts:

| File | Result context |
| --- | --- |
| `src/split/processor.c` | Derivatives, antiderivatives, series, expansion, factors, related rates, polynomial quotients, and the `Parsed:` echo |
| `src/modules/calc_int/strategy_main.c` | Integration-strategy antiderivatives |
| `src/integrate.c` | Separable and linear differential equations, plus the definite-integral antiderivative |
| `src/algebra.c` | Polynomial remainders |
| `src/vector.c` | Gradient, divergence, and curl components |
| `src/dsolve.c` | Exact-equation components and the displayed forcing expression |
| `src/graph.c` | Implicit-graph labels |

Steps that share a result buffer also use display text. Other steps keep canonical text. The desktop monolith in `src/main.c` remains unchanged.

CECISM, CEIPFR, and CECLMB retain `CE_AST_DISPLAY_PRINT=0` under the fixed integration policy. The original size-budget decision remains unchanged after printer compaction. Basic and partial-fraction antiderivatives retain canonical parentheses. CECLMB retains canonical auxiliary AST text. Computation and step recording remain enabled.

## Check parser inputs

The audited parser inputs come from user input, macro-expanded user input, or inter-module requests:

- `src/core/launcher/main.c`: tokenizes the input buffer before routing.
- `src/split/processor.c`: tokenizes the request expression in desktop or non-preparsed paths.
- `src/split/core_main.c`: tokenizes its request expression.
- `src/mathprint.c`: tokenizes macro-expanded editor input for drawing.
- `src/main.c`: parses editor input, method-selection input, and entered variable values.

None consumes a result string from either printer. The launcher's Ans key inserts the `ans` token. History recall copies the submitted expression, not its answer.

## Check string comparisons and composition

The polynomial-division paths in `src/split/processor.c` and `src/main.c` compare the remainder against `"0"`. Both printers render a zero leaf identically.

Other audited string comparisons inspect keywords, menu entries, or input text. They do not compare printed AST results.

`ast_to_display_str()` supplies standalone expressions or explicitly wrapped components. Related rates wraps each component before multiplication. Linear differential equations wrap the numerator and denominator.

`ast_to_display_term()` protects components joined by `+` or `-`, including curl subtrahends and exact-equation terms. It does not promise safe composition after `/` or beside `^`.

## Preserve the parsed operator tree

The printer retains right-nested operations, including `X+(Y+Z)` and `X*(Y/Z)`. It also retains the product beneath unary minus, as `-(2X)`. It does not rewrite addition of a negative term into subtraction.

The requested derivative prints `cos(X)*X^2+sin(X)*(2X)`. A regression re-parses that string and checks `ast_equal()` against the derivative AST. The parsed-input table also checks structural equality.

Numeric leaves retain historical six-significant-digit formatting. A negative NUMBER leaf re-parses as unary minus on a positive NUMBER. The randomized tests check numerical agreement for those generated shapes.

An implicit product follows a finite numeric literal only. The printer retains `*` before `e`, `e^`, and `exp(` to avoid scientific-notation tokenization. Letter-to-letter products also retain `*`.

Both display entry points mark truncation with `...` when the buffer has at least four bytes. Smaller buffers remain bounded but cannot hold the complete marker.

## Reproduce the audit

Search `src/` for `ast_to_str`, `ast_to_display_str`, `ast_to_display_term`, `tok_init`, and `parser_parse`. Inspect comparisons and every composition site above.

The task evidence includes `string-call-audit.tsv`, which records each call and its surrounding source. Package manifests and frame checksums remain with the separate release-refresh task. This work provides emulator evidence, not hardware verification.
