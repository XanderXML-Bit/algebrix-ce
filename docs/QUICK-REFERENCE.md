# CalcEngine Quick Reference

Complete input syntax reference for CalcEngine on the TI-84 Plus CE.

All commands are case-insensitive. Implicit multiplication is supported (e.g., `2x` = `2*x`).
Natural calculus shorthand is supported (`d/dx expr`, `int(expr)dx`).

---

## Macros & Shortcuts

| Input | Description |
|-------|-------------|
| `macro(name, body)` | Define a zero-argument shortcut |
| `macro(name(x), body)` | Define a 1-3 argument macro |
| `unmacro(name)` | Remove a macro |
| `macros()` | List defined macros |

Notes:
- Macro names and parameter names use letters/digits/underscore (`_`).
- Macro expansion is bounded; recursive macros are rejected.

---

## Arithmetic & Constants

| Input | Description |
|-------|-------------|
| `+` `-` `*` `/` `^` | Basic operators |
| `!` | Factorial |
| `%` | Percentage |
| `pi` | 3.14159... |
| `e` | Euler's number 2.71828... |
| `i` | Imaginary unit |
| `inf` | Infinity |
| `ans` | Previous answer |

---

## Functions

### Trigonometric
| Input | Description |
|-------|-------------|
| `sin(x)` `cos(x)` `tan(x)` | Standard trig |
| `sec(x)` `csc(x)` `cot(x)` | Reciprocal trig |
| `asin(x)` `acos(x)` `atan(x)` | Inverse trig (also `arcsin`, `arccos`, `arctan`) |
| `asec(x)` `acsc(x)` `acot(x)` | Inverse reciprocal trig |

### Hyperbolic
| Input | Description |
|-------|-------------|
| `sinh(x)` `cosh(x)` `tanh(x)` | Hyperbolic |
| `asinh(x)` `acosh(x)` `atanh(x)` | Inverse hyperbolic |
| `sech(x)` `csch(x)` `coth(x)` | Reciprocal hyperbolic |

### Logarithmic & Exponential
| Input | Description |
|-------|-------------|
| `ln(x)` | Natural logarithm |
| `log(x)` | Base-10 logarithm |
| `exp(x)` | e^x |
| `sqrt(x)` | Square root |
| `abs(x)` | Absolute value |

### Rounding & Parts
| Input | Description |
|-------|-------------|
| `floor(x)` | Floor (round down) |
| `ceil(x)` | Ceiling (round up) |
| `round(x)` | Round to nearest integer |
| `ipart(x)` | Integer part |
| `fpart(x)` | Fractional part |

### Other
| Input | Description |
|-------|-------------|
| `sign(x)` or `sgn(x)` | Sign function (-1, 0, 1) |
| `heaviside(x)` | Step function (0, 0.5, 1) |
| `gcd(a,b)` | Greatest common divisor |
| `lcm(a,b)` | Least common multiple |
| `mod(a,b)` | Modulo |
| `rand` | Random number [0,1] |
| `randint(a,b)` | Random integer in [a,b] |
| `ncr(n,r)` | Combinations (n choose r) |
| `npr(n,r)` | Permutations |
| `tofrac(x)` | Convert decimal to fraction |

---

## Calculus

### Derivatives
| Input | Description |
|-------|-------------|
| `d/dx(expr)` | Derivative with respect to x |
| `d/dx expr` | Derivative shorthand without required parentheses |
| `d/dx(expr)\|x=a` | Derivative evaluated at x=a |
| `nderiv(expr,var,n)` | nth derivative |
| `pdiff(expr,x)` | Partial derivative |
| `pdiff(expr,x,y)` | Mixed partial derivative |
| `gradient(expr,x,y)` | Gradient magnitude |
| `tangent(expr,var,pt)` | Tangent line at point |
| `relrate(expr)` | Related rates (d/dt) |

### Integration
| Input | Description |
|-------|-------------|
| `int(expr,x)` | Indefinite integral |
| `int(expr)dx` | Indefinite integral shorthand |
| `int(expr,x,a,b)` | Definite integral |
| `dblint(expr,x,a,b,y,c,d)` | Double integral |
| `arclen(f,x,a,b)` | Arc length |
| `surfarea(f,x,a,b)` | Surface area of revolution |
| `disk(f,x,a,b)` | Volume — disk method |
| `washer(f,g,x,a,b)` | Volume — washer method |
| `shell(f,x,a,b)` | Volume — shell method |
| `ivp(expr,x,x0,y0)` | Antiderivative with initial condition |
| `tabular(expr,x)` | Tabular integration by parts |

