#include "split/astwire.h"

#include <string.h>

#include "memory.h"

#define CE_AST_WIRE_NULL 0u
#define CE_AST_WIRE_NODE 1u
#define CE_AST_WIRE_MAX_DEPTH 128

typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint16_t pos;
    bool ok;
} wire_writer_t;

typedef struct {
    const uint8_t *buf;
    uint16_t size;
    uint16_t pos;
    bool ok;
} wire_reader_t;

static void w_u8(wire_writer_t *w, uint8_t v)
{
    if (!w || !w->ok || w->pos >= w->cap) {
        if (w) w->ok = false;
        return;
    }
    w->buf[w->pos++] = v;
}

static void w_u16(wire_writer_t *w, uint16_t v)
{
    w_u8(w, (uint8_t)(v & 0xFFu));
    w_u8(w, (uint8_t)((v >> 8) & 0xFFu));
}

static void w_f32(wire_writer_t *w, ce_float_t v)
{
    uint8_t tmp[sizeof(ce_float_t)];
    memcpy(tmp, &v, sizeof(tmp));
    if (!w || !w->ok || (uint16_t)(w->cap - w->pos) < (uint16_t)sizeof(tmp)) {
        if (w) w->ok = false;
        return;
    }
    memcpy(&w->buf[w->pos], tmp, sizeof(tmp));
    w->pos = (uint16_t)(w->pos + sizeof(tmp));
}

static bool r_u8(wire_reader_t *r, uint8_t *out)
{
    if (!r || !out || !r->ok || r->pos >= r->size) {
        if (r) r->ok = false;
        return false;
    }
    *out = r->buf[r->pos++];
    return true;
}

static bool r_u16(wire_reader_t *r, uint16_t *out)
{
    uint8_t lo, hi;
    if (!r_u8(r, &lo) || !r_u8(r, &hi)) return false;
    *out = (uint16_t)lo | ((uint16_t)hi << 8);
    return true;
}

static bool r_f32(wire_reader_t *r, ce_float_t *out)
{
    uint8_t tmp[sizeof(ce_float_t)];
    if (!r || !out || !r->ok || (uint16_t)(r->size - r->pos) < (uint16_t)sizeof(tmp)) {
        if (r) r->ok = false;
        return false;
    }
    memcpy(tmp, &r->buf[r->pos], sizeof(tmp));
    memcpy(out, tmp, sizeof(tmp));
    r->pos = (uint16_t)(r->pos + sizeof(tmp));
    return true;
}

