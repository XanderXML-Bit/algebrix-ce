/*
 * matrix.c — Matrix operations
 *
 * Fixed-size matrices (max 6x6) with row-major storage.
 * Determinant via Gaussian elimination with partial pivoting (O(n^3)).
 * Direct formulas for n<=3, elimination for n>=4.
 * Inverse and RREF via Gauss-Jordan elimination.
 */
#include "matrix.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

/* Epsilon for pivot detection and near-zero cleanup.
 * 1e-10 is below 32-bit float precision (~7 digits); 1e-7 is correct. */
#define MAT_EPS 1e-7f

/* ── Initialisation ─────────────────────────────────────── */

void matrix_init(ce_matrix_t *m, uint8_t rows, uint8_t cols)
{
    memset(m, 0, sizeof(*m));
    if (rows > MAX_MATRIX_DIM) rows = MAX_MATRIX_DIM;
    if (cols > MAX_MATRIX_DIM) cols = MAX_MATRIX_DIM;
    m->rows = rows;
    m->cols = cols;
}

/* ── Arithmetic ─────────────────────────────────────────── */

bool matrix_add(const ce_matrix_t *a, const ce_matrix_t *b, ce_matrix_t *out)
{
    ce_matrix_t tmp;
    int i;
    if (a->rows != b->rows || a->cols != b->cols) return false;
    matrix_init(&tmp, a->rows, a->cols);
    for (i = 0; i < a->rows * a->cols; i++)
        tmp.data[i] = a->data[i] + b->data[i];
    *out = tmp;
    return true;
}

bool matrix_sub(const ce_matrix_t *a, const ce_matrix_t *b, ce_matrix_t *out)
{
    ce_matrix_t tmp;
    int i;
    if (a->rows != b->rows || a->cols != b->cols) return false;
    matrix_init(&tmp, a->rows, a->cols);
    for (i = 0; i < a->rows * a->cols; i++)
        tmp.data[i] = a->data[i] - b->data[i];
    *out = tmp;
    return true;
}

bool matrix_mul(const ce_matrix_t *a, const ce_matrix_t *b, ce_matrix_t *out)
{
    ce_matrix_t tmp;
    int i, j, k;
    if (a->cols != b->rows) return false;
    matrix_init(&tmp, a->rows, b->cols);
    for (i = 0; i < a->rows; i++) {
        for (j = 0; j < b->cols; j++) {
            ce_float_t sum = 0;
            for (k = 0; k < a->cols; k++)
                sum += MAT_AT(a, i, k) * MAT_AT(b, k, j);
            MAT_AT(&tmp, i, j) = sum;
        }
    }
    *out = tmp;
    return true;
}

void matrix_scale(const ce_matrix_t *a, ce_float_t s, ce_matrix_t *out)
{
    ce_matrix_t tmp;
    int i;
    matrix_init(&tmp, a->rows, a->cols);
    for (i = 0; i < a->rows * a->cols; i++)
        tmp.data[i] = a->data[i] * s;
    *out = tmp;
}

void matrix_transpose(const ce_matrix_t *a, ce_matrix_t *out)
{
    ce_matrix_t tmp;
    int i, j;
    matrix_init(&tmp, a->cols, a->rows);
    for (i = 0; i < a->rows; i++)
        for (j = 0; j < a->cols; j++)
            MAT_AT(&tmp, j, i) = MAT_AT(a, i, j);
    *out = tmp;
}

/* ── Trace ──────────────────────────────────────────────── */

ce_float_t matrix_trace(const ce_matrix_t *m)
{
    int i, n;
    ce_float_t sum = 0;
    if (!m || m->rows != m->cols) return NAN;
    n = m->rows;
    for (i = 0; i < n; i++)
        sum += MAT_AT(m, i, i);
    return sum;
}

/* ── Determinant ────────────────────────────────────────── */

/*
 * Determinant via Gaussian elimination with partial pivoting.
 * O(n^3) time, O(1) stack depth (just one ce_matrix_t copy).
 * For n<=3, uses direct formulas; for n>=4, uses elimination.
 * This avoids the O(n!) recursive cofactor expansion which
 * creates dangerously deep stack frames for 6x6 on eZ80.
 */
