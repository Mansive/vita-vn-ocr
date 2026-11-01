# Desktop benchmark (Windows)

Times VOCR's C runtime (`../runtime`) on your own screenshots, in three builds:

- **C scalar**: the runtime as the console ships it, plain C.
- **C AVX2, lightest**: AVX2 kernels for the recognizer and the textspot8 line finder, one thread. Same text.
- **C AVX2, fastest**: the same on several threads (a small thread pool, `pool.h`), the finder in 64-frame tiles.
  Same text.

## Build

You need Windows, a CPU with AVX2 (Intel Haswell / AMD Zen or newer) and zig, a C compiler:

```
winget install zig.zig
```

or `pip install ziglang==0.16.0`. Then, in this folder:

```
powershell -ExecutionPolicy Bypass -File fetch_models.ps1
build.bat
```

`fetch_models.ps1` downloads the five model files (about 5.5 MB) from release
[v0.5.0](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.5.0) into `models\` and checks their SHA-256.
`build.bat` writes `build\ocr_page.exe` (AVX2) and `build\ocr_page_scalar.exe`, plus `parity.exe` /
`parity_scalar.exe` for the check below. `build.bat native` also builds the AVX2 code tuned for your own CPU
(`-march=native`: AVX-512 on Zen 4 / Zen 5 for the loops the compiler vectorises itself); `bench.ps1` adds its rows.
That build runs only on CPUs like the one that built it.

## Run

Put a few screenshots with dialogue in a `frames\` folder here (`.png`, `.jpg`, `.bmp`; `frames\` is git-ignored,
don't commit game frames), then:

```
powershell -ExecutionPolicy Bypass -File bench.ps1
```

| option | default | |
|---|---|---|
| `-Frames DIR` | `frames\` | the screenshots |
| `-Repeat N` | `20` | warm reads per frame and setup |
| `-Threads N` | logical CPUs, at most 8 | threads of the "fastest" rows |

First, a **parity check**: `parity.exe` of every build (1 thread and N threads) hashes every recognizer output
(logits, top-8, confidences, text over 20 random line crops per frame, per tier) and every line-finder map row and box,
and must print the same hashes as `parity_scalar.exe`. A build that differs fails the run. Then the timings: each
frame is read greedily, without the LM or the cascade; a cell is the mean over frames of the median read. **per line**
is the recognizer's time per text line, **line finder** the textspot8 pass, **page** the whole frame, **peak RAM** the
process's peak working set. Two frames take about 10 s.

Ryzen 7 5800X3D, two 960x544 / 1200x680 frames:

| tier | build | per line ms | line finder ms | page ms | peak RAM |
|---|---|---|---|---|---|
| S | C scalar | 8.73 | 20.4 | 37.9 | 7.4 MiB |
| S | C AVX2, lightest (1 thread) | 2.31 | 4.8 | 9.5 | 7.4 MiB |
| S | C AVX2, fastest (8 threads, tile 64) | 1.64 | 2.0 | 5.3 | 7.4 MiB |
| N | C scalar | 9.13 | 20.5 | 38.8 | 6.9 MiB |
| N | C AVX2, lightest (1 thread) | 2.20 | 4.8 | 9.3 | 6.9 MiB |
| N | C AVX2, fastest (8 threads, tile 64) | 2.10 | 2.0 | 6.2 | 8.4 MiB |
| L | C scalar | 23.51 | 20.5 | 67.5 | 7.7 MiB |
| L | C AVX2, lightest (1 thread) | 5.01 | 4.9 | 14.9 | 7.7 MiB |
| L | C AVX2, fastest (8 threads, tile 64) | 3.17 | 1.9 | 8.1 | 10.5 MiB |

One frame by hand, with the LM and the cascade (the full read), as JSON:

```
build\ocr_page.exe frames\shot.ppm --models models --lm models\vocr_lm6_o3_1000k.vlm --repeat 20 --threads 8
```

(`ocr_page` reads binary `.ppm` / `.pgm`; `bench.ps1` leaves converted copies in `build\frames\`.)

## Next: AVX-512 kernels for Zen 4 / Zen 5 (open)

Written on a Ryzen 7 5800X3D (Zen 3, no AVX-512), so nothing here uses AVX-512 by hand yet. On a Zen 5 CPU
(e.g. Ryzen 7 9800X3D: full 512-bit units, AVX-512 VNNI, AVX-VNNI) these are the places to try, measured with
`bench.ps1` on that machine:

- **Integer products** (W8I recognizer, `runtime/vocr.c` around `_mm256_madd_epi16` + `_mm256_add_epi32`, ~line 1620;
  the line finder's `MADD` macro in `runtime/vocr_textspot6.c`, ~line 1381): `vpdpwssd` (`_mm512_dpwssd_epi32`, or the
  256-bit `_mm256_dpwssd_avx_epi32`) does the multiply-add-accumulate in one instruction with the same int32 result,
  and 512 bits cover twice the outputs per instruction.
- **Float front end** (`runtime/vocr.c`, `_mm256_mul_ps` + `_mm256_add_ps` across outputs, ~lines 941 and 1063):
  widen to 16 lanes. Keep the multiply and the add separate (no FMA) and each output's order of additions unchanged,
  or the result changes.

Rules: output stays bit-identical to the scalar build (`bench.ps1`'s parity check must say `same` for every build);
keep `-ffp-contract=off`; put AVX-512 behind its own guard (e.g. `#ifdef __AVX512F__`) and a `build.bat avx512` option,
so the AVX2 and scalar builds don't change; the console runtime stays plain C (`VOCR_NO_AVX2` builds none of this).
Report page / per-line / finder times before and after, from `bench.ps1` with the same frames.

## Licence

The code is GPL-3.0-or-later, like the rest of this repository. The model weights are for **non-commercial use only**;
see the release notes and attribution files of v0.5.0.
