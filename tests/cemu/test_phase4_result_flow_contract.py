from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
LAUNCHER_MAIN = ROOT / "src" / "core" / "launcher" / "main.c"
PHASES_2_4 = ROOT / "tests" / "cemu" / "test_phases_2_4.py"


def test_launcher_result_flow_has_no_blocking_pretty_print_overlay() -> None:
    text = LAUNCHER_MAIN.read_text(encoding="utf-8")

    assert 'Removed "Any key=continue" pause after showing results.' in text
    assert "static void pp_show_result(void)" not in text
    assert "pp_show_result();" not in text
    assert "ENTER or any key" not in text


def test_phase4_real_cemu_contract_requires_single_enter_result_flow() -> None:
    text = PHASES_2_4.read_text(encoding="utf-8")

    assert "dismiss graphx display" not in text
    assert "pressing any key returns to the prompt" not in text
    assert "single ENTER is enough" in text
