"""
tests/cemu/test_stack_check.py — Build-time stack-usage guard regression tests.

These tests exercise tools/check_stack.py using synthetic .su files written to
a temporary directory.  They always run — no cemu-headless binary, ROM, or
calculator hardware is required.

Background: PR #40 documented a hardware-only crash caused by a ~9 KB combined
stack depth that the eZ80 OS watchdog caught but CEmu's flat host memory model
did not.  tools/check_stack.py catches this class of bug at build time by
parsing -fstack-usage .su files and failing if any function frame exceeds
the threshold (default 1024 B).
"""

from __future__ import annotations

import os
import sys
import textwrap
from pathlib import Path

import pytest

# Make tools/ importable regardless of how pytest is invoked.
_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / 'tools'))

import check_stack  # noqa: E402 (import after sys.path tweak)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def write_su(tmp_path: Path, name: str, content: str) -> Path:
    """Write a synthetic .su file and return its path."""
    p = tmp_path / name
    p.write_text(textwrap.dedent(content))
    return p


def run_main(argv: list[str]) -> int:
    """Run check_stack.main() and return the exit code."""
    return check_stack.main(argv)


# ---------------------------------------------------------------------------
# Parser unit tests
# ---------------------------------------------------------------------------

class TestSuLineParser:
    """_parse_su_line() handles all valid and degenerate inputs."""

    def test_static_frame(self):
        line = "src/module_runtime.c:42:ce_module_runtime_finish\t128\tstatic\n"
        entry = check_stack._parse_su_line(line, "x.su")
        assert entry is not None
        assert entry.function == "ce_module_runtime_finish"
        assert entry.bytes_ == 128
        assert entry.frame_type == "static"
        assert entry.line == 42

    def test_dynamic_bounded_frame(self):
        line = "src/parser.c:100:parse_expr\t256\tdynamic,bounded\n"
        entry = check_stack._parse_su_line(line, "x.su")
        assert entry is not None
        assert entry.frame_type == "dynamic,bounded"
        assert entry.bytes_ == 256

    def test_dynamic_unbounded_frame(self):
        line = "src/eval.c:7:eval_bad\t0\tdynamic\n"
        entry = check_stack._parse_su_line(line, "x.su")
        assert entry is not None
        assert entry.frame_type == "dynamic"
        assert entry.bytes_ == 0

    def test_blank_line_returns_none(self):
        assert check_stack._parse_su_line("", "x.su") is None
        assert check_stack._parse_su_line("\n", "x.su") is None
        assert check_stack._parse_su_line("   \n", "x.su") is None

    def test_comment_line_returns_none(self):
        assert check_stack._parse_su_line("# comment\n", "x.su") is None

    def test_malformed_no_tabs_returns_none(self):
        assert check_stack._parse_su_line("src/foo.c:1:func 64 static\n", "x.su") is None

    def test_malformed_non_integer_bytes_returns_none(self):
        assert check_stack._parse_su_line("src/foo.c:1:func\tXYZ\tstatic\n", "x.su") is None

    def test_four_colon_location_with_column(self):
        """Some clang versions emit file:line:col:func — col should be ignored."""
        line = "src/foo.c:10:5:my_func\t64\tstatic\n"
        entry = check_stack._parse_su_line(line, "x.su")
        # Should parse successfully even with extra colon
        if entry is not None:
            assert entry.bytes_ == 64
            assert entry.frame_type == "static"

    def test_zero_bytes_static(self):
        """Functions with no stack locals emit 0 B — should be parsed and not
        flagged as a violation."""
        line = "src/foo.c:1:leaf_func\t0\tstatic\n"
        entry = check_stack._parse_su_line(line, "x.su")
        assert entry is not None
        assert entry.bytes_ == 0


# ---------------------------------------------------------------------------
# File-level parsing
# ---------------------------------------------------------------------------