ce_float_t matrix_det(const ce_matrix_t *m)
{
    ce_matrix_t tmp;
    int n, i, j, k, pivot_row;
    ce_float_t det, max_val, pivot;

    if (m->rows != m->cols) return NAN;  /* non-square: error */
    n = m->rows;

    if (n == 0) return 1.0f;  /* empty matrix: det = 1 by convention */
    if (n == 1) return m->data[0];
    if (n == 2)
        return m->data[0] * m->data[3] - m->data[1] * m->data[2];
    if (n == 3) {
        /* Sarrus' rule — avoids overhead of elimination for 3x3 */
        return MAT_AT(m,0,0) * (MAT_AT(m,1,1)*MAT_AT(m,2,2) - MAT_AT(m,1,2)*MAT_AT(m,2,1))
             - MAT_AT(m,0,1) * (MAT_AT(m,1,0)*MAT_AT(m,2,2) - MAT_AT(m,1,2)*MAT_AT(m,2,0))
             + MAT_AT(m,0,2) * (MAT_AT(m,1,0)*MAT_AT(m,2,1) - MAT_AT(m,1,1)*MAT_AT(m,2,0));
    }

    /* n >= 4: Gaussian elimination with partial pivoting */
    tmp = *m;  /* work on a copy */
    det = 1.0f;

    for (i = 0; i < n; i++) {
        /* Partial pivoting: find row with largest absolute value in column i */
        max_val = fabsf(MAT_AT(&tmp, i, i));
        pivot_row = i;
        for (k = i + 1; k < n; k++) {
            ce_float_t v = fabsf(MAT_AT(&tmp, k, i));
            if (v > max_val) { max_val = v; pivot_row = k; }
        }

        if (max_val < MAT_EPS) return 0.0f;  /* singular */

        /* Swap rows if needed (flips determinant sign) */
        if (pivot_row != i) {
            for (j = 0; j < n; j++) {
                ce_float_t t = MAT_AT(&tmp, i, j);
                MAT_AT(&tmp, i, j) = MAT_AT(&tmp, pivot_row, j);
                MAT_AT(&tmp, pivot_row, j) = t;
            }
            det = -det;  /* row swap negates determinant */
        }

        /* Multiply determinant by pivot */
        pivot = MAT_AT(&tmp, i, i);
        det *= pivot;

        /* Eliminate below */
        for (k = i + 1; k < n; k++) {
            ce_float_t factor = MAT_AT(&tmp, k, i) / pivot;
            for (j = i + 1; j < n; j++)
                MAT_AT(&tmp, k, j) -= factor * MAT_AT(&tmp, i, j);
            MAT_AT(&tmp, k, i) = 0;  /* not strictly needed, but clean */
        }
    }

    return det;
}

/* ── RREF (Gauss-Jordan elimination in place) ───────────── */

void matrix_rref(ce_matrix_t *m)
{
    int lead = 0;
    int r, i, j;

    for (r = 0; r < m->rows; r++) {
        ce_float_t div, mult;

        if (lead >= m->cols) return;

        /* Find pivot row */
        i = r;
        while (fabsf(MAT_AT(m, i, lead)) < MAT_EPS) {
            i++;
            if (i == m->rows) {
                i = r;
                lead++;
                if (lead == m->cols) return;
            }
        }

        /* Swap rows i and r */
        if (i != r) {
            for (j = 0; j < m->cols; j++) {
                ce_float_t tmp = MAT_AT(m, i, j);
                MAT_AT(m, i, j) = MAT_AT(m, r, j);
                MAT_AT(m, r, j) = tmp;
            }
        }

        /* Scale pivot row */
        div = MAT_AT(m, r, lead);
        if (fabsf(div) > MAT_EPS) {
            for (j = 0; j < m->cols; j++)
                MAT_AT(m, r, j) /= div;
        }

        /* Eliminate column */
        for (i = 0; i < m->rows; i++) {
            if (i == r) continue;
            mult = MAT_AT(m, i, lead);
            for (j = 0; j < m->cols; j++)
                MAT_AT(m, i, j) -= mult * MAT_AT(m, r, j);
        }
        lead++;
    }

    /* Clean near-zero values */
    for (i = 0; i < m->rows * m->cols; i++)
        if (fabsf(m->data[i]) < MAT_EPS) m->data[i] = 0;
}

/* ── Inverse (augmented matrix + Gauss-Jordan) ─────────── */

/* Uses a local 2D array for the augmented matrix [A|I] so we
 * don't hit the MAX_MATRIX_DIM column cap in ce_matrix_t.
 * Stack cost: 6*12*4 = 288 bytes — fine for TI-84 (4KB stack). */

