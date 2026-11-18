"""Host checks for the disposable backstop build's effective depth flag."""
import pytest

from test_realrom_eval_backstop import _raise_cebase_diagnostic_cap


def test_diagnostic_cap_changes_only_the_exact_expected_definition(tmp_path):
    path = tmp_path / "Makefile.base"
    before = "CFLAGS += -DKEEP=16 -DCE_EVAL_MAX_DEPTH=16 \\\n -DOTHER=8\n"
    path.write_text(before)
    _raise_cebase_diagnostic_cap(path)
    assert path.read_text() == before.replace("-DCE_EVAL_MAX_DEPTH=16",
                                              "-DCE_EVAL_MAX_DEPTH=200")


@pytest.mark.parametrize("flags", [
    "-DOTHER=16",
    "-DCE_EVAL_MAX_DEPTH=160",
    "-DCE_EVAL_MAX_DEPTH=16u",
    "-DCE_EVAL_MAX_DEPTH",
    "-DCE_EVAL_MAX_DEPTH=16 -DCE_EVAL_MAX_DEPTH=16",
    "-DCE_EVAL_MAX_DEPTH=16 -DCE_EVAL_MAX_DEPTH=200",
    "-DCE_EVAL_MAX_DEPTH=16 -D CE_EVAL_MAX_DEPTH=32",
    "-D CE_EVAL_MAX_DEPTH=16",
])
def test_diagnostic_cap_rejects_unexpected_or_duplicate_definitions(tmp_path, flags):
    path = tmp_path / "Makefile.base"
    before = "CFLAGS += " + flags + "\n"
    path.write_text(before)
    with pytest.raises(AssertionError):
        _raise_cebase_diagnostic_cap(path)
    assert path.read_text() == before
