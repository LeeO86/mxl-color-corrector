# Performance

## 1.2.0: AVX-512 levels (`CC_SIMD`) and cheaper clip statistics

Measured 2026-10-13 on the lab's 2× Xeon Gold 6136 (Skylake-SP, 2 FMA units, TSC 3.0 GHz), GCC 13.3 `-O3`, no `-march`. Lab scripts and raw numbers: `~/mxl-lab/simd/g3/` (`G3-COMMANDS.txt`, `results-20261013.txt`). Clock = 3.0 GHz × cycles / ref-cycles; licence = share of cycles in `core_power.lvl{0,1,2}_turbo_license`.

### Clip statistics

The engine always counts clipped samples (`clipped_ratio`). In 1.1.1 this cost the AVX2 path more than a third of its time: one thread, 1080p, a new grain per call, `mxl-cc-bench run avx2 matrix camera 1080 8 off|on`:

| Path | Statistics off | Statistics on |
| --- | --- | --- |
| 1.1.1 AVX2 | 1.60 ms | 2.50 ms |
| 1.1.1 scalar | 20.0 ms | 26.0 ms |
| now `avx2` | 1.50 ms | 1.92 ms |
| now `scalar` | 21.9 ms | 20.9 ms |

The odd samples' chroma rows (needed only for the count) were computed again in full, and every register was counted with a movemask and a popcount. Now the rows share the chroma pair's part, the odd chroma costs one multiply per row, AVX2 counts unchanged lanes in a vector per line and the AVX-512 levels popcount mask registers. The scalar path wrote both counters through `stats` for every pixel (the compiler must assume it aliases the output); it counts per line now. The inside test of the RGB gamut clip uses 16-bit multiplies (exact for every lane it keeps): 1.1.1 6.0 → 3.5 ms on the camera picture.

### One thread (micro)

`micro-table.sh`: statistics on, medians of 3 runs, ms per frame (ns per pixel). Camera = gradients and noise in the legal range (most pixels skip the exact gamut test); bench = `mxl-cc-bench`'s ramp on Cb 512, Cr 700 (most take it). Controls gain 110 %, saturation 90 %, brightness 2 %. 8 frames = a new 5.5 MB grain per call (more than the L3 cache), as in production.

| Format | Path | Picture | `scalar` | 1.1.1 AVX2 | `avx2` | `avx512vl` | `avx512` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1080p, 8 frames | matrix | camera | 20.9 (10.1) | 2.50 (1.20) | 1.92 (0.93) | 1.57 (0.76) | 1.35 (0.65) |
| 1080p, 8 frames | matrix | bench | 21.0 | 2.48 | 1.89 | 1.56 | 1.34 |
| 1080p, 8 frames | RGB clip | camera | 30.8 | 6.0 | 3.51 (1.69) | 3.26 (1.57) | 2.51 (1.21) |
| 1080p, 8 frames | RGB clip | bench | 116 | 21.7 | 17.5 (8.46) | 13.8 (6.67) | 9.42 (4.54) |
| 1080p, 1 frame (warm) | matrix | camera | — | — | 1.77 | 1.47 | 1.12 |
| 1080p, 1 frame (warm) | RGB clip | camera | — | — | 3.35 | 3.14 | 2.39 |
| 2160p, 8 frames | matrix | camera | 82.2 | — | 7.55 | 6.23 | 5.35 |
| 2160p, 8 frames | RGB clip | camera | 123 | — | 13.9 | 12.9 | 9.85 |
| 2160p, 8 frames | RGB clip | bench | 467 | — | 70.4 | 55.4 | 38.1 |
| Clock, licence | | | 3.6 GHz, 0 | 3.3 GHz, 1 | 3.3 GHz, 1 | 3.3 GHz, 1 | 2.7 GHz, 2 |

Against `avx2`: `avx512vl` 1.22× (matrix), 1.08× (RGB clip, camera), 1.27× (RGB clip, bench); `avx512` 1.43×, 1.40×, 1.86×, although it runs in the lowest frequency licence (2.7 GHz).

The same table (1080p, 8 frames) on GitHub-hosted runners, natively (`mxl-cc-bench table`, one run each in a shared VM):

| Runner CPU | Matrix `avx2` / `avx512vl` / `avx512` | RGB clip camera | RGB clip bench |
| --- | --- | --- | --- |
| Xeon Platinum 8573C (Emerald Rapids) | 1.53 / 1.34 / 0.98 | 3.09 / 2.80 / 2.01 | 13.0 / 10.5 / 7.04 |
| EPYC 9V45 (Zen 5) | 0.69 / 0.60 / 0.32 | 1.64 / 1.54 / 0.87 | 7.5 / 7.0 / 3.9 |
| EPYC 9V74 (Zen 4; the VM hides AVX-512) | 1.07 / – / – | 2.45 | 12.3 |
| EPYC 7763 (Zen 3, no AVX-512) | 1.02 / – / – | 2.30 | 11.5 |