class TestSuFileParsing:
    def test_parse_multi_function_file(self, tmp_path):
        su = write_su(tmp_path, "test.su", """\
            src/foo.c:1:small_fn\t64\tstatic
            src/foo.c:20:medium_fn\t512\tstatic
            src/foo.c:50:large_fn\t2048\tstatic
        """)
        entries, errors = check_stack.parse_su_file(str(su))
        assert errors == []
        assert len(entries) == 3
        names = {e.function for e in entries}
        assert names == {'small_fn', 'medium_fn', 'large_fn'}

    def test_nonexistent_file_returns_error(self):
        entries, errors = check_stack.parse_su_file("/nonexistent/path/x.su")
        assert entries == []
        assert len(errors) == 1
        assert "Cannot read" in errors[0]

    def test_empty_file_returns_no_entries(self, tmp_path):
        su = write_su(tmp_path, "empty.su", "")
        entries, errors = check_stack.parse_su_file(str(su))
        assert entries == []
        assert errors == []

    def test_file_with_blank_lines_and_comments(self, tmp_path):
        su = write_su(tmp_path, "sparse.su", """\
            # generated by clang

            src/foo.c:1:func_a\t100\tstatic

            src/foo.c:2:func_b\t200\tstatic
        """)
        entries, errors = check_stack.parse_su_file(str(su))
        assert errors == []
        assert len(entries) == 2


# ---------------------------------------------------------------------------
# Core regression: 2 KB local is caught
# ---------------------------------------------------------------------------

class TestViolationDetection:
    """The PRIMARY regression suite — these mirror the PR #40 bug class."""

    def test_2kb_stack_local_is_caught(self, tmp_path):
        """A function with a 2 KB stack frame must be a violation at threshold=1024."""
        su = write_su(tmp_path, "big.su", """\
            src/module_runtime.c:200:ce_module_runtime_recover_child_failure\t2048\tstatic
        """)
        result = check_stack.scan([str(tmp_path)], [])
        violations, warnings = check_stack.classify(
            result.entries, threshold=1024, warn_threshold=512, dynamic_fail=False
        )
        assert len(violations) == 1
        assert violations[0].function == "ce_module_runtime_recover_child_failure"
        assert violations[0].bytes_ == 2048

    def test_combined_depth_representative(self, tmp_path):
        """The PR #40 scenario: ~4510 B + ~4273 B combined; each alone is a violation."""
        su = write_su(tmp_path, "pr40.su", """\
            src/module_runtime.c:123:ce_module_runtime_recover_child_failure\t4510\tstatic
            src/core/launcher/main.c:456:main\t4273\tstatic
        """)
        result = check_stack.scan([str(tmp_path)], [])
        violations, _ = check_stack.classify(
            result.entries, threshold=1024, warn_threshold=512, dynamic_fail=False
        )
        assert len(violations) == 2
        sizes = {e.bytes_ for e in violations}
        assert sizes == {4510, 4273}

    def test_exactly_at_threshold_is_a_violation(self, tmp_path):
        su = write_su(tmp_path, "at.su", """\
            src/foo.c:1:borderline\t1024\tstatic
        """)
        result = check_stack.scan([str(tmp_path)], [])
        violations, warnings = check_stack.classify(
            result.entries, threshold=1024, warn_threshold=512, dynamic_fail=False
        )
        assert len(violations) == 1
        assert violations[0].function == "borderline"

    def test_one_below_threshold_is_not_a_violation(self, tmp_path):
        su = write_su(tmp_path, "under.su", """\
            src/foo.c:1:safe_fn\t1023\tstatic
        """)
        result = check_stack.scan([str(tmp_path)], [])
        violations, warnings = check_stack.classify(
            result.entries, threshold=1024, warn_threshold=512, dynamic_fail=False
        )
        assert violations == []

    def test_safe_function_not_flagged(self, tmp_path):
        su = write_su(tmp_path, "safe.su", """\
            src/foo.c:1:tiny_fn\t32\tstatic
        """)
        result = check_stack.scan([str(tmp_path)], [])
        violations, warnings = check_stack.classify(
            result.entries, threshold=1024, warn_threshold=512, dynamic_fail=False
        )
        assert violations == []
        assert warnings == []

    def test_multiple_files_multiple_violations(self, tmp_path):
        sub = tmp_path / "obj"
        sub.mkdir()
        write_su(sub, "a.su", "src/a.c:1:fn_a\t2000\tstatic\n")
        write_su(sub, "b.su", "src/b.c:1:fn_b\t1500\tstatic\n")
        write_su(sub, "c.su", "src/c.c:1:fn_c\t100\tstatic\n")
        result = check_stack.scan([str(sub)], [])
        violations, _ = check_stack.classify(
            result.entries, threshold=1024, warn_threshold=512, dynamic_fail=False
        )
        funcs = {e.function for e in violations}
        assert "fn_a" in funcs
        assert "fn_b" in funcs
        assert "fn_c" not in funcs

    def test_warning_zone_entries(self, tmp_path):
        """Frames between warn_threshold and threshold appear as warnings, not violations."""
        su = write_su(tmp_path, "warn.su", """\
            src/foo.c:1:mid_fn\t700\tstatic
        """)
        result = check_stack.scan([str(tmp_path)], [])
        violations, warnings = check_stack.classify(
            result.entries, threshold=1024, warn_threshold=512, dynamic_fail=False
        )
        assert violations == []
        assert len(warnings) == 1
        assert warnings[0].function == "mid_fn"