bool matrix_inverse(const ce_matrix_t *m, ce_matrix_t *out)
{
    ce_float_t aug[MAX_MATRIX_DIM][2 * MAX_MATRIX_DIM];
    int i, j, n, lead, r;
    int ncols;

    if (m->rows != m->cols) return false;
    n = m->rows;
    if (n > MAX_MATRIX_DIM) return false;
    ncols = 2 * n;

    /* Check determinant */
    if (fabsf(matrix_det(m)) < MAT_EPS) return false;

    /* Build augmented matrix [A | I] */
    memset(aug, 0, sizeof(aug));
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++)
            aug[i][j] = MAT_AT(m, i, j);
        aug[i][n + i] = 1.0f;
    }

    /* Gauss-Jordan elimination on augmented matrix */
    lead = 0;
    for (r = 0; r < n; r++) {
        ce_float_t div, mult;

        if (lead >= ncols) break;

        /* Find pivot row */
        i = r;
        while (fabsf(aug[i][lead]) < MAT_EPS) {
            i++;
            if (i == n) {
                i = r;
                lead++;
                if (lead == ncols) goto done;
            }
        }

        /* Swap rows i and r */
        if (i != r) {
            for (j = 0; j < ncols; j++) {
                ce_float_t tmp = aug[i][j];
                aug[i][j] = aug[r][j];
                aug[r][j] = tmp;
            }
        }

        /* Scale pivot row */
        div = aug[r][lead];
        if (fabsf(div) > MAT_EPS) {
            for (j = 0; j < ncols; j++)
                aug[r][j] /= div;
        }

        /* Eliminate column */
        for (i = 0; i < n; i++) {
            if (i == r) continue;
            mult = aug[i][lead];
            for (j = 0; j < ncols; j++)
                aug[i][j] -= mult * aug[r][j];
        }
        lead++;
    }

done:
    /* Extract inverse from right half */
    matrix_init(out, (uint8_t)n, (uint8_t)n);
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            MAT_AT(out, i, j) = aug[i][n + j];

    return true;
}

/* ── Eigenvalues ────────────────────────────────────────── */

/*
 * Real eigenvalues of a square 1x1..3x3 matrix in 32-bit floats, listed
 * with multiplicity, largest first.
 *
 * - The matrix is first divided by amax = max|a_ij|, so no product below
 *   can overflow; the values are multiplied back at the end.
 * - Triangular input (exact zeros on one side of the diagonal, as in most
 *   textbook examples) answers its diagonal exactly.
 * - 2x2: lambda = s +- sqrt(h^2 + b*c) with s = (a+d)/2, h = (a-d)/2.
 * - 3x3: shift by s = trace/3 and scale by max|A - s*I|, which gives the
 *   depressed cubic f(t) = t^3 + p*t + q (p = sum of the principal 2x2
 *   minors, q = -det).  f' = 3t^2 + p vanishes at +-c, c = sqrt(-p/3), and
 *   the signs of f(-c) and f(c) say how many real roots there are and
 *   bracket each one for bisection.  A local extremum within the rounding
 *   noise of zero is a double root, placed at the critical point, which
 *   floats find accurately.  (Newton on the raw cubic split double roots
 *   into 3.0012 and 2.9997, and a deflated discriminant lost or invented
 *   real roots.)
 *
 * Float32 limits: two eigenvalues closer than about 0.3% of max|A - s*I|
 * (three within about 1%) can come out as one repeated value, and a complex
 * pair that close to the real axis as a repeated real value; their vectors
 * then fail the residual check.  At that distance the rounding of the
 * coefficients alone can merge them: a double root moves with the square
 * root of the rounding, a triple with the cube root.  Strongly non-normal
 * matrices (large entries, small eigenvalues) reach these limits sooner,
 * and can also split a true double root.
 */

/* Rounding noise allowed in a 2x2 discriminant, relative to h^2 + |b*c|. */
#define EIG_DISC2_TOL 1e-6f
/* Rounding noise allowed in f at a critical point, per unit of
 * sp*c + sq (see eig_cubic).  Measured on exact double roots in float32:
 * up to 1.3e-6 with round-toward-zero (the CE soft-float truncates), 8e-7
 * with round-to-nearest, over integer P*D*P^-1 matrices (entries to 200
 * and 5000), rotated symmetric ones, and the same scaled by 1e-20 and 1e25.
 * Larger merges more close pairs (2e-6 merged pairs 0.5% apart); smaller
 * splits true double roots of the larger-entry matrices on the CE. */
#define EIG_F_TOL 1.5e-6f
/* Inputs beyond this would overflow on the way back (|lambda| <= 3*amax). */
#define EIG_MAX_ENTRY 1e37f

/* r, or 0 when |r| is below the rounding error of a sum whose terms
 * total mag in magnitude (about 16 ulp).  A zero 3x3 eigenvalue then
 * prints as 0, not -0 or 9e-8. */
static ce_float_t eig_snap(ce_float_t r, ce_float_t mag)
{
    return fabsf(r) <= 2e-6f * mag ? 0 : r;
}

static ce_float_t eig_f(ce_float_t t, ce_float_t p, ce_float_t q)
{
    return (t * t + p) * t + q;
}

/* A root of f in [lo, hi], where f(lo) and f(hi) have opposite signs (or
 * one is zero). */
