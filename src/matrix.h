/*
 * matrix.h — Matrix operations for CalcEngine
 * Fixed-size matrices up to 6x6 (suitable for TI-84 CE).
 */
#ifndef MATRIX_H
#define MATRIX_H

#include "calcengine.h"

#define MAX_MATRIX_DIM 6

/* ── Matrix type ────────────────────────────────────────── */
typedef struct {
    uint8_t    rows;
    uint8_t    cols;
    ce_float_t data[MAX_MATRIX_DIM * MAX_MATRIX_DIM];  /* row-major */
} ce_matrix_t;

/* ── Initialisation ─────────────────────────────────────── */
void matrix_init(ce_matrix_t *m, uint8_t rows, uint8_t cols);

/* ── Element access macros ──────────────────────────────── */
#define MAT_AT(m, r, c)  ((m)->data[(r) * (m)->cols + (c)])

/* ── Arithmetic ─────────────────────────────────────────── */
bool matrix_add(const ce_matrix_t *a, const ce_matrix_t *b, ce_matrix_t *out);
bool matrix_sub(const ce_matrix_t *a, const ce_matrix_t *b, ce_matrix_t *out);
bool matrix_mul(const ce_matrix_t *a, const ce_matrix_t *b, ce_matrix_t *out);
void matrix_scale(const ce_matrix_t *a, ce_float_t s, ce_matrix_t *out);
void matrix_transpose(const ce_matrix_t *a, ce_matrix_t *out);

/* ── Linear algebra ─────────────────────────────────────── */
ce_float_t matrix_trace(const ce_matrix_t *m);
ce_float_t matrix_det(const ce_matrix_t *m);
bool matrix_inverse(const ce_matrix_t *m, ce_matrix_t *out);
void matrix_rref(ce_matrix_t *m);

/* ── Eigenvalues ────────────────────────────────────────── */
/* Real eigenvalues of a square 1x1..3x3 matrix, with multiplicity, largest
 * first.  max_vals must be at least the matrix size.  Returns the count
 * (n, or 1 for a 3x3 with a complex pair), 0 if none are real (or the
 * matrix is not square, or max_vals is too small), -1 if larger than 3x3,
 * -2 if an entry is NaN or above 1e37 in magnitude. */
int matrix_eigenvalues(const ce_matrix_t *m, ce_float_t *eigenvals, int max_vals);

/* ── Eigenvectors ───────────────────────────────────────── */
/* Compute eigenvectors for given eigenvalues by solving (A - λI)v = 0.
 * eigenvals[]: input eigenvalues, n_eigenvals: count.
 * eigenvecs[][MAX_MATRIX_DIM]: output eigenvectors (one per eigenvalue),
 * scaled so the largest component has magnitude 1.  A vector that does not
 * satisfy A v = lambda v to the tolerance in matrix.c is all zeros.  An
 * eigenvalue repeated more times than it has independent vectors
 * (defective) repeats the last one found.
 * Returns the number of vectors written (min(n_eigenvals, n)). */
int matrix_eigenvectors(const ce_matrix_t *m, const ce_float_t eigenvals[],
                        ce_float_t eigenvecs[][MAX_MATRIX_DIM], int n_eigenvals);

/* ── Rank & Nullity ─────────────────────────────────────── */
int matrix_rank(const ce_matrix_t *m);
int matrix_nullity(const ce_matrix_t *m);

/* ── LU Decomposition ──────────────────────────────────── */
/* Doolittle LU decomposition: A = LU where L is lower (unit diag), U is upper.
 * Returns false if non-square or singular. */
bool matrix_lu(const ce_matrix_t *m, ce_matrix_t *L, ce_matrix_t *U);

/* ── Dot & Cross Product ────────────────────────────────── */
/* Dot product of two vectors (row or column). Returns 0 on dimension mismatch. */
ce_float_t matrix_dot(const ce_matrix_t *a, const ce_matrix_t *b);
/* Cross product of two 3-vectors. Returns false if not 3D. */
bool matrix_cross(const ce_matrix_t *a, const ce_matrix_t *b, ce_matrix_t *out);

/* ── Display ────────────────────────────────────────────── */
/* Format matrix as multi-line string: "[1 2; 3 4]" style */
int matrix_to_str(const ce_matrix_t *m, char *buf, int size);

#endif /* MATRIX_H */