### Limits
| Input | Description |
|-------|-------------|
| `lim(expr,x,a)` | Two-sided limit |
| `lim(expr,x,a,1)` | Right-sided limit |
| `lim(expr,x,a,-1)` | Left-sided limit |
| `lim(expr,x,inf)` | Limit at infinity |
| `continuity(expr,x,a)` | Continuity check at point |

### Series & Sequences
| Input | Description |
|-------|-------------|
| `sum(expr,n,a,b)` | Finite/infinite summation |
| `prod(expr,n,a,b)` | Finite product |
| `taylor(expr,x,a,n)` | Taylor series (degree n, center a) |
| `fourier(expr,x,n)` | Fourier series (n harmonics) |
| `seqlim(expr,n)` | Sequence limit (n -> inf) |
| `recseq(expr,n,init,k)` | Recursive sequence |
| `arithsum(a1,d,n)` | Arithmetic series sum |
| `geomsum(a1,r,n)` | Geometric series sum |

---

## Algebra

### Solving
| Input | Description |
|-------|-------------|
| `solve(expr,x)` | Solve equation for x |
| `2x+3=7` | Auto-solve (detects `=` sign) |
| `nlsolve(eq1,eq2,x,y)` | Nonlinear system solver |

### Manipulation
| Input | Description |
|-------|-------------|
| `expand(expr)` | Expand expression |
| `factor(expr)` | Factor polynomial |
| `csquare(expr,x)` | Complete the square |
| `pfrac(expr,x)` | Partial fraction decomposition |
| `polydiv(p,q,x)` | Polynomial long division |
| `descartes(expr,x)` | Descartes' rule of signs |

### Critical Points & Root Finding
| Input | Description |
|-------|-------------|
| `critical(expr,x)` | Find critical points with classification |
| `newton(expr,x,x0)` | Newton's method |
| `bisect(expr,x,a,b)` | Bisection method |
| `secant(expr,x,x0,x1)` | Secant method |

### Coordinate Geometry
| Input | Description |
|-------|-------------|
| `line(x1,y1,x2,y2)` | Line equation from two points |
| `linept(m,x1,y1)` | Line from slope + point |
| `parabola(vx,vy,px,py)` | Parabola from vertex + point |
| `midpoint(x1,y1,x2,y2)` | Midpoint of two points |
| `distance(x1,y1,x2,y2)` | Distance between two points |
| `slope(x1,y1,x2,y2)` | Slope between two points |
| `unitcirc(angle)` | Unit circle lookup (degrees) |

---

## Linear Algebra (Matrices)

Matrix input uses semicolons for row separators (on the calculator, press **APPS** to type `;`):
```
mat(1,2;3,4)       → 2x2 matrix [[1,2],[3,4]]
mat(1,0,0;0,1,0;0,0,1)  → 3x3 identity
```

| Input | Description |
|-------|-------------|
| `mat(a,b;c,d)` | Create matrix |
| `det(M)` | Determinant |
| `inv(M)` | Inverse |
| `rref(M)` | Row reduced echelon form |
| `transpose(M)` | Transpose |

Eigenvalues, eigenvectors, LU decomposition, rank, nullity, cross/dot products are also supported via matrix operations.

---

## Statistics

### Descriptive
| Input | Description |
|-------|-------------|
| `mean(a,b,c,...)` | Mean |
| `median(a,b,c,...)` | Median |
| `mode(a,b,c,...)` | Mode |
| `stdev(a,b,c,...)` | Standard deviation |
| `variance(a,b,c,...)` | Variance (also `var`) |

### Regression
| Input | Description |
|-------|-------------|
| `linreg(x1,y1,x2,y2,...)` | Linear regression (y=ax+b) |
| `quadreg(x1,y1,...)` | Quadratic regression |
| `expreg(x1,y1,...)` | Exponential regression |
| `pwrreg(x1,y1,...)` | Power regression |
| `lnreg(x1,y1,...)` | Logarithmic regression |
| `covar(x1,y1,...)` | Covariance |
| `2varstats(x1,y1,...)` | Two-variable statistics |

### Hypothesis Tests
| Input | Description |
|-------|-------------|
| `ztest(x_bar,mu0,sigma,n,alt)` | One-sample Z-test |
| `ttest(x_bar,mu0,s,n,alt)` | One-sample T-test |
| `2sampztest(x1,x2,s1,s2,n1,n2,alt)` | Two-sample Z-test |
| `2sampttest(x1,x2,s1,s2,n1,n2,alt)` | Two-sample T-test |
| `1propztest(x,n,p0,alt)` | 1-proportion Z-test |
| `2propztest(x1,n1,x2,n2,alt)` | 2-proportion Z-test |
| `chi2test(o1,e1,o2,e2,...)` | Chi-squared GOF test |

