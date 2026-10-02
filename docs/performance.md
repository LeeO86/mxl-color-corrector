# Performance

Measured with `mxl-cc-bench` on the build host (4-core Intel Xeon, AVX2). The bench applies a non-neutral matrix to packed v210. Channels per core is `1 / (seconds_per_frame × frame rate)`. RGB gamut clip stays on the scalar path because it converts every pixel through R′G′B′; the matrix path uses AVX2.

| Format | Matrix | Channels / core | RGB gamut clip | Channels / core |
| --- | --- | --- | --- | --- |
| 1080p50 | 13.3 ms/frame | 1.50 | 42.3 ms/frame | 0.47 |
| 2160p50 | 56.5 ms/frame | 0.35 | 169 ms/frame | 0.12 |

A 1080p50 channel with the matrix path fits in one frame on one core. 2160p50 needs about three cores per channel for the matrix path, and about eight when RGB gamut clip is on. These figures are not from a Dell Precision 3930; treat them as a starting point for the Kubernetes CPU request (one core per 1080p50 channel, three per 2160p50 channel, roughly triple that with RGB clip).
