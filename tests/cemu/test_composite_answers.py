"""Exercise composite answers with each worker's production feature flags."""
from pathlib import Path
import shutil
import sys
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_module_flag_parity as parity

REL_INPUTS = [
    "relrate((x+y+z+r+s+w+u+v+a+b)^123456)",
    "relrate((x+y+z+r+s+w+u+v+a+b)^2)",
    # One partial exceeds the 64-byte term buffer while the whole answer
    # would fit: the printer cut marker alone must refuse it.
    "relrate(sin(x^2+x^3+x^4+x^5+x^6+x^7+y))",
    "relrate(sin(x+y+z+r+s+w+u+v+a+b))",
    "relrate(x+y)",
]


def build_checked(tmp_path, monkeypatch, sanitize=False, mode="bounds"):
    """Check the member bound before pointer formation, not just ASan."""
    root = tmp_path / "source"
    shutil.copytree(parity.ROOT / "src", root / "src")
    (root / "tests").symlink_to(parity.ROOT / "tests", target_is_directory=True)
    (root / "split").symlink_to(parity.ROOT / "split", target_is_directory=True)
    p = root / "src/split/processor.c"
    source = p.read_text()
    needle = "snprintf(st.result + rlen, sizeof(st.result) - rlen,"
    assert source.count(needle) >= 2
    source = source.replace(needle, "checked_result_write(st.result, sizeof(st.result), rlen,")
    helper = r'''
#include <assert.h>
#include <stdarg.h>
/* Check the result member, which ASan cannot isolate within proc_state_t. */
static int checked_result_write(char *base, size_t cap, int pos,
                                const char *fmt, ...) {
    int n;
    va_list ap;
    unsigned char before[128];
    memcpy(before, base + cap, sizeof(before));
#if CHECK_MEMBER_OFFSET
    if (pos < 0 || (size_t)pos >= cap)
        fprintf(stderr, "INVALID_RESULT_OFFSET pos=%d cap=%zu\n", pos, cap);
    assert(pos >= 0 && (size_t)pos < cap);
#else
    /* error[128] follows result without padding. Permit writes there only
     * to demonstrate intra-struct corruption, never outside the struct. */
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    assert(pos >= 0 && n >= 0 && (size_t)(pos + n) < cap + 128);
#endif
    va_start(ap, fmt);
    n = vsnprintf(base + pos, cap - (size_t)pos, fmt, ap);
    va_end(ap);
    if (memcmp(before, base + cap, sizeof(before)))
        fprintf(stderr, "RESULT_MEMBER_CORRUPTION pos=%d cap=%zu required=%d\n", pos, cap, n);
    assert(memcmp(before, base + cap, sizeof(before)) == 0);
    return n;
}
'''
    helper = f"#define CHECK_MEMBER_OFFSET {int(mode == 'bounds')}\n" + helper
    # Place after source includes so CE formatting redirects remain in effect.
    at = source.index("typedef struct")
    source = source[:at] + helper + source[at:]
    p.write_text(source)
    monkeypatch.setattr(parity, "ROOT", root)
    return parity._build_module_driver(root / "split/Makefile.calc.lim.rel",
                                      tmp_path, sanitize=sanitize)


@pytest.mark.parametrize("sanitize", [False, True])
@pytest.mark.parametrize("mode", ["bounds", "canary"])
def test_related_rate_member_write_bounds(tmp_path, monkeypatch, sanitize, mode):
    exe = build_checked(tmp_path, monkeypatch, sanitize=sanitize, mode=mode)
    rows = parity._run_stats(exe, REL_INPUTS)
    assert [row[0] for row in rows] == [False, False, False, True, True]
    for expr, (ok, text, leaked, double_free) in zip(REL_INPUTS, rows):
        assert len(expr) <= 127
        assert leaked == double_free == 0
        assert "..." not in text
        if ok:
            last_var = "y" if expr == "relrate(x+y)" else "b"
            assert text.endswith(f"d{last_var}/dt")
            assert text.count("/dt") == (3 if last_var == "y" else 11)
        else:
            assert text == "Related rate result incomplete"
    assert rows[-1][:2] == (True, "d/dt = (1+0)*dx/dt + (0+1)*dy/dt")