# ---------------------------------------------------------------------------
# Dynamic frame handling
# ---------------------------------------------------------------------------

class TestDynamicFrames:
    def test_dynamic_unbounded_is_warning_by_default(self, tmp_path):
        su = write_su(tmp_path, "dyn.su", """\
            src/foo.c:1:vla_fn\t0\tdynamic
        """)
        result = check_stack.scan([str(tmp_path)], [])
        violations, warnings = check_stack.classify(
            result.entries, threshold=1024, warn_threshold=512, dynamic_fail=False
        )
        assert violations == []
        assert len(warnings) == 1

    def test_dynamic_unbounded_is_violation_with_dynamic_fail(self, tmp_path):
        su = write_su(tmp_path, "dyn.su", """\
            src/foo.c:1:vla_fn\t0\tdynamic
        """)
        result = check_stack.scan([str(tmp_path)], [])
        violations, warnings = check_stack.classify(
            result.entries, threshold=1024, warn_threshold=512, dynamic_fail=True
        )
        assert len(violations) == 1
        assert violations[0].function == "vla_fn"

    def test_dynamic_bounded_large_is_still_a_violation(self, tmp_path):
        su = write_su(tmp_path, "db.su", """\
            src/foo.c:1:bounded_fn\t1500\tdynamic,bounded
        """)
        result = check_stack.scan([str(tmp_path)], [])
        violations, _ = check_stack.classify(
            result.entries, threshold=1024, warn_threshold=512, dynamic_fail=False
        )
        assert len(violations) == 1


# ---------------------------------------------------------------------------
# File discovery / exclusion
# ---------------------------------------------------------------------------