static ce_float_t eig_root(ce_float_t lo, ce_float_t hi, ce_float_t p, ce_float_t q)
{
    bool lo_neg = eig_f(lo, p, q) < 0;
    int it;
    /* q = 0: t = 0 is an exact root, and the bracket holds only one root
     * (bisection would stop at about 1e-22 instead). */
    if (q == 0 && lo <= 0 && hi >= 0) return 0;
    for (it = 0; it < 64; it++) {
        ce_float_t mid = (lo + hi) * 0.5f;
        if (mid == lo || mid == hi) break;
        if ((eig_f(mid, p, q) < 0) == lo_neg) lo = mid;
        else hi = mid;
    }
    return (lo + hi) * 0.5f;
}

/* Real roots of t^3 + p*t + q into t[], with multiplicity; returns 1 or 3.
 * sp and sq are the sums of the magnitudes of the products that made p and
 * q, so their rounding error is a few ulps of sp and sq, and the noise in
 * f(+-c) is about c*err(p) + err(q): tol = EIG_F_TOL * (sp*c + sq).  (A
 * tolerance relative to 1 + |p| + |q| took exact coefficients such as
 * p = 5e-5, q = 0 for a triple root.) */
static int eig_cubic(ce_float_t p, ce_float_t q, ce_float_t sp, ce_float_t sq,
                     ce_float_t t[3])
{
    ce_float_t hi, c, tol;
    hi = 1.0f + (fabsf(p) > fabsf(q) ? fabsf(p) : fabsf(q));
    c = p < 0 ? sqrtf(-p / 3.0f) : 0;
    tol = EIG_F_TOL * (sp * c + sq);
    ce_float_t fa = eig_f(-c, p, q);   /* local maximum (p < 0) */
    ce_float_t fb = eig_f(c, p, q);    /* local minimum (p < 0) */

    if (fabsf(fa) <= tol && fabsf(fb) <= tol && fabsf(p) <= EIG_F_TOL * sp) {
        t[0] = t[1] = t[2] = 0;        /* triple root */
        return 3;
    }
    if (c == 0) {                      /* f is increasing: one root */
        t[0] = eig_root(-hi, hi, p, q);
        return 1;
    }
    if (fb > tol) {                    /* only left of -c */
        t[0] = eig_root(-hi, -c, p, q);
        return 1;
    }
    if (fa < -tol) {                   /* only right of c */
        t[0] = eig_root(c, hi, p, q);
        return 1;
    }
    if (fabsf(fa) <= tol && fb < -tol) {   /* double root at -c */
        t[0] = t[1] = -c;
        t[2] = eig_root(c, hi, p, q);
        return 3;
    }
    if (fabsf(fb) <= tol && fa > tol) {    /* double root at c */
        t[0] = t[1] = c;
        t[2] = eig_root(-hi, -c, p, q);
        return 3;
    }
    if (fa > tol && fb < -tol) {           /* three distinct */
        t[0] = eig_root(-hi, -c, p, q);
        t[1] = eig_root(-c, c, p, q);
        t[2] = eig_root(c, hi, p, q);
        return 3;
    }
    /* Both extrema within the noise but p is not: a cluster narrower than
     * float32 can split.  Report the centre three times. */
    t[0] = t[1] = t[2] = 0;
    return 3;
}