static void encode_node(wire_writer_t *w, const ast_node_t *node, uint8_t depth)
{
    if (!w || !w->ok) return;
    if (depth > CE_AST_WIRE_MAX_DEPTH) {
        w->ok = false;
        return;
    }

    if (!node) {
        w_u8(w, CE_AST_WIRE_NULL);
        return;
    }

    w_u8(w, CE_AST_WIRE_NODE);
    w_u8(w, (uint8_t)node->type);

    switch (node->type) {
    case NODE_NUMBER:
        w_f32(w, node->d.number);
        break;
    case NODE_VARIABLE:
        w_u8(w, (uint8_t)node->d.variable);
        break;
    case NODE_CONSTANT:
        w_u8(w, (uint8_t)node->d.constant);
        break;
    case NODE_BINOP:
        w_u8(w, (uint8_t)node->d.binop.op);
        encode_node(w, node->d.binop.left, (uint8_t)(depth + 1));
        encode_node(w, node->d.binop.right, (uint8_t)(depth + 1));
        break;
    case NODE_UNOP:
        w_u8(w, (uint8_t)node->d.unop.op);
        encode_node(w, node->d.unop.operand, (uint8_t)(depth + 1));
        break;
    case NODE_FUNC:
        w_u8(w, (uint8_t)node->d.func.id);
        encode_node(w, node->d.func.arg, (uint8_t)(depth + 1));
        break;
    case NODE_DERIV:
        w_u8(w, (uint8_t)node->d.deriv.var);
        encode_node(w, node->d.deriv.expr, (uint8_t)(depth + 1));
        break;
    case NODE_INTEGRAL:
        w_u8(w, (uint8_t)node->d.integral.var);
        encode_node(w, node->d.integral.expr, (uint8_t)(depth + 1));
        encode_node(w, node->d.integral.lo, (uint8_t)(depth + 1));
        encode_node(w, node->d.integral.hi, (uint8_t)(depth + 1));
        break;
    case NODE_LIMIT:
        w_u8(w, (uint8_t)node->d.limit.var);
        w_u8(w, (uint8_t)node->d.limit.direction);
        encode_node(w, node->d.limit.expr, (uint8_t)(depth + 1));
        encode_node(w, node->d.limit.point, (uint8_t)(depth + 1));
        break;
    case NODE_SUM:
        w_u8(w, (uint8_t)node->d.sum.var);
        encode_node(w, node->d.sum.expr, (uint8_t)(depth + 1));
        encode_node(w, node->d.sum.lo, (uint8_t)(depth + 1));
        encode_node(w, node->d.sum.hi, (uint8_t)(depth + 1));
        break;
    case NODE_TAYLOR:
        w_u8(w, (uint8_t)node->d.taylor.var);
        encode_node(w, node->d.taylor.expr, (uint8_t)(depth + 1));
        encode_node(w, node->d.taylor.center, (uint8_t)(depth + 1));
        encode_node(w, node->d.taylor.degree, (uint8_t)(depth + 1));
        break;
    case NODE_FOURIER:
        w_u8(w, (uint8_t)node->d.fourier.var);
        encode_node(w, node->d.fourier.expr, (uint8_t)(depth + 1));
        encode_node(w, node->d.fourier.nterms, (uint8_t)(depth + 1));
        break;
    case NODE_EXPAND:
        w_u8(w, (uint8_t)node->d.expand.var);
        encode_node(w, node->d.expand.expr, (uint8_t)(depth + 1));
        break;
    case NODE_FACTOR:
        w_u8(w, (uint8_t)node->d.factor.var);
        encode_node(w, node->d.factor.expr, (uint8_t)(depth + 1));
        break;
    case NODE_SOLVE:
        w_u8(w, (uint8_t)node->d.solve.var);
        encode_node(w, node->d.solve.expr, (uint8_t)(depth + 1));
        break;
    case NODE_PAIR:
        encode_node(w, node->d.pair.head, (uint8_t)(depth + 1));
        encode_node(w, node->d.pair.tail, (uint8_t)(depth + 1));
        break;
    case NODE_STAT:
        w_u16(w, (uint16_t)node->d.stat.fn);
        encode_node(w, node->d.stat.args, (uint8_t)(depth + 1));
        break;
    case NODE_TANGENT:
        w_u8(w, (uint8_t)node->d.tangent.var);
        encode_node(w, node->d.tangent.expr, (uint8_t)(depth + 1));
        encode_node(w, node->d.tangent.point, (uint8_t)(depth + 1));
        break;
    case NODE_MATRIX:
        w_u8(w, node->d.matrix.store_idx);
        w_u8(w, node->d.matrix.rows);
        w_u8(w, node->d.matrix.cols);
        break;
    case NODE_MATRIX_OP:
        w_u8(w, (uint8_t)node->d.matrix_op.op);
        w_u8(w, node->d.matrix_op.rows);
        w_u8(w, node->d.matrix_op.cols);
        encode_node(w, node->d.matrix_op.arg, (uint8_t)(depth + 1));
        break;
    case NODE_POLAR:
        w_u8(w, (uint8_t)node->d.polar.op);
        w_u8(w, (uint8_t)node->d.polar.var);
        encode_node(w, node->d.polar.expr1, (uint8_t)(depth + 1));
        encode_node(w, node->d.polar.expr2, (uint8_t)(depth + 1));
        encode_node(w, node->d.polar.lo, (uint8_t)(depth + 1));
        encode_node(w, node->d.polar.hi, (uint8_t)(depth + 1));
        break;
    case NODE_LIST_LITERAL:
        encode_node(w, node->d.list_literal.elems, (uint8_t)(depth + 1));
        break;
    case NODE_LIST_REF:
        w_u8(w, node->d.list_ref.idx);
        break;
    case NODE_LIST_OP:
        w_u8(w, node->d.list_op.fn);
        encode_node(w, node->d.list_op.arg, (uint8_t)(depth + 1));
        break;
    case NODE_LIST_ELEM:
        w_u8(w, node->d.list_elem.list_idx);
        encode_node(w, node->d.list_elem.index, (uint8_t)(depth + 1));
        break;
    case NODE_STORE:
        w_u8(w, node->d.store.list_idx);
        encode_node(w, node->d.store.expr, (uint8_t)(depth + 1));
        break;
    case NODE_PIECEWISE:
        encode_node(w, node->d.piecewise.pieces, (uint8_t)(depth + 1));
        break;
    case NODE_PRODUCT:
        w_u8(w, (uint8_t)node->d.product.var);
        encode_node(w, node->d.product.expr, (uint8_t)(depth + 1));
        encode_node(w, node->d.product.lo, (uint8_t)(depth + 1));
        encode_node(w, node->d.product.hi, (uint8_t)(depth + 1));
        break;
    default:
        w->ok = false;
        break;
    }
}