`alt`: 0 = two-sided, 1 = greater, -1 = less

### Confidence Intervals
| Input | Description |
|-------|-------------|
| `zinterval(x_bar,sigma,n,cl)` | Z-interval |
| `tinterval(x_bar,s,n,cl)` | T-interval |
| `1propzint(x,n,cl)` | 1-proportion Z-interval |
| `2propzint(x1,n1,x2,n2,cl)` | 2-proportion Z-interval |
| `2sampzint(x1,x2,s1,s2,n1,n2,cl)` | Two-sample Z-interval |
| `2samptint(x1,x2,s1,s2,n1,n2,cl)` | Two-sample T-interval |

---

## Probability Distributions

| Input | Description |
|-------|-------------|
| `normalpdf(x,mu,sigma)` | Normal PDF |
| `normalcdf(lo,hi,mu,sigma)` | Normal CDF |
| `invnorm(area,mu,sigma)` | Inverse normal |
| `binompdf(n,p,k)` | Binomial PDF |
| `binomcdf(n,p,k)` | Binomial CDF |
| `poissonpdf(lambda,k)` | Poisson PDF |
| `poissoncdf(lambda,k)` | Poisson CDF |
| `geometpdf(p,k)` | Geometric PDF |
| `geometcdf(p,k)` | Geometric CDF |
| `tcdf(lo,hi,df)` | Student's t CDF |
| `invt(area,df)` | Inverse t |
| `chi2cdf(lo,hi,df)` | Chi-squared CDF |
| `invchi2(area,df)` | Inverse chi-squared |
| `fpdf(x,d1,d2)` | F-distribution PDF |
| `fcdf(lo,hi,d1,d2)` | F-distribution CDF |
| `invf(area,d1,d2)` | Inverse F |

---

## Finance (TVM)

| Input | Description |
|-------|-------------|
| `pv(rate,nper,pmt,fv)` | Present value |
| `fv(rate,nper,pmt,pv)` | Future value |
| `pmt(rate,nper,pv,fv)` | Payment |
| `nper(rate,pmt,pv,fv)` | Number of periods |
| `rate(nper,pmt,pv,fv)` | Interest rate |
| `nom(eff_rate,n)` | Effective -> nominal rate |
| `eff(nom_rate,n)` | Nominal -> effective rate |
| `bal(rate,nper,pmt,pv,k)` | Balance after k payments |
| `sumprn(rate,nper,pmt,pv,a,b)` | Sum of principal (payments a to b) |
| `sumint(rate,nper,pmt,pv,a,b)` | Sum of interest (payments a to b) |

---

## Lists

| Input | Description |
|-------|-------------|
| `{1,2,3}` | List literal |
| `L1` through `L6` | Named lists |
| `{1,2,3} -> L1` | Store to list |
| `dim(L1)` | List length |
| `cumsum(L1)` | Cumulative sum |
| `deltalist(L1)` | Differences |
| `sorta(L1)` | Sort ascending |
| `sortd(L1)` | Sort descending |
| `seq(expr,x,a,b,step)` | Generate sequence |

Lists support element-wise arithmetic: `{1,2,3} + {4,5,6}`, `2 * L1`, etc.

---

## Complex Numbers

| Input | Description |
|-------|-------------|
| `3+4i` | Complex literal |
| `real(z)` | Real part |
| `imag(z)` | Imaginary part |
| `conj(z)` | Conjugate |
| `abs(z)` | Magnitude |
| `angle(z)` | Argument (angle) |

All trig, hyperbolic, log, and exponential functions work with complex arguments when Complex mode is enabled (Settings > Complex: Rect or Polar).

---

## Polar & Parametric

| Input | Description |
|-------|-------------|
| `polar(x,y)` | Cartesian to polar |
| `rect(r,theta)` | Polar to Cartesian |
| `arclen(xt,yt,t,a,b)` | Parametric arc length |

---

## Piecewise Functions

```
piecewise(expr1, cond1, expr2, cond2, ..., default)
```

Example: `piecewise(x^2, x>0, -x, x<0, 0)`

---

## Number Base Conversions

| Input | Description |
|-------|-------------|
| `dec(x)` | To decimal |
| `hex(x)` | To hexadecimal |
| `bin(x)` | To binary |
| `oct(x)` | To octal |

---

## Variable Assignment

```
expr -> X       Store a number in X (A-Z except E and complex-mode I).
                Kept across entries and relaunches. Arithmetic entries use
                values. Root d/dx, int, solve commands keep letters symbolic.
                MODE > Stored vars > RIGHT, ENTER clears.
expr -> L1      Store to list L1-L6
```

---

## Keyboard Shortcuts