int matrix_eigenvalues(const ce_matrix_t *m, ce_float_t *eigenvals, int max_vals)
{
    ce_float_t d[9];
    ce_float_t amax = 0;
    bool exact = false;     /* triangular: the diagonal as entered */
    int n, i, j, found;

    if (!m || !eigenvals || max_vals < 1) return 0;
    if (m->rows != m->cols) return 0;
    n = m->rows;
    if (n < 1) return 0;
    if (n > 3) return -1;  /* >3x3 not supported */
    if (max_vals < n) return 0;

    for (i = 0; i < n * n; i++) {
        ce_float_t v = m->data[i];
        if (v != v) return -2;                     /* NaN */
        if (fabsf(v) > amax) amax = fabsf(v);
    }
    if (amax > EIG_MAX_ENTRY) return -2;
    if (n == 1) {
        eigenvals[0] = m->data[0];
        return 1;
    }
    if (amax == 0) {                               /* the zero matrix */
        for (i = 0; i < n; i++) eigenvals[i] = 0;
        return n;
    }
    for (i = 0; i < n * n; i++) d[i] = m->data[i] / amax;

    if (n == 2) {
        /* d = [a b; c e] */
        if (d[1] == 0 || d[2] == 0) {
            /* Triangular: the diagonal, unscaled so it is exact. */
            eigenvals[0] = m->data[0];
            eigenvals[1] = m->data[3];
            found = 2;
            exact = true;
        } else {
            ce_float_t s = (d[0] + d[3]) * 0.5f;
            ce_float_t h = (d[0] - d[3]) * 0.5f;
            ce_float_t bc = d[1] * d[2];
            ce_float_t disc = h * h + bc;
            ce_float_t tol = EIG_DISC2_TOL * (h * h + fabsf(bc));
            if (disc < -tol) return 0;             /* complex pair */
            if (disc <= tol) {
                eigenvals[0] = eigenvals[1] = s;
            } else {
                /* The root of larger magnitude has no cancellation; the
                 * other is det / lambda1 (s - sqrt(disc) lost most digits
                 * of a small eigenvalue next to a large one). */
                ce_float_t sq = sqrtf(disc);
                ce_float_t l1 = s < 0 ? s - sq : s + sq;
                eigenvals[0] = l1;
                ce_float_t ad = d[0] * d[3], bc2 = d[1] * d[2];
                ce_float_t det = ad - bc2;
                /* Below about 4 ulp of |ad| + |bc| the determinant is 0
                 * (mat2(1,2,3,6) -> {7, 0}).  The noise is the parse of each
                 * entry, the division by amax and the two products (about
                 * 2.5 ulp with a parse within 1 ulp); a cancelling
                 * subtraction is exact.  16 ulp zeroed a resolvable -1e-4
                 * (mat2(1,1,-0.990099,-0.9901)).  Products that underflow
                 * (entries near 1e-20 and 1e20 together) are not covered. */
                if (fabsf(det) <= 5e-7f * (fabsf(ad) + fabsf(bc2))) det = 0;
                eigenvals[1] = det / l1;
            }
            found = 2;
        }
    } else if ((d[3] == 0 && d[6] == 0 && d[7] == 0) ||
               (d[1] == 0 && d[2] == 0 && d[5] == 0)) {
        eigenvals[0] = m->data[0];
        eigenvals[1] = m->data[4];
        eigenvals[2] = m->data[8];
        found = 3;
        exact = true;
    } else {
        ce_float_t s = (d[0] + d[4] + d[8]) / 3.0f;
        ce_float_t sa = (fabsf(d[0]) + fabsf(d[4]) + fabsf(d[8])) / 3.0f;
        ce_float_t scale = 0, p, q, sp, sq, t[3];
        for (i = 0; i < 9; i += 4) d[i] -= s;      /* d = A - s*I */
        for (i = 0; i < 9; i++) {
            if (fabsf(d[i]) > scale) scale = fabsf(d[i]);
        }
        if (scale < 1e-6f) {
            /* A = s*I to float precision */
            eigenvals[0] = eigenvals[1] = eigenvals[2] = s;
            found = 3;
        } else {
            for (i = 0; i < 9; i++) d[i] /= scale;
            /* p = sum of the principal 2x2 minors, q = -det by cofactors
             * of row 0; sp, sq = sums of the magnitudes of their products.
             * Table-driven: each inline float product costs eZ80 code. */
            p = sp = q = sq = 0;
            for (i = 0; i < 3; i++) {
                /* pm[i]: a principal 2x2 minor (summed in this order);
                 * qm[i]: the minor of rows 1-2 omitting column i, times
                 * d[i], gives q. */
                static const unsigned char pm[3][4] = {
                    { 0, 4, 1, 3 }, { 0, 8, 2, 6 }, { 4, 8, 5, 7 } };
                static const unsigned char qm[3][4] = {
                    { 4, 8, 5, 7 }, { 3, 8, 5, 6 }, { 3, 7, 4, 6 } };
                ce_float_t x = d[pm[i][0]] * d[pm[i][1]];
                ce_float_t y = d[pm[i][2]] * d[pm[i][3]];
                ce_float_t u = d[qm[i][0]] * d[qm[i][1]];
                ce_float_t w = d[qm[i][2]] * d[qm[i][3]];
                ce_float_t term = d[i] * (u - w);
                p += x - y;
                sp += fabsf(x) + fabsf(y);
                q += (i == 1) ? term : -term;      /* -det = -(+ - +) */
                sq += fabsf(d[i]) * (fabsf(u) + fabsf(w));
            }
            /* q within its own rounding of 0 (a singular matrix): t = 0 is
             * a root, exactly, instead of bisection noise near -q/p.  Only
             * when that root (about scale*q/p) is within a few ulp of the
             * matrix (sa + scale; sa alone is 0 for a zero diagonal): with
             * small |p| (large entries, small eigenvalues) the shift would
             * move the other roots too. */
            if (fabsf(q) <= EIG_F_TOL * sq &&
                fabsf(q) * scale <= 2e-6f * (sa + scale) * fabsf(p)) q = 0;
            found = eig_cubic(p, q, sp, sq, t);
            /* s carries the rounding of the trace (terms of size sa), so a
             * result below that is 0 (a singular matrix with trace 0). */
            for (i = 0; i < found; i++) {
                ce_float_t x = scale * t[i];
                eigenvals[i] = eig_snap(s + x, sa + fabsf(x));
            }
        }
    }

    for (i = 0; i < found && !exact; i++) eigenvals[i] *= amax;

    /* Sort descending.  The inner loop only reads (see the codegen note in
     * matrix_eigenvectors); the swap happens after it. */
    for (i = 0; i < found - 1; i++) {
        int best = i;
        ce_float_t tmp;
        for (j = i + 1; j < found; j++) {
            if (eigenvals[j] > eigenvals[best]) best = j;
        }
        tmp = eigenvals[i];
        eigenvals[i] = eigenvals[best];
        eigenvals[best] = tmp;
    }
    return found;
}

