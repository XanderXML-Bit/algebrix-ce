$repoRoot = Join-Path $PSScriptRoot '..\..'
$zig = Join-Path $repoRoot '.tools\zig152\zig-x86_64-windows-0.15.2\zig.exe'
Set-Location -LiteralPath (Join-Path $repoRoot 'tests')

$sources = @(
  'test_main.c',
  '..\src\memory.c', '..\src\token.c', '..\src\ast.c', '..\src\parser.c',
  '..\src\eval.c', '..\src\deriv.c', '..\src\simplify.c', '..\src\steps.c',
  '..\src\integrate.c', '..\src\limits.c', '..\src\series.c', '..\src\algebra.c',
  '..\src\stats.c', '..\src\matrix.c', '..\src\polar.c', '..\src\ui.c',
  '..\src\list.c', '..\src\complex.c', '..\src\prob.c', '..\src\finance.c',
  '..\src\stattest.c', '..\src\macros.c', '..\src\graph.c', '..\src\mathprint.c',
  '..\src\mathprint_template.c',
  '..\src\split\classify.c', '..\src\split\astwire.c', '..\src\split\ipc.c',
  '..\src\split\module_runtime.c', '..\src\split\module_forward.c', '..\src\split\processor.c',
  '..\src\split\disambig.c',
  '..\src\solver.c', '..\src\history.c',
  '..\src\dsolve.c', '..\src\vector.c', '..\src\laplace.c',
  '..\src\units.c', '..\src\userfunc.c', '..\src\bookmark.c',
  '.\mock\graphx_mock.c', '.\mock\keypadc_mock.c', '.\mock\tice_mock.c', '.\mock\fileioc_mock.c'
)

$allArgs = @(
  'cc', '-Wall', '-Wextra', '-Wno-unused-function', '-g', '-O0',
  '-I..\src', '-Imock', '-std=c99',
  '-DDESKTOP_BUILD', '-DCE_PROC_ALG=1', '-DCE_PROC_CALC=1', '-DCE_PROC_STAT=1',
  '-DCE_PROC_FIN=1', '-DCE_PROC_MATRIX=1', '-DCE_PROC_CMPLX=1', '-DCE_PROC_GRAPH=1'
) + $sources + @('-lm', '-o', 'test_runner_new.exe')

Write-Host "Building with zig cc ($($sources.Count) source files)..."
$proc = Start-Process -FilePath $zig -ArgumentList $allArgs -Wait -PassThru -NoNewWindow `
        -RedirectStandardError 'build_zig_err.txt' -RedirectStandardOutput 'build_zig_out.txt'

$out = Get-Content 'build_zig_out.txt' -ErrorAction SilentlyContinue
$err = Get-Content 'build_zig_err.txt' -ErrorAction SilentlyContinue

if ($out) { $out | ForEach-Object { Write-Host $_ } }
if ($err) { $err | ForEach-Object { Write-Host "ERR: $_" } }

if ($proc.ExitCode -ne 0) {
    Write-Host "Build FAILED with exit code $($proc.ExitCode)"
    exit 1
}

Write-Host "Build succeeded."
Write-Host "Running tests..."
$run = Start-Process -FilePath '.\test_runner_new.exe' -Wait -PassThru -NoNewWindow `
       -RedirectStandardOutput 'test_zig_out.txt' -RedirectStandardError 'test_zig_err.txt'

$tOut = Get-Content 'test_zig_out.txt' -ErrorAction SilentlyContinue
$tErr = Get-Content 'test_zig_err.txt' -ErrorAction SilentlyContinue
if ($tOut) { $tOut | Select-Object -Last 20 | ForEach-Object { Write-Host $_ } }
if ($tErr) { $tErr | ForEach-Object { Write-Host "STDERR: $_" } }

Write-Host "Test exit code: $($run.ExitCode)"
exit $run.ExitCode
