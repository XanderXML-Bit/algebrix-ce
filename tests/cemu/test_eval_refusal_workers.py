"""Eval refusals are errors, never values, in the real worker configs.

Desktop builds of each worker with its own makefile flags (CE_EVAL_MAX_DEPTH
included), driven through the launcher's AST wire format. This is not CE
stack or formatting evidence: tests/cemu/test_realrom_eval_refusals.py runs
the same kind of inputs on the owned OS 5.8 ROM.

These callers used to turn eval errors into numeric results:
fourier() of a 55-term sum (deeper than the 50-level cap) answered
"Fourier_2 = 0", fourier(1/x) and fourier(ln(x)) answered 0, recseq() stopped at the term
before a failed one and called it s(k), nlsolve() listed
Newton starts where the equations failed to evaluate as roots, and the
volume/length workers printed "= nan" or used 0 for a bound that failed.
"""
from pathlib import Path

import pytest

import test_module_flag_parity as parity

S55 = "+".join(["x"] * 55)
S51 = "+".join(["x"] * 51)
S20 = "+".join(["x"] * 20)
S13 = "+".join(["x"] * 13)
N51 = "+".join(["n"] * 51)
CAP = "Eval recursion limit"


@pytest.fixture(scope="module")
def workers(tmp_path_factory):
    out = tmp_path_factory.mktemp("refusal_workers")
    built = {}
    for name in ("calc.series", "algsy", "calc.pol.num", "calc.seq",
                 "calc.lim.base", "calc.lim.adv", "calc.lim.cont"):
        makefile = parity.SPLIT / f"Makefile.{name}"
        values = parity._makefile_vars(makefile)
        entries = {s for s in values["PROGRAM_SOURCES"].split()
                   if s.startswith("src/modules/") and s.endswith("main.c")}
        with pytest.MonkeyPatch.context() as mp:
            mp.setattr(parity, "_HOST_EXCLUDED_SOURCES",
                       parity._HOST_EXCLUDED_SOURCES | entries)
            built[name] = parity._build_module_driver(makefile, out)
    return built


def run(exe: Path, exprs):
    rows = parity._run_stats(exe, exprs)
    for expr, (_, _, leaked, double_free) in zip(exprs, rows):
        assert leaked in (0, None) and double_free in (0, None), (expr, leaked, double_free)
    return [(ok, text) for ok, text, _, _ in rows]


def first_line(text: str) -> str:
    return text.split("\\n", 1)[0].split("\n", 1)[0]


def test_fourier_refuses_failed_samples_and_keeps_shallow_answers(workers):
    exprs = [f"fourier({S55},x,2)", "fourier(x,x,2)",
             "fourier(1/x,x,2)", "fourier(ln(x),x,2)", "fourier(sqrt(x),x,1)",
             "fourier(sin(x)/x,x,2)", "fourier(abs(x),x,2)", "fourier(x^2,x,3)"]
    rows = run(workers["calc.series"], exprs)
    assert rows[0] == (False, CAP), rows[0]
    # The refusal does not poison the next request of the same process.
    assert rows[1][0] and rows[1][1].startswith("Fourier_2 = 1.19209e-08+"), rows[1]
    assert "+2sin(1x)+" in rows[1][1], rows[1]
    for ok, text in rows[2:6]:
        assert (ok, text) == (False, "Could not compute Fourier series"), (ok, text)
    assert rows[6][0] and rows[6][1].startswith("Fourier_2 = 1.5708+(-1.27324cos"), rows[6]
    assert rows[7][0] and rows[7][1].startswith("Fourier_3 = 3.28987+(-4cos"), rows[7]