/* ── Eigenvectors ───────────────────────────────────────── */

/*
 * For each eigenvalue, a null vector of B = A - lambda*I by Gauss-Jordan
 * elimination with full pivoting.  lambda is a float approximation, so B is
 * only nearly singular: elimination stops after n-1 pivots (B is singular
 * by construction) or when the largest remaining entry is at most tol.  The
 * remaining (free) columns give the candidate vectors (below).
 *
 * The vector is scaled so its largest component has magnitude 1 and is
 * then checked: if max|A v - lambda v| exceeds EIG_RES_TOL times
 * max|A - s*I| (all relative to max|A|), it is not trusted and is returned
 * as all zeros.
 */
#define EIG_REL_TOL 1e-4f
#define EIG_RES_TOL 1e-3f

/* Candidate null vector for free column fc: v[fc] = 1, the other free
 * variables 0, each pivot variable from its reduced row; scaled so the
 * largest component has magnitude 1. */
static void eig_candidate(int n, ce_float_t *const row[],
                          const signed char pivcol[], int rank, int fc,
                          ce_float_t *v)
{
    ce_float_t maxv = 0;
    int j, r;
    for (j = 0; j < n; j++) v[j] = 0;
    v[fc] = 1.0f;
    for (r = 0; r < rank; r++) v[(int)pivcol[r]] = -row[r][fc];
    for (j = 0; j < n; j++) {
        if (fabsf(v[j]) > maxv) maxv = fabsf(v[j]);
    }
    for (j = 0; j < n; j++) {
        /* Components below 1e-6 of the largest are under the pivot
         * tolerance (EIG_REL_TOL) and print as 0, not "-0" or "3e-9". */
        v[j] /= maxv;
        if (fabsf(v[j]) < 1e-6f) v[j] = 0;
    }
}

/* max_i |(A v - lambda v)_i| */
static ce_float_t eig_residual(const ce_float_t *a, int n, ce_float_t lambda,
                               const ce_float_t *v)
{
    ce_float_t res = 0;
    int i, j;
    for (i = 0; i < n; i++) {
        ce_float_t s = -lambda * v[i];
        for (j = 0; j < n; j++) s += a[i * n + j] * v[j];
        if (fabsf(s) > res) res = fabsf(s);
    }
    return res;
}

