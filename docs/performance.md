# Performance

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