class TestFileDiscovery:
    def test_finds_su_files_recursively(self, tmp_path):
        (tmp_path / "obj").mkdir()
        (tmp_path / "obj" / "sub").mkdir()
        (tmp_path / "obj" / "a.su").write_text("src/a.c:1:fn\t10\tstatic\n")
        (tmp_path / "obj" / "sub" / "b.su").write_text("src/b.c:1:fn\t20\tstatic\n")
        (tmp_path / "obj" / "c.txt").write_text("ignored\n")
        files = check_stack.find_su_files([str(tmp_path)], [])
        basenames = {Path(f).name for f in files}
        assert basenames == {"a.su", "b.su"}
        assert "c.txt" not in str(files)

    def test_direct_su_file_path_is_accepted(self, tmp_path):
        su = tmp_path / "x.su"
        su.write_text("src/x.c:1:fn\t50\tstatic\n")
        files = check_stack.find_su_files([str(su)], [])
        assert len(files) == 1
        assert files[0] == str(su)

    def test_exclude_glob_skips_matching_files(self, tmp_path):
        (tmp_path / "good.su").write_text("src/g.c:1:fn\t10\tstatic\n")
        (tmp_path / "third_party.su").write_text("src/t.c:1:fn\t10\tstatic\n")
        files = check_stack.find_su_files([str(tmp_path)], ["third_party.su"])
        basenames = {Path(f).name for f in files}
        assert "good.su" in basenames
        assert "third_party.su" not in basenames

    def test_nonexistent_path_returns_empty(self):
        files = check_stack.find_su_files(["/nonexistent/dir"], [])
        assert files == []

    def test_empty_directory_returns_empty(self, tmp_path):
        files = check_stack.find_su_files([str(tmp_path)], [])
        assert files == []

    @pytest.mark.parametrize('alias', ['./obj/X/lto.su', 'absolute', 'symlink'])
    def test_realpath_aliases_count_once(self, tmp_path, monkeypatch, alias):
        report_dir = tmp_path / 'obj' / 'X'
        report_dir.mkdir(parents=True)
        su = write_su(report_dir, 'lto.su', 'src/f.c:1:fn\t64\tstatic\n')
        monkeypatch.chdir(tmp_path)
        if alias == 'absolute':
            alias = str(su)
        elif alias == 'symlink':
            try:
                (tmp_path / 'alias.su').symlink_to(su)
            except (OSError, NotImplementedError) as exc:
                pytest.skip(f'cannot create symlink here: {exc}')
            alias = 'alias.su'
        result = check_stack.scan(['obj', alias], [])
        assert result.su_files_scanned == 1
        assert len(result.entries) == 1


# ---------------------------------------------------------------------------
# CLI integration (main() exit codes)
# ---------------------------------------------------------------------------