int matrix_eigenvectors(const ce_matrix_t *m, const ce_float_t eigenvals[],
                        ce_float_t eigenvecs[][MAX_MATRIX_DIM], int n_eigenvals)
{
    static ce_float_t work[MAX_MATRIX_DIM * MAX_MATRIX_DIM];
    static ce_float_t a[MAX_MATRIX_DIM * MAX_MATRIX_DIM];   /* A / big */
    ce_float_t *row[MAX_MATRIX_DIM];
    signed char pivcol[MAX_MATRIX_DIM];   /* pivot column of row r          */
    bool used[MAX_MATRIX_DIM];            /* column holds a pivot           */
    ce_float_t cn = 0, big = 0, sh = 0, tol, res_tol;
    int k, n, i, j, rank;

    if (!m || !eigenvals || !eigenvecs || n_eigenvals < 1) return 0;
    if (m->rows != m->cols) return 0;
    n = m->rows;
    if (n < 1) return 0;
    if (n > MAX_MATRIX_DIM) return 0;
    if (n_eigenvals > n) n_eigenvals = n;

    /* Work on A / max|a_ij| (and lambda / max|a_ij|), so the tolerances are
     * relative at any scale: with absolute floors, mat2(1e-8,2e-8,2e-8,1e-8)
     * skipped elimination and accepted [0,1] and [1,0].  The zero matrix
     * keeps big = 1: every vector is an eigenvector. */
    for (i = 0; i < n * n; i++) {
        if (fabsf(m->data[i]) > big) big = fabsf(m->data[i]);
    }
    if (big == 0) big = 1.0f;
    for (i = 0; i < n * n; i++) a[i] = m->data[i] / big;
    for (i = 0; i < n; i++) sh += a[i * (n + 1)];
    sh /= (ce_float_t)n;
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            ce_float_t dv = fabsf(a[i * n + j] - (i == j ? sh : 0));
            if (dv > cn) cn = dv;
        }
    }
    /* Pivot and residual tolerances: relative to max|A - s*I| (s =
     * trace/n), plus about 8 ulp of max|A| = 1 for the rounding of lambda
     * itself. */
    tol = cn * EIG_REL_TOL + 1e-6f;
    res_tol = cn * EIG_RES_TOL + 1e-5f;

    for (k = 0; k < n_eigenvals; k++) {
        ce_float_t lambda = eigenvals[k] / big;
        ce_float_t *v = eigenvecs[k];
        ce_float_t res;
        int free_col, skip = 0;

        /* Rows of B; a row swap swaps the pointers. */
        for (i = 0; i < n; i++) {
            row[i] = work + i * n;
            used[i] = false;
            for (j = 0; j < n; j++)
                row[i][j] = a[i * n + j] - (i == j ? lambda : 0);
        }

        for (rank = 0; rank < n - 1; rank++) {
            ce_float_t best = 0, div, *t;
            int bi = -1, bj = -1;
            for (i = rank; i < n; i++) {
                for (j = 0; j < n; j++) {
                    if (!used[j] && fabsf(row[i][j]) > best) {
                        best = fabsf(row[i][j]);
                        bi = i;
                        bj = j;
                    }
                }
            }
            if (bi < 0 || best <= tol) break;
            t = row[bi]; row[bi] = row[rank]; row[rank] = t;
            /* Codegen note.  The loops skip column bj and set it
             * afterwards.  ez80-clang (CEdev v11.2, -Oz LTO) kept the low
             * 24 bits of div in a stack slot but re-read its exponent byte
             * from t[bj] inside the loop, after the loop had overwritten
             * it (seen in obj/CEMAT/lto.src): on the OS 5.8 ROM div = 2
             * became 0.5 and eigenvec(mat2(4,1,2,3)) failed, while the
             * host build was right.  mult has the same shape and gets the
             * same treatment as a precaution.  Never write the element a
             * hoisted float was loaded from inside the same loop. */
            div = t[bj];
            for (j = 0; j < n; j++) {
                if (j != bj) t[j] /= div;
            }
            t[bj] = 1.0f;
            for (i = 0; i < n; i++) {
                ce_float_t mult, *ri = row[i];
                if (i == rank) continue;
                mult = ri[bj];
                for (j = 0; j < n; j++) {
                    if (j != bj) ri[j] -= mult * t[j];
                }
                ri[bj] = 0;
            }
            used[bj] = true;
            pivcol[rank] = (signed char)bj;
        }

        /* Each free column gives a candidate vector (that free variable 1,
         * the other free ones 0, each pivot variable from its row).  Keep
         * the one with the smallest residual |A v - lambda v|: when the
         * pivot tolerance leaves several free columns (eigenvalues closer
         * than tol), the column order alone gave diag(1000,1.05,1) swapped
         * vectors.  An eigenvalue repeated exactly (a double or triple root
         * is returned as equal values) skips the candidates its earlier
         * copies took, so A = c*I gives independent vectors. */
        for (j = 0; j < k; j++) {
            if (eigenvals[j] == eigenvals[k]) skip++;
        }
        {
            ce_float_t cres[MAX_MATRIX_DIM];
            bool taken[MAX_MATRIX_DIM];
            int c, pass;
            for (c = 0; c < n; c++) {
                taken[c] = used[c];
                cres[c] = 0;
                if (used[c]) continue;
                eig_candidate(n, row, pivcol, rank, c, v);
                cres[c] = eig_residual(a, n, lambda, v);
            }
            /* The (skip+1)-th smallest residual; the loops only read
             * cres (codegen note above), and nothing is taken twice. */
            free_col = -1;
            for (pass = 0; pass <= skip; pass++) {
                int best = -1;
                for (c = 0; c < n; c++) {
                    if (!taken[c] && (best < 0 || cres[c] < cres[best]))
                        best = c;
                }
                if (best < 0) break;     /* fewer free columns than copies */
                taken[best] = true;
                free_col = best;
            }
            eig_candidate(n, row, pivcol, rank, free_col, v);
            res = cres[free_col];
        }
        if (!(res <= res_tol)) {
            for (j = 0; j < n; j++) v[j] = 0;
        }
    }
    return n_eigenvals;
}

/* ── Rank & Nullity ─────────────────────────────────────── */

