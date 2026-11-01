@echo off
rem Builds the benchmark into bench\build\ with zig cc (a C compiler that needs no install of its own):
rem
rem   build.bat           build\ocr_page.exe, parity.exe                AVX2 kernels (Intel Haswell+, AMD Zen / Excavator+)
rem                       build\ocr_page_scalar.exe, parity_scalar.exe  plain C, the console runtime as it ships
rem   build.bat native    also build\ocr_page_native.exe, parity_native.exe: the AVX2 build tuned for this machine's
rem                       CPU (-march=native); runs only on CPUs like it
rem
rem Compiler: %CC% if set (any GCC/Clang-style one, e.g. set CC=clang), else zig from PATH, else "python -m ziglang"
rem (pip install ziglang==0.16.0), else install zig: winget install zig.zig
setlocal
cd /d "%~dp0"

if not defined CC where zig >nul 2>nul && set CC=zig cc
if not defined CC (
    python -c "import ziglang" >nul 2>nul && set CC=python -m ziglang cc
)
if not defined CC (
    echo zig not found. Install it with one of:
    echo   winget install zig.zig
    echo   pip install ziglang==0.16.0
    exit /b 1
)

set RT=..\runtime
set FLAGS=-std=c99 -ffp-contract=off -O2 -I%RT% -I.
set SRC=%RT%\vocr.c %RT%\vocr_lm.c %RT%\vocr_post.c %RT%\vocr_lines.c %RT%\vocr_lines_rows.c %RT%\vocr_textspot6.c
if not exist build mkdir build

echo building the AVX2 build with %CC%
%CC% %FLAGS% -mavx2 -o build\ocr_page.exe ocr_page.c %SRC% -lpsapi || exit /b 1
%CC% %FLAGS% -mavx2 -o build\parity.exe parity.c %SRC% || exit /b 1
echo building the scalar build
%CC% %FLAGS% -DVOCR_NO_AVX2 -o build\ocr_page_scalar.exe ocr_page.c %SRC% -lpsapi || exit /b 1
%CC% %FLAGS% -DVOCR_NO_AVX2 -o build\parity_scalar.exe parity.c %SRC% || exit /b 1
if /i "%~1"=="native" (
    echo building the native build
    %CC% %FLAGS% -march=native -o build\ocr_page_native.exe ocr_page.c %SRC% -lpsapi || exit /b 1
    %CC% %FLAGS% -march=native -o build\parity_native.exe parity.c %SRC% || exit /b 1
)

if not exist models\VOCR6.2_N_w8g2.vocr echo models missing: run  powershell -ExecutionPolicy Bypass -File fetch_models.ps1
echo done