def test_fourier_poles_between_nodes_and_finite_controls(workers):
    poles = ["1/x", "1/(x-1)", "tan(x)", "1/(x-1)^2", "1/x^2",
             "1/(x-3)", "1/(x+2.5)", "1/(x-pi)", "1/(x+pi)^2",
             "1/(1e-12+(x-1)^2)",
             "1/(x-0.03141594)^2", "1/(x+3.1101768)^2", "1/(x-1)^2-1e6*x", "1/(x-1)+10000", "1/(x-1)^2+1e9", "x^2/x^3", "abs(x)^(-2)"]
    poles.append("1/(x+4)+1/(x+5)+1/(x+6)+1/(x+7)+1/(x+8)+1/(x+9)+1/(x+10)+1/(x+11)+1/(x+12)+1/(x-1)")
    poles.append("1/(x-1)-1e7*x")
    poles.append("abs(x-1)^(-x)")
    poles.append("1/abs(x-1)^0.9")
    poles.append("(x-1)/(x-1)+1/(x-1)")
    poles.extend(f"1/(x-({c}))" for c in [-3.1, -1, 0.39, 1, 3.1])
    poles.extend(["(1/sqrt(abs(x-1)))^4", "(1/sqrt(abs(x-1)))^2*sign(x-1)"])
    rows = run(workers["calc.series"], [f"fourier({f},x,2)" for f in poles])
    assert rows == [(False, "Could not compute Fourier series")] * len(poles), rows
    controls = [
        ("fourier(piecewise(x^(----------1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1),0,1),x,2)", "Fourier_2 = 1"),
        ("fourier(piecewise(1/(----------x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x),0,1),x,2)", "Fourier_2 = 1"),
        ("fourier((x-1)^2*((x-1)^2-0.0179507*(x-1)+3.57854e-5)*(x-1)/(x-1),x,2)",
         "Fourier_2 = 40.4163+(-55.1724cos(1x))+(-39.2036sin(1x))+22.793cos(2x)+37.6826sin(2x)"),
        ("fourier(1/abs(x-0.39)^0.84,x,2)",
         "Fourier_2 = 1.53673+1.79268cos(1x)+0.733321sin(1x)+1.0589cos(2x)+1.04527sin(2x)"),
        # A failed owner-only depth probe must not poison the real result.
        ("fourier(piecewise((----------x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x+x)/(x-1),0,1),x,2)", "Fourier_2 = 1"),
        ("fourier(x,x,2)",
         "Fourier_2 = 1.19209e-08+2sin(1x)+(-1sin(2x))"),
        ("fourier(abs(x),x,2)",
         "Fourier_2 = 1.5708+(-1.27324cos(1x))"),
        ("fourier(x^2,x,3)",
         "Fourier_3 = 3.28987+(-4cos(1x))+0.999997cos(2x)+(-0.444437cos(3x))"),
        ("fourier(sin(x),x,2)",
         "Fourier_2 = 1.22256e-08+1sin(1x)"),
        ("fourier(cos(x),x,2)",
         "Fourier_2 = -4.13259e-08+1cos(1x)"),
        ("fourier(exp(x),x,2)",
         "Fourier_2 = 3.67608+(-3.67608cos(1x))+3.67608sin(1x)+1.47043cos(2x)+(-2.94086sin(2x))"),
        ("fourier(sin(10x),x,10)",
         "Fourier_10 = -1.18679e-07+1sin(10x)"),
        ("fourier(x^3-x,x,3)",
         "Fourier_3 = 7.43866e-07+(-1.69118e-06cos(1x))+5.73921sin(1x)+1.38601e-06cos(2x)+(-7.36961sin(2x))+(-2.19981e-06cos(3x))+5.46866sin(3x)"),
        ("fourier(sign(x),x,2)",
         "Fourier_2 = 0+1.27324sin(1x)"),
        ("fourier(floor(x),x,2)",
         "Fourier_2 = -0.496667+0.00666653cos(1x)+2.00993sin(1x)+0.00666682cos(2x)+(-0.986574sin(2x))"),
        ("fourier(1/sqrt(abs(x-1)),x,2)",
         "Fourier_2 = 1.10502+0.473556cos(1x)+0.70521sin(1x)+(-0.243985cos(2x))+0.46902sin(2x)"),
        ("fourier(1000000/(1+1000000*(x-1)^2),x,2)",
         "Fourier_2 = 238.117+254.973cos(1x)+401.367sin(1x)+(-201.68cos(2x))+429.474sin(2x)"),
        ("fourier(1/sqrt(abs(x-0.39)),x,2)",
         "Fourier_2 = 1.08636+0.711322cos(1x)+0.29094sin(1x)+0.331172cos(2x)+0.329996sin(2x)"),
        ("fourier(ln(abs(x-0.39)),x,2)",
         "Fourier_2 = 0.157994+(-1.09546cos(1x))+(-0.445999sin(1x))+(-0.298365cos(2x))+(-0.305754sin(2x))"),
        ("fourier(1/(0.01+(x-1)^2),x,2)",
         "Fourier_2 = 4.86603+4.90054cos(1x)+7.60415sin(1x)+(-3.40007cos(2x))+7.38665sin(2x)"),
        ("fourier(tan(x/100)+1/cos(x/100)+1/(x+4)+1/(x+5)+1/(x+6)+1/(x+7)+1/(x+8)+1/(x+9)+1/(x+10),x,2)",
         "Fourier_2 = 2.26288+(-0.226471cos(1x))+(-0.434359sin(1x))+0.08906cos(2x)+0.270387sin(2x)"),
        ("fourier(1/(x+4)+1/(x+5)+1/(x+6)+1/(x+7)+1/(x+8)+1/(x+9)+1/(x+10)+1/(x+11)+1/(x+12),x,2)",
         "Fourier_2 = 1.44155+(-0.231928cos(1x))+(-0.485719sin(1x))+0.0905253cos(2x)+0.296626sin(2x)"),
        ("fourier(1/(x+4)+1/(x+5)+1/(x+6)+1/(x+7)+1/(x+8)+1/(x+9)+1/(x+10)+1/(x+11)+1/(x+12)+(x-1)/(x-1),x,2)",
         "Fourier_2 = 2.44155+(-0.231928cos(1x))+(-0.48572sin(1x))+0.0905253cos(2x)+0.296626sin(2x)"),
        ("fourier(1/(0.01+(x-1)^2)+10.7*x,x,2)",
         "Fourier_2 = 4.86603+4.90054cos(1x)+29.0042sin(1x)+(-3.40007cos(2x))+(-3.31337sin(2x))"),
        ("fourier((x-1.00056)^2*(x-1)/(x-1),x,2)",
         "Fourier_2 = 4.29099+(-4cos(1x))+(-4.00224sin(1x))+0.999997cos(2x)+2.00112sin(2x)"),
        ("fourier(abs(x)^(x+5),x,2)",
         "Fourier_2 = 469.181+(-879.671cos(1x))+222.924sin(1x)+739.053cos(2x)+(-381.605sin(2x))"),
        ("fourier(abs(x-1)^x,x,2)",
         "Fourier_2 = 1.08081+(-0.935029cos(1x))+0.490435sin(1x)+1.05403cos(2x)+(-0.890678sin(2x))"),
        # Compatibility only: non-integrable known miss, not a valid series.
        ("fourier(1/(x-1)+1e12,x,2)",
         "Fourier_2 = 1e+12+(-56361cos(1x))+(-46785.7sin(1x))+122771cos(2x)+31967.6sin(2x)"),
        ("fourier(1/(0.0001+(x-1)^2)+55800000*(x-1)^2,x,2)",
         "Fourier_2 = 2.39375e+08+(-2.232e+08cos(1x))+(-2.232e+08sin(1x))+5.57998e+07cos(2x)+1.116e+08sin(2x)"),
        ("fourier((x-1.00299)^3*(x-1)/(x-1),x,2)",
         "Fourier_2 = -10.9081+12.0359cos(1x)+13.7751sin(1x)+(-3.00896cos(2x))+(-11.3876sin(2x))"),
        ("fourier((x-1)/(x-1)*(abs(x-1.0001)+(x-1)^2),x,2)",
         "Fourier_2 = 6.01988+(-4.9805cos(1x))+(-5.17236sin(1x))+1.22539cos(2x)+2.1737sin(2x)"),
        ("fourier((x-1)/(x-1)*(sign(x-1.0001)-(x-1)^2),x,2)",
         "Fourier_2 = -4.6032+3.46963cos(1x)+4.989sin(1x)+(-1.29369cos(2x))+(-2.44178sin(2x))"),
        ("fourier(atan(1/(x-1)),x,2)",
         "Fourier_2 = -0.0932305+(-0.500238cos(1x))+0.540986sin(1x)+(-0.407892cos(2x))+(-0.267905sin(2x))"),
        ("fourier(exp(-1/(x-1)^2),x,2)",
         "Fourier_2 = 0.545599+(-0.264407cos(1x))+(-0.388821sin(1x))+0.0651899cos(2x)+(-0.0996647sin(2x))"),
        # Compatibility only: this tuned background hides a non-integrable pole.
        ("fourier(1/(x-1)+1e10*(x-1)^2,x,2)",
         "Fourier_2 = 4.28987e+10+(-4e+10cos(1x))+(-4e+10sin(1x))+9.99997e+09cos(2x)+2e+10sin(2x)"),
        ("fourier((x-1.00299)^3*((x-1)/(x-1)),x,2)",
         "Fourier_2 = -10.9081+12.0359cos(1x)+13.7751sin(1x)+(-3.00896cos(2x))+(-11.3876sin(2x))"),
    ]
    assert run(workers["calc.series"], [e for e, _ in controls]) == [
        (True, answer) for _, answer in controls]