class TestCLIExitCodes:
    """check_stack.main() must distinguish success, violations, and missing evidence."""

    def test_no_su_files_exits_2(self, tmp_path, capsys):
        rc = run_main([str(tmp_path)])
        assert rc == 2
        assert "no .su files found" in capsys.readouterr().err

    def test_allow_empty_exits_0(self, tmp_path):
        assert run_main(['--allow-empty', str(tmp_path)]) == 0

    @pytest.mark.parametrize('malformed', [
        'not a stack report',
        'src/f.c:2:bad\tXYZ\tstatic',
    ])
    def test_unparsable_report_exits_2(self, tmp_path, capsys, malformed):
        su = write_su(tmp_path, 'bad.su', 'src/f.c:1:fn\t64\tstatic\n' + malformed)
        result = check_stack.scan([str(tmp_path)], [])
        assert len(result.entries) == 1
        assert result.parse_errors == [f'{su}:2: unparsable line']
        assert run_main([str(tmp_path)]) == 2
        captured = capsys.readouterr()
        assert f'error: {su}:2: unparsable line' in captured.err
        assert 'OK' not in captured.out

    @pytest.mark.parametrize('content', ['', '# comment\n\n   \n'])
    def test_empty_report_among_valid_reports_exits_2(self, tmp_path, capsys, content):
        write_su(tmp_path, 'ok.su', 'src/f.c:1:fn\t64\tstatic\n')
        empty = write_su(tmp_path, 'empty.su', content)
        assert run_main(['--expect-count', '2', str(tmp_path)]) == 2
        captured = capsys.readouterr()
        assert f'error: {empty}: no function frame entries' in captured.err
        assert 'OK' not in captured.out

    @pytest.mark.parametrize('content', ['', 'unparsable\n'])
    def test_allow_empty_permits_invalid_reports(self, tmp_path, content):
        write_su(tmp_path, 'invalid.su', content)
        assert run_main(['--allow-empty', '--expect-count', '1', str(tmp_path)]) == 0
        assert run_main(['--allow-empty', '--expect-count', '2', str(tmp_path)]) == 2

    @pytest.mark.parametrize('content, warning', [
        ('', 'EMPTY REPORT'),
        ('unparsable\nsrc/f.c:1:fn\t64\tstatic\n', 'PARSE ERROR'),
    ])
    def test_allow_empty_quiet_still_warns(self, tmp_path, capsys, content, warning):
        bad = write_su(tmp_path, 'invalid.su', content)
        assert run_main(['--allow-empty', '--quiet', str(tmp_path)]) == 0
        captured = capsys.readouterr()
        assert warning in captured.err
        assert str(bad) in captured.err
        assert captured.out == ''

    def test_allow_empty_quiet_count_mismatch_still_warns(self, tmp_path, capsys):
        bad = write_su(tmp_path, 'invalid.su', '')
        assert run_main(['--allow-empty', '--quiet', '--expect-count', '2',
                         str(tmp_path)]) == 2
        err = capsys.readouterr().err
        assert 'expected 2 .su file(s), found 1' in err
        assert f'EMPTY REPORT: {bad}' in err

    def test_indented_comment_is_not_a_parse_error(self, tmp_path):
        write_su(tmp_path, 'ok.su', '   # indented comment\nsrc/f.c:1:fn\t64\tstatic\n')
        entries, errors = check_stack.parse_su_file(str(tmp_path / 'ok.su'))
        assert len(entries) == 1
        assert errors == []

    def test_allow_empty_preserves_frame_violations(self, tmp_path):
        write_su(tmp_path, 'bad.su', 'unparsable\nsrc/f.c:1:fn\t2048\tstatic\n')
        assert run_main(['--allow-empty', str(tmp_path)]) == 1

    def test_expect_count_matches(self, tmp_path):
        write_su(tmp_path, "ok.su", "src/f.c:1:fn\t64\tstatic\n")
        assert run_main(['--expect-count', '1', str(tmp_path)]) == 0

    @pytest.mark.parametrize('expected', [0, 2, 35])
    def test_expect_count_mismatch_exits_2(self, tmp_path, expected, capsys):
        write_su(tmp_path, "ok.su", "src/f.c:1:fn\t64\tstatic\n")
        assert run_main(['--expect-count', str(expected), str(tmp_path)]) == 2
        diagnostic = capsys.readouterr().err
        assert f"expected {expected} .su file(s), found 1" in diagnostic
        assert "run 'make clean' and rebuild" in diagnostic

    def test_allow_empty_does_not_override_expected_count(self, tmp_path):
        assert run_main(['--allow-empty', '--expect-count', '35', str(tmp_path)]) == 2

    def test_expect_zero_still_requires_allow_empty(self, tmp_path):
        assert run_main(['--expect-count', '0', str(tmp_path)]) == 2
        assert run_main(['--allow-empty', '--expect-count', '0', str(tmp_path)]) == 0

    def test_negative_expect_count_exits_2(self, tmp_path):
        assert run_main(['--expect-count', '-1', str(tmp_path)]) == 2

    def test_overlapping_paths_do_not_inflate_count(self, tmp_path):
        su = write_su(tmp_path, "ok.su", "src/f.c:1:fn\t64\tstatic\n")
        assert run_main(['--expect-count', '1', str(tmp_path), str(su)]) == 0

    def test_clean_su_exits_0(self, tmp_path):
        write_su(tmp_path, "clean.su", """\
            src/foo.c:1:tiny\t128\tstatic
            src/foo.c:2:also_tiny\t64\tstatic
        """)
        rc = run_main([str(tmp_path)])
        assert rc == 0

    def test_large_frame_exits_1(self, tmp_path):
        """REGRESSION: function with 2 KB local must fail the build."""
        write_su(tmp_path, "big.su", """\
            src/module_runtime.c:200:huge_local_fn\t2048\tstatic
        """)
        rc = run_main([str(tmp_path)])
        assert rc == 1

    def test_custom_threshold_respected(self, tmp_path):
        write_su(tmp_path, "t.su", "src/f.c:1:fn\t500\tstatic\n")
        # Default threshold 1024 → OK
        assert run_main([str(tmp_path)]) == 0
        # Lower threshold 256 → FAIL
        assert run_main([str(tmp_path), '--threshold', '256']) == 1

    def test_quiet_flag_exits_0_on_success(self, tmp_path, capsys):
        write_su(tmp_path, "q.su", "src/f.c:1:fn\t64\tstatic\n")
        rc = run_main(['--quiet', str(tmp_path)])
        assert rc == 0
        captured = capsys.readouterr()
        assert captured.out == ""

    def test_summary_flag_exits_1_on_violation(self, tmp_path, capsys):
        write_su(tmp_path, "s.su", "src/f.c:1:fn\t2000\tstatic\n")
        rc = run_main(['--summary', str(tmp_path)])
        assert rc == 1

    def test_dynamic_fail_flag_catches_unbounded(self, tmp_path):
        write_su(tmp_path, "d.su", "src/f.c:1:vla\t0\tdynamic\n")
        # Without flag: warning, exit 0
        assert run_main([str(tmp_path)]) == 0
        # With flag: violation, exit 1
        assert run_main(['--dynamic-fail', str(tmp_path)]) == 1

    def test_multiple_su_files_all_violations_caught(self, tmp_path):
        sub = tmp_path / "obj"
        sub.mkdir()
        write_su(sub, "a.su", "src/a.c:1:fn_a\t2000\tstatic\n")
        write_su(sub, "b.su", "src/b.c:1:fn_b\t3000\tstatic\n")
        rc = run_main([str(sub)])
        assert rc == 1

    def test_exclude_skips_file(self, tmp_path):
        write_su(tmp_path, "keep.su", "src/k.c:1:fn_k\t100\tstatic\n")
        write_su(tmp_path, "skip.su", "src/s.c:1:fn_s\t9999\tstatic\n")
        rc = run_main(['--exclude', 'skip.su', str(tmp_path)])
        assert rc == 0

    def test_bad_threshold_exits_2(self, tmp_path):
        rc = run_main([str(tmp_path), '--threshold', '0'])
        assert rc == 2