### Running corrector (system)

`g3-cc.sh`: the branch image, 2 channels of 1080p50 test-player bars with burn-in (gain 110 %, saturation 120 %, pedestal 1 %), cpuset of 4 idle cores, 30 s, process CPU from the cgroup's `cpu.stat`, 2 rounds (1.1.1 one). Every run: 50.1 grains/s per channel, 0 late.

| RGB gamut clip | 1.1.1 | `avx2` | `avx512vl` | `avx512` | `scalar` |
| --- | --- | --- | --- | --- | --- |
| off, cores per channel | 0.199 | 0.166 (0.168, 0.164) | 0.155 (−7 %) | 0.135 (−19 %) | 1.00, 43 grains/s, late grains |
| on, cores per channel | 0.841 | 0.725 (0.720, 0.730) | 0.583 (−20 %) | 0.435 (−40 %) | — |
| licence 0/1/2 of the cpuset (off) | 56/44/0 % | 60–69/31–40/0 % | 61–76/24–39/0 % | 78–83/1/16–21 % | 100/0/0 % |

With clip off about 75 % of the process is the kernel (perf, `avx2`); the JPEG previews take most of the rest. `scalar` cannot keep up: one channel thread is one core.

### Co-located workload (Skylake-SP downclocking)

`coloc.sh`: `mxl-cc-bench` paced to 100 frames per second (the work of 2 × 1080p50, camera picture) on core 20, a fixed scalar integer loop (`g3-int`) on core 20's other hyperthread (44) or on another core of the same socket (4); medians of 3 runs of 8 s. The loop alone: 369.4 million iterations per second at 3.6 GHz.

| Kernel | Matrix: loop on the sibling | RGB clip: loop on the sibling | RGB clip: loop on another core |
| --- | --- | --- | --- |
| `avx2` | 355.5 (3.50 GHz) | 352.4 (3.44 GHz) | 369.3 (3.6 GHz) |
| `avx512vl` | 357.0 (+0.4 % against `avx2`) | 356.5 (+1.2 %) | – |
| `avx512` | 341.3 (−4.0 %, 3.41 GHz, 17 % in licence 2) | 340.5 (−3.4 %) | 369.3 (0 %) |

The licence belongs to the physical core, so the other hyperthread runs at the lower clock while the kernel's 512-bit code (and about 2 ms after it) runs; another core keeps 3.6 GHz. Flat out (no pacing), the loop on the sibling loses 30 % with `avx512` against `avx2` (232 against 330).

### Decision (`auto`)

Rule of platform plan G: AVX-512 becomes `auto` only when it saves at least 15 % of the function's CPU and a workload on the same core loses less than 3 %.