def test_nlsolve_never_reports_a_failed_evaluation_as_a_root(workers):
    exprs = [f"nlsolve({S51}-1,y-x,x,y)",
             "nlsolve(sqrt(x)+y-3,y-1,x,y)", "nlsolve(ln(x)-y,y,x,y)",
             "nlsolve(x^2+y^2-25,y-x,x,y)", "nlsolve(x+y-3,x-y-1,x,y)"]
    rows = run(workers["algsy"], exprs)
    assert rows[0] == (False, CAP), rows[0]
    assert rows[1:] == [
        (True, "(x, y) = (4, 1)"),
        (True, "(x, y) = (1, 0)"),
        (True, "(x, y) = (3.5355, 3.5355); (x, y) = (-3.5355, -3.5355)"),
        (True, "(x, y) = (2, 1)"),
    ], rows


def test_volumes_and_lengths_refuse_failed_samples_and_bounds(workers):
    failing = [f"disk({S51},x,0,1)", f"shell({S51},x,0,1)", f"washer({S51},x,x,0,1)",
               "disk(sqrt(x),x,-1,1)", "disk(1/x,x,-1,1)", "disk(x,x,0,ln(-1))",
               "shell(sqrt(x),x,-1,1)", "washer(sqrt(x),x,x,-1,1)",
               "arclen(sqrt(x),x,-1,1)", "surfarea(ln(x),x,0,1)"]
    rows = run(workers["calc.pol.num"], failing + ["disk(sqrt(x),x,0,4)", "arclen(x^2,x,0,1)"])
    for expr, (ok, text) in zip(failing, rows):
        assert not ok and text.endswith(": computation failed"), (expr, ok, text)
        assert "nan" not in text, (expr, text)
    assert rows[-2] == (True, "Volume (disk) = 25.1327"), rows[-2]
    assert rows[-1] == (True, "Arc length = 1.47894"), rows[-1]