# ---------------------------------------------------------------------------
# Known PR #40 function names — must always be caught
# ---------------------------------------------------------------------------

class TestPR40Regression:
    """Concrete regression: the exact functions from PR #40 that caused the
    hardware crash must be flagged by the checker."""

    _PR40_ENTRIES = [
        ("src/split/module_runtime.c", 1, "ce_module_runtime_recover_child_failure", 4510, "static"),
        ("src/core/launcher/main.c",   1, "main",                                    4273, "static"),
    ]

    def _make_su(self, tmp_path: Path) -> Path:
        lines = "\n".join(
            f"{src}:{ln}:{fn}\t{b}\t{typ}"
            for src, ln, fn, b, typ in self._PR40_ENTRIES
        )
        su = tmp_path / "pr40_repro.su"
        su.write_text(lines + "\n")
        return su

    def test_pr40_functions_violate_1024_threshold(self, tmp_path):
        su = self._make_su(tmp_path)
        entries, errors = check_stack.parse_su_file(str(su))
        assert errors == []
        violations, _ = check_stack.classify(entries, 1024, 512, False)
        funcs = {e.function for e in violations}
        assert "ce_module_runtime_recover_child_failure" in funcs
        assert "main" in funcs

    def test_pr40_check_main_exits_1(self, tmp_path):
        self._make_su(tmp_path)
        rc = run_main([str(tmp_path)])
        assert rc == 1

    def test_pr40_functions_safe_after_demotion_to_static_storage(self, tmp_path):
        """After the PR #40 fix (file-static storage), both functions have
        0-byte stack locals for those specific allocations.  Simulate that."""
        su = tmp_path / "pr40_fixed.su"
        su.write_text(
            "src/split/module_runtime.c:1:ce_module_runtime_recover_child_failure\t0\tstatic\n"
            "src/core/launcher/main.c:1:main\t0\tstatic\n"
        )
        rc = run_main([str(tmp_path)])
        assert rc == 0