| CPU family | `auto` | Why |
| --- | --- | --- |
| Skylake-SP, Cascade Lake, Cooper Lake (Intel AVX-512 without VBMI2) | `avx2` | `avx512` saves 19 % / 40 % but costs the other hyperthread 3.4–4 %; `avx512vl` costs nothing but saves 7 % with clip off (20 % with RGB gamut clip on bars, about 8 % on camera pictures). Opt-in: `avx512` where cores are not shared, `avx512vl` for RGB gamut clip on graphics-like content |
| Ice Lake, Sapphire Rapids, Emerald Rapids and later (Intel with VBMI2) | `avx512` | kernels 1.5–1.85× (Emerald Rapids runner), so about 30 % less process CPU; these CPUs keep their clock with AVX-512 (published), co-located cost not measured |
| Zen 5 | `avx512` | kernels 1.9–2.2× (EPYC 9V45 runner); full-width 512-bit units at full clock (published) |
| Zen 4 | `avx512` | 512-bit work double-pumped at full clock (published), so no co-located cost; expected gain from the 32 registers and masks about 1.2× (as `avx512vl` on the Gold 6136); not measured (the runners' VM hides AVX-512) |
| AVX2 only (i9-9900K, EPYC Rome/Milan), other AVX-512 CPUs | `avx2` | |

## 1.0.4: RGB gamut clip

A deployed corrector on the lab's 2× Xeon Gold 6136 (as in the 2026-10-03 run below): 4 channels, 1080p50 test-player bars with burn-in, gain 1.1, saturation 1.2, pedestal 0.01, 20 s measured. All grains on time in every case.

| RGB gamut clip | 1.0.3 | divisions → multiplications | 1.0.4 (also the inside test) |
| --- | --- | --- | --- |
| off | 0.79 cores | 0.79 cores | 0.79 cores |
| on | 1.89 cores | 1.66 cores | 1.33 cores |

`perf` on 1.0.3 with gamut clip: the time spread over the double-precision R′G′B′ test of every pixel (four lanes per instruction), with `vdivpd` the most expensive part. 1.0.4 multiplies by reciprocals and first checks eight pixels at a time in 32-bit fixed point: when R′, G′ and B′ are at least 2⁻¹⁰ inside [0, 1] and the codes inside the legal ranges, the coefficients' rounding (at most 886 · 2⁻²⁰) cannot move them out, so the exact test is skipped. Colour bars lie on the cube's faces and mostly still take the exact test; camera pictures mostly do not.

## 1.0.1: the whole v210 path in AVX2

`mxl-cc-bench` on one GitHub Actions runner (ubuntu-24.04, AVX2), this version and 1.0.0 built and run in the same CI job (`ci.yml`, step "bench"). One core, no other load.

| Format | Path | 1.0.0 | 1.0.1 | Channels / core (1.0.1) |
| --- | --- | --- | --- | --- |
| 1080p50 | matrix | 14.2 ms/frame | 0.70 ms/frame | 28.8 |
| 1080p50 | RGB gamut clip | 52.6 ms/frame | 11.8 ms/frame | 1.7 |
| 2160p50 | matrix | 57.0 ms/frame | 2.7 ms/frame | 7.3 |
| 2160p50 | RGB gamut clip | 211 ms/frame | 47.1 ms/frame | 0.42 |

1.0.0 vectorised only the 3×4 dot product of one 6-pixel group; unpacking, clamping and packing were scalar. 1.0.1 takes eight groups (48 pixels) at a time: the 32 words are transposed so that each register holds one word position of the eight groups, every sample is then a shift and a mask, the matrix runs in 32-bit fixed point with the same rounding, the clamps are min/max, and the inverse transpose packs the result. RGB gamut clip does the scalar double-precision conversion on four lanes with the same operations in the same order and rounds like `llround`. Both are bit-exact with the scalar reference, clip statistics included (unit tests on 6 to 1920 pixel wide rasters, all clip modes). A matrix whose sums could leave 32 bits (far outside the control ranges) and the groups after the last block of a line keep the per-group code.

A 1080p50 channel now needs well under a tenth of a core for the correction, also with RGB gamut clip on less than a core. 2160p50 with RGB gamut clip still needs about two and a half cores, which one channel thread cannot give (rows are not split across threads).

## 1.0.0

Measured with `mxl-cc-bench` on the build host (4-core Intel Xeon, AVX2). The bench applies a non-neutral matrix to packed v210. Channels per core is `1 / (seconds_per_frame × frame rate)`. RGB gamut clip stays on the scalar path because it converts every pixel through R′G′B′; the matrix path uses AVX2.

| Format | Matrix | Channels / core | RGB gamut clip | Channels / core |
| --- | --- | --- | --- | --- |
| 1080p50 | 13.3 ms/frame | 1.50 | 42.3 ms/frame | 0.47 |
| 2160p50 | 56.5 ms/frame | 0.35 | 169 ms/frame | 0.12 |

A 1080p50 channel with the matrix path fits in one frame on one core. 2160p50 needs about three cores per channel for the matrix path, and about eight when RGB gamut clip is on. These figures are not from a Dell Precision 3930; treat them as a starting point for the Kubernetes CPU request (one core per 1080p50 channel, three per 2160p50 channel, roughly triple that with RGB clip).

### Lab run 2026-10-03: Xeon Gold 6136, image 1.0.0

A deployed corrector (not `mxl-cc-bench`) on a 2× Xeon Gold 6136 host (Skylake-SP, 3.0 GHz, AVX2 and AVX-512). Inputs were 1080p50 v210 flows (mxl-test-player bars, `mxl-mv-writer` solids) routed by IS-05; controls gain 1.1, saturation 1.2, pedestal 0.01, `CC_CLIP=legal`. The channels ran in whole-grain mode (`fallback_reason: no_slice_commits`). 15 s warm-up, 30 s measured.

| Channels | RGB gamut clip | Output grains/s per channel | `late_grains_total` | Added latency (mean) | Process CPU |
| --- | --- | --- | --- | --- | --- |
| 1 | off | 37.5 | 390 | 25.8 ms | 1.00 core |
| 4 | off | 36.1–37.4 | 1638 | 26.0 ms | 4.00 cores |
| 1 | on | 30.9 | 598 | 31.3 ms | 1.00 core |

A 1080p50 channel does not keep real time on this CPU: one core per channel is saturated at about 27 ms per frame (37 fps), and the channel loop cannot use a second core. `perf` puts 72 % in `processLineAvx2`, 14 % in `unpackV210Group` and 6 % in `packV210Group`: only the 3×4 dot product is vectorised, the unpack, clamp and pack of each 6-pixel group are scalar. Keeping 1080p50 on a CPU like this needs a vectorised unpack/pack or rows split across threads within a channel.