def test_series_and_recurrences_refuse_instead_of_inconclusive_or_truncated(workers):
    rows = run(workers["calc.seq"], [f"sum({N51},n,1,10)", f"recseq({S51},x,0,5)",
                                     "sum(1/n^2,n,1,inf)", "recseq(2*x,x,1,5)",
                                     "recseq(ln(x),x,1,5)"])
    assert rows[0] == (False, CAP), rows[0]
    assert rows[1] == (False, CAP), rows[1]
    # ln(0) at s(2): main answered "s(1) = 0" as if the sequence ended there.
    assert rows[4] == (False, "recseq: s(2) could not be evaluated"), rows[4]
    assert rows[2][0] and first_line(rows[2][1]) == "Convergence: converges (telescoping)", rows[2]
    assert rows[3][0] and first_line(rows[3][1]) == "s(5) = 32", rows[3]


def test_limits_recover_their_own_probes(workers):
    # limits.c turns every eval error into "this method does not apply",
    # so its depth refusals (direct substitution, the 0/0 test, L'Hopital's
    # numeric fallback) are handled there and keep these answers.
    adv = run(workers["calc.lim.adv"], [f"lim({S20},x,1)", f"lim(({S55})/x,x,0)",
                                        f"lim({S13},x,pi)"])
    assert adv == [(True, "= 20"), (True, "= 55"), (True, "= 40.8407")], adv
    base = run(workers["calc.lim.base"], ["lim(" + "sin(" * 10 + "x" + ")" * 10 + "/x,x,0)"])
    assert base == [(True, "= 1")], base
    cont = run(workers["calc.lim.cont"], [
        f"continuity({S13},x,0)",
        "continuity(1/x+" + "x/(" * 20 + "x" + ")" * 20 + ",x,0)"])
    assert cont == [(True, "Continuity: could not determine")] * 2, cont