static ast_node_t *decode_node(wire_reader_t *r, uint8_t depth)
{
    uint8_t marker;
    uint8_t type_u8;
    ast_node_t *node;

    if (!r || !r->ok) return NULL;
    if (depth > CE_AST_WIRE_MAX_DEPTH) {
        r->ok = false;
        return NULL;
    }
    if (!r_u8(r, &marker)) return NULL;
    if (marker == CE_AST_WIRE_NULL) return NULL;
    if (marker != CE_AST_WIRE_NODE) {
        r->ok = false;
        return NULL;
    }
    if (!r_u8(r, &type_u8)) return NULL;

    node = mem_alloc_node();
    if (!node) {
        r->ok = false;
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->type = (node_type_t)type_u8;

    switch (node->type) {
    case NODE_NUMBER:
        if (!r_f32(r, &node->d.number)) goto fail;
        break;
    case NODE_VARIABLE: {
        uint8_t v;
        if (!r_u8(r, &v)) goto fail;
        node->d.variable = (char)v;
        break;
    }
    case NODE_CONSTANT: {
        uint8_t v;
        if (!r_u8(r, &v)) goto fail;
        node->d.constant = (const_id_t)v;
        break;
    }
    case NODE_BINOP: {
        uint8_t op;
        if (!r_u8(r, &op)) goto fail;
        node->d.binop.op = (binop_t)op;
        node->d.binop.left = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.binop.right = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_UNOP: {
        uint8_t op;
        if (!r_u8(r, &op)) goto fail;
        node->d.unop.op = (unop_t)op;
        node->d.unop.operand = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_FUNC: {
        uint8_t id;
        if (!r_u8(r, &id)) goto fail;
        node->d.func.id = (func_id_t)id;
        node->d.func.arg = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_DERIV: {
        uint8_t var;
        if (!r_u8(r, &var)) goto fail;
        node->d.deriv.var = (char)var;
        node->d.deriv.expr = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_INTEGRAL: {
        uint8_t var;
        if (!r_u8(r, &var)) goto fail;
        node->d.integral.var = (char)var;
        node->d.integral.expr = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.integral.lo = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.integral.hi = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_LIMIT: {
        uint8_t var, dir_u8;
        if (!r_u8(r, &var) || !r_u8(r, &dir_u8)) goto fail;
        node->d.limit.var = (char)var;
        node->d.limit.direction = (int8_t)dir_u8;
        node->d.limit.expr = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.limit.point = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_SUM: {
        uint8_t var;
        if (!r_u8(r, &var)) goto fail;
        node->d.sum.var = (char)var;
        node->d.sum.expr = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.sum.lo = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.sum.hi = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_TAYLOR: {
        uint8_t var;
        if (!r_u8(r, &var)) goto fail;
        node->d.taylor.var = (char)var;
        node->d.taylor.expr = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.taylor.center = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.taylor.degree = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_FOURIER: {
        uint8_t var;
        if (!r_u8(r, &var)) goto fail;
        node->d.fourier.var = (char)var;
        node->d.fourier.expr = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.fourier.nterms = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_EXPAND: {
        uint8_t var;
        if (!r_u8(r, &var)) goto fail;
        node->d.expand.var = (char)var;
        node->d.expand.expr = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_FACTOR: {
        uint8_t var;
        if (!r_u8(r, &var)) goto fail;
        node->d.factor.var = (char)var;
        node->d.factor.expr = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_SOLVE: {
        uint8_t var;
        if (!r_u8(r, &var)) goto fail;
        node->d.solve.var = (char)var;
        node->d.solve.expr = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_PAIR:
        node->d.pair.head = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.pair.tail = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    case NODE_STAT: {
        uint16_t fn;
        if (!r_u16(r, &fn)) goto fail;
        node->d.stat.fn = (stat_fn_t)fn;
        node->d.stat.args = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_TANGENT: {
        uint8_t var;
        if (!r_u8(r, &var)) goto fail;
        node->d.tangent.var = (char)var;
        node->d.tangent.expr = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.tangent.point = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_MATRIX: {
        if (!r_u8(r, &node->d.matrix.store_idx) ||
            !r_u8(r, &node->d.matrix.rows) ||
            !r_u8(r, &node->d.matrix.cols)) goto fail;
        break;
    }
    case NODE_MATRIX_OP: {
        uint8_t op;
        if (!r_u8(r, &op) ||
            !r_u8(r, &node->d.matrix_op.rows) ||
            !r_u8(r, &node->d.matrix_op.cols)) goto fail;
        node->d.matrix_op.op = (matrix_op_t)op;
        node->d.matrix_op.arg = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_POLAR: {
        uint8_t op, var;
        if (!r_u8(r, &op) || !r_u8(r, &var)) goto fail;
        node->d.polar.op = (polar_op_t)op;
        node->d.polar.var = (char)var;
        node->d.polar.expr1 = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.polar.expr2 = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.polar.lo = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.polar.hi = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    case NODE_LIST_LITERAL:
        node->d.list_literal.elems = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    case NODE_LIST_REF:
        if (!r_u8(r, &node->d.list_ref.idx)) goto fail;
        break;
    case NODE_LIST_OP:
        if (!r_u8(r, &node->d.list_op.fn)) goto fail;
        node->d.list_op.arg = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    case NODE_LIST_ELEM:
        if (!r_u8(r, &node->d.list_elem.list_idx)) goto fail;
        node->d.list_elem.index = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    case NODE_STORE:
        if (!r_u8(r, &node->d.store.list_idx)) goto fail;
        node->d.store.expr = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    case NODE_PIECEWISE:
        node->d.piecewise.pieces = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    case NODE_PRODUCT: {
        uint8_t var;
        if (!r_u8(r, &var)) goto fail;
        node->d.product.var = (char)var;
        node->d.product.expr = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.product.lo = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        node->d.product.hi = decode_node(r, (uint8_t)(depth + 1));
        if (!r->ok) goto fail;
        break;
    }
    default:
        goto fail;
    }

    return node;

fail:
    r->ok = false;
    ast_free(node);
    return NULL;
}

bool ce_ast_serialize(const ast_node_t *root,
                      uint8_t *buf,
                      uint16_t cap,
                      uint16_t *out_size)
{
    wire_writer_t w;
    if (!buf || !out_size || cap == 0) return false;
    w.buf = buf;
    w.cap = cap;
    w.pos = 0;
    w.ok = true;
    encode_node(&w, root, 0);
    if (!w.ok) return false;
    *out_size = w.pos;
    return true;
}

ast_node_t *ce_ast_deserialize(const uint8_t *buf, uint16_t size)
{
    wire_reader_t r;
    ast_node_t *root;
    if (!buf || size == 0) return NULL;
    r.buf = buf;
    r.size = size;
    r.pos = 0;
    r.ok = true;

    root = decode_node(&r, 0);
    if (!r.ok || r.pos != r.size) {
        if (root) ast_free(root);
        return NULL;
    }
    return root;
}
