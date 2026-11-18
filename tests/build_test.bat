@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "C:\Users\user\src\algebrix-ce\tests"
cl /nologo /W3 /D_CRT_SECURE_NO_WARNINGS /I../src /Imock /DDESKTOP_BUILD /Fe:test_runner.exe ../src/memory.c ../src/token.c ../src/ast.c ../src/parser.c ../src/eval.c ../src/deriv.c ../src/simplify.c ../src/steps.c ../src/integrate.c ../src/limits.c ../src/series.c ../src/algebra.c ../src/ui.c ../src/matrix.c ../src/stats.c ../src/polar.c ../src/prob.c ../src/finance.c ../src/macros.c ../src/graph.c ../src/list.c ../src/complex.c ../src/stattest.c ../src/mathprint.c ../src/split/classify.c ../src/split/astwire.c mock/graphx_mock.c mock/keypadc_mock.c mock/tice_mock.c mock/fileioc_mock.c test_main.c