int matrix_rank(const ce_matrix_t *m)
{
    ce_matrix_t tmp;
    int r, rank = 0;

    if (!m) return 0;
    tmp = *m;  /* copy */
    matrix_rref(&tmp);

    for (r = 0; r < tmp.rows; r++) {
        int c;
        bool nonzero = false;
        for (c = 0; c < tmp.cols; c++) {
            if (fabsf(MAT_AT(&tmp, r, c)) > MAT_EPS) {
                nonzero = true;
                break;
            }
        }
        if (nonzero) rank++;
    }
    return rank;
}

int matrix_nullity(const ce_matrix_t *m)
{
    if (!m) return 0;
    return m->cols - matrix_rank(m);
}

/* ── Dot & Cross Product ────────────────────────────────── */

static int vec_length(const ce_matrix_t *v)
{
    if (v->rows == 1) return v->cols;
    if (v->cols == 1) return v->rows;
    return 0;  /* not a vector */
}

static ce_float_t vec_elem(const ce_matrix_t *v, int i)
{
    if (v->rows == 1) return MAT_AT(v, 0, i);
    return MAT_AT(v, i, 0);
}

ce_float_t matrix_dot(const ce_matrix_t *a, const ce_matrix_t *b)
{
    int la, lb, i;
    ce_float_t sum = 0;

    la = vec_length(a);
    lb = vec_length(b);
    if (la == 0 || la != lb) return 0;

    for (i = 0; i < la; i++)
        sum += vec_elem(a, i) * vec_elem(b, i);
    return sum;
}

bool matrix_cross(const ce_matrix_t *a, const ce_matrix_t *b, ce_matrix_t *out)
{
    int la, lb;
    ce_float_t a0, a1, a2, b0, b1, b2;

    la = vec_length(a);
    lb = vec_length(b);
    if (la != 3 || lb != 3) return false;

    a0 = vec_elem(a, 0); a1 = vec_elem(a, 1); a2 = vec_elem(a, 2);
    b0 = vec_elem(b, 0); b1 = vec_elem(b, 1); b2 = vec_elem(b, 2);

    /* Output as column vector */
    matrix_init(out, 3, 1);
    MAT_AT(out, 0, 0) = a1*b2 - a2*b1;
    MAT_AT(out, 1, 0) = a2*b0 - a0*b2;
    MAT_AT(out, 2, 0) = a0*b1 - a1*b0;
    return true;
}

/* ── LU Decomposition (Doolittle) ───────────────────────── */

bool matrix_lu(const ce_matrix_t *m, ce_matrix_t *L, ce_matrix_t *U)
{
    int n, i, j, k;
    ce_float_t sum;

    if (m->rows != m->cols || m->rows == 0) return false;
    n = m->rows;

    matrix_init(L, (uint8_t)n, (uint8_t)n);
    matrix_init(U, (uint8_t)n, (uint8_t)n);

    for (i = 0; i < n; i++) {
        /* Upper triangular U */
        for (j = i; j < n; j++) {
            sum = 0;
            for (k = 0; k < i; k++)
                sum += MAT_AT(L, i, k) * MAT_AT(U, k, j);
            MAT_AT(U, i, j) = MAT_AT(m, i, j) - sum;
        }

        /* Check for singular (zero pivot) */
        if (fabsf(MAT_AT(U, i, i)) < MAT_EPS) return false;

        /* Lower triangular L (unit diagonal) */
        MAT_AT(L, i, i) = 1.0f;
        for (j = i + 1; j < n; j++) {
            sum = 0;
            for (k = 0; k < i; k++)
                sum += MAT_AT(L, j, k) * MAT_AT(U, k, i);
            MAT_AT(L, j, i) = (MAT_AT(m, j, i) - sum) / MAT_AT(U, i, i);
        }
    }

    return true;
}

/* ── Display ────────────────────────────────────────────── */

int matrix_to_str(const ce_matrix_t *m, char *buf, int size)
{
    int pos = 0, i, j;
    char tmp[24];
    if (!buf || size <= 0) return 0;

    for (i = 0; i < m->rows && pos < size - 2; i++) {
        if (i > 0) {
            if (pos < size - 1) buf[pos++] = '\n';
        }
        if (pos < size - 1) buf[pos++] = '[';
        for (j = 0; j < m->cols && pos < size - 8; j++) {
            if (j > 0) {
                if (pos < size - 1) buf[pos++] = ' ';
            }
            snprintf(tmp, sizeof(tmp), "%.4g", (double)MAT_AT(m, i, j));
            {
                int k;
                for (k = 0; tmp[k] && pos < size - 1; k++)
                    buf[pos++] = tmp[k];
            }
        }
        if (pos < size - 1) buf[pos++] = ']';
    }
    buf[pos < size ? pos : size - 1] = '\0';
    return pos;
}
