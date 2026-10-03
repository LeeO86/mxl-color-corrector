# Performance

Measured with `mxl-cc-bench` on the build host (4-core Intel Xeon, AVX2). The bench applies a non-neutral matrix to packed v210. Channels per core is `1 / (seconds_per_frame × frame rate)`. RGB gamut clip stays on the scalar path because it converts every pixel through R′G′B′; the matrix path uses AVX2.

| Format | Matrix | Channels / core | RGB gamut clip | Channels / core |
| --- | --- | --- | --- | --- |
| 1080p50 | 13.3 ms/frame | 1.50 | 42.3 ms/frame | 0.47 |
| 2160p50 | 56.5 ms/frame | 0.35 | 169 ms/frame | 0.12 |

A 1080p50 channel with the matrix path fits in one frame on one core. 2160p50 needs about three cores per channel for the matrix path, and about eight when RGB gamut clip is on. These figures are not from a Dell Precision 3930; treat them as a starting point for the Kubernetes CPU request (one core per 1080p50 channel, three per 2160p50 channel, roughly triple that with RGB clip).

## Lab run 2026-10-03: Xeon Gold 6136, image 1.0.0

A deployed corrector (not `mxl-cc-bench`) on a 2× Xeon Gold 6136 host (Skylake-SP, 3.0 GHz, AVX2 and AVX-512). Inputs were 1080p50 v210 flows (mxl-test-player bars, `mxl-mv-writer` solids) routed by IS-05; controls gain 1.1, saturation 1.2, pedestal 0.01, `CC_CLIP=legal`. The channels ran in whole-grain mode (`fallback_reason: no_slice_commits`). 15 s warm-up, 30 s measured.

| Channels | RGB gamut clip | Output grains/s per channel | `late_grains_total` | Added latency (mean) | Process CPU |
| --- | --- | --- | --- | --- | --- |
| 1 | off | 37.5 | 390 | 25.8 ms | 1.00 core |
| 4 | off | 36.1–37.4 | 1638 | 26.0 ms | 4.00 cores |
| 1 | on | 30.9 | 598 | 31.3 ms | 1.00 core |

A 1080p50 channel does not keep real time on this CPU: one core per channel is saturated at about 27 ms per frame (37 fps), and the channel loop cannot use a second core. `perf` puts 72 % in `processLineAvx2`, 14 % in `unpackV210Group` and 6 % in `packV210Group`: only the 3×4 dot product is vectorised, the unpack, clamp and pack of each 6-pixel group are scalar. Keeping 1080p50 on a CPU like this needs a vectorised unpack/pack or rows split across threads within a channel.
