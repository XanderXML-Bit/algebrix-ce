from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "src" / "core" / "launcher" / "main.c"


def _process_key_body(text: str) -> str:
    start = text.index("static int8_t process_key")
    end = text.index("static bool ce_input_scan", start)
    return text[start:end]


def _case_body(text: str, key: str, *, occurrence: int = 1) -> str:
    matches = list(
        re.finditer(
            rf"case {re.escape(key)}:(.*?)(?=\n\s*case sk_|\n\s*default:|\n\s*}})",
            _process_key_body(text),
            re.S,
        )
    )
    assert len(matches) >= occurrence, f"missing process_key case {occurrence} for {key}"
    return matches[occurrence - 1].group(1)


def test_home_up_down_do_not_recall_history_into_input() -> None:
    text = LAUNCHER.read_text(encoding="utf-8")

    up_case = _case_body(text, "sk_Up")
    down_case = _case_body(text, "sk_Down")

    assert "input_load_history" not in up_case
    assert "input_load_history" not in down_case
    assert "s_hist_saved" not in up_case
    assert "s_hist_saved" not in down_case
    assert "TI-OS home screen keeps UP/DOWN from mutating the edit line" in text


def test_second_clear_uses_full_home_history_clear() -> None:
    text = LAUNCHER.read_text(encoding="utf-8")
    clear_2nd_case = _case_body(text, "sk_Clear", occurrence=1)

    assert "home_clear_all();" in clear_2nd_case
    assert "static void home_clear_all(void)" in text
    assert "g_shared.hist_count = 0" in text
    assert "g_shared.hist_write = 0" in text
    assert "memset(g_shared.history" in text
    assert "memset(g_shared.history_results" in text
    assert "g_shared.last_result[0] = '\\0'" in text
    assert "ce_state_save(&g_shared)" in text