| Key | Normal | 2nd |
|-----|--------|-----|
| ENTER | Evaluate / next step | — |
| CLEAR | Clear input | Function menu (2nd+CLEAR) |
| MODE | Cycle answer format (dec/frac/sci) | — |
| Y= | Graph: equation entry | — |
| WINDOW | Graph: viewport editor | — |
| ZOOM | Graph: zoom presets | — |
| TRACE | Graph: trace mode. CE launcher home screen: opens the full-result viewer (same as UP), which lists the steps when Show work is ON | — |
| GRAPH | Graph: draw/refresh | Help pages (2nd+GRAPH) |
| ln | ln( | e^( |
| log | log( | 10^( |
| x^-1 | ^(-1) | abs( |
| MATH | nderiv( | solve( |
| Up/Down | Scroll steps / History | — |
| Left/Right | Move cursor | — |
| ALPHA | Toggle letter input | — |

Status bar indicators: `[2ND]` = 2nd mode active, `[A]` = ALPHA mode active

---

## Settings (MODE key in settings view)

1. **Angle mode**: Radians / Degrees
2. **Show steps**: On / Off
3. **Precision**: Decimal places (1-9)
4. **Complex mode**: Off / Rectangular / Polar
5. **Step detail**: All steps / Compact
6. **Show method choices**: Off / On — when On, ambiguous expressions (e.g. `sin(x)`) show an Evaluate vs Simplify dialog
7. **Segment pause**: Off / On — pause between step segments for easier reading

---

## MathPrint Templates

Typing certain prefixes automatically activates a visual fill-in template with highlighted slots:

| Trigger | Template | Slots |
|---------|----------|-------|
| `int(` | Indefinite integral: `int(□,□)` | expr, var |
| `def(` | Definite integral: `int(□,□,□,□)` | expr, var, lo, hi |
| `d/dx(` | Derivative: `d/dx(□)` | expr |
| `lim(` | Limit: `lim(□,□,□)` | expr, var, point |
| `sum(` | Sum: `sum(□,□,□,□)` | expr, var, lo, hi |
| `product(` | Product: `product(□,□,□,□)` | expr, var, lo, hi |
| `x/` | Fraction: `(□)/(□)` | numerator, denominator |
| `taylor(` | Taylor series: `taylor(□,□,□,□)` | expr, var, center, degree |
| `tangent(` | Tangent line: `tangent(□,□,□)` | expr, var, point |
| `nderiv(` | Nth derivative: `nderiv(□,□,□)` | expr, var, n |

- **TAB** / right arrow: advance to next slot
- **DEL**: cancel template, return to plain input
- Template box is drawn around the active slot with a highlight border

---

## Smart Disambiguation

This dialog exists in the desktop build only. On the TI-84 Plus CE, an expression in a variable with no operation around it shows `X has no value`; press UP for the hints (`d/dX(...)`, `int(...,X)`, `solve(...=0,X)`, `factor(...)`, `expand(...)`), or type the operation directly.

When you enter an expression that could be interpreted multiple ways, the desktop build shows a disambiguation dialog:

| Expression type | Options shown |
|-----------------|---------------|
| Polynomial `x^2+5x+6` | Evaluate / Factor / Find Roots / Simplify |
| Linear `2x+5` | Evaluate / Solve (=0) |
| Rational `(x^2-1)/(x-1)` | Evaluate / Partial Fractions / Simplify |
| Expandable `(x+1)^3` | Evaluate / Expand / Simplify |

Explicit operations (`factor(...)`, `d/dx(...)`, `solve(...)`) are **never** ambiguous — they go straight to evaluation.

Navigate the dialog with Up/Down, confirm with ENTER.

---

## Function Menu (CLEAR key)

12 pages of shortcuts, accessible by pressing CLEAR:

| Page | Category | Count |
|------|----------|-------|
| (Recent) | Last 5 entries | up to 5 |
| 1 | Calculus | 14 items |
| 2 | Int Methods | 12 items |
| 3 | ODEs & Graphs | 10 items |
| 4 | Algebra | 17 items |
| 5 | Statistics | 24 items |
| 6 | Linear Algebra | 5 items |
| 7 | Probability | 15 items |
| 8 | Finance | 10 items |
| 9 | Functions | 28 items |
| 10 | Sequences | 9 items |
| 11 | Symbols | 30 items |

The Symbols page includes: `∞`, `π`, `e`, `i`, `√`, `≠`, `≤`, `≥`, `α`, `β`, `γ`, `δ`, `θ`, `λ`, `μ`, `σ`, `Σ`, `Δ`, `∂`, `∫`, `→`, `±`, `°`, `‰`, and more.
