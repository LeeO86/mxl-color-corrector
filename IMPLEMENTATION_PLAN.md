# Implementation notes

Pinned MXL: `dmf-mxl/mxl` `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, Fabrics off. The library is built from that git tag inside CMake (same sources the platform pin builds).

## Behaviour choices

* **Slice commits.** On this revision `mxlFlowWriterOpenGrain` clears `validSlices` only when the index is not already the open grain, and `mxlFlowWriterCommitGrain` leaves the grain open while `validSlices < totalSlices`. The worker opens a grain once, writes the newly committed input lines, and commits with an updated `validSlices`. The struct returned by `OpenGrain` is kept so `version`, `size`, `grainSize` and `totalSlices` are not zeroed. Input grain flags, including `MXL_GRAIN_FLAG_INVALID`, are copied onto the output. An invalid grain is copied through, not replaced with black. Flow directories are `<id>.mxl-flow/`, which is the layout this MXL revision writes.

* **Interlaced grain rate.** MXL rejects an interlaced `grain_rate` other than 25/1 or 30000/1001. The corrector reports whole-grain fallback for any non-progressive `interlace_mode` once the flow can be opened.

* **Co-siting.** v210 chroma belongs to the even luma sample. Odd luma uses that same pair. Output chroma is the even sample's result. A neutral matrix with legal samples is a byte copy, including line padding.

* **Matrix.** The RGB affine correction and the saturation scale are folded with the BT.709 matrices into one 3×4 fixed-point matrix (1/65536). Neutral controls force an exact identity. RGB gamut clip is a separate per-pixel step and does not round-trip samples that are already inside 0…1. `clip=off` still limits codes to the 10-bit container, 0–1023. Extended clip is 4–1019.

* **Read offset.** Slice mode follows the writer's head (offset 0). Whole-grain mode reads `CC_READ_OFFSET_GRAINS` behind the head, default 1, so the grain is complete. Fallback reasons are `interlaced`, `no_slice_commits` (four grains seen only as complete) and `slice_layout` (slice size is not an integer number of v210 lines that divide the frame). The change is logged once.

* **Added latency.** The histogram is the time from `mxlFlowReaderGetGrainSlice` returning (the slice is visible to this process) until `mxlFlowWriterCommitGrain` returns. It does not include the writer's own timestamp.

* **NMOS.** The node is an in-process IS-04 v1.3 and IS-05 v1.1 implementation with BCP-007-03 `video/v210` and `urn:x-nmos:transport:mxl`, deterministic UUIDv5 ids, and static registry registration. `NMOS_DNS_SD=true` is accepted and logged; this build does not advertise over DNS-SD. Receivers activate even when the domain or flow is missing (`waiting`, backoff to 2 s) and the IS-04 `subscription` follows the activation. A new input geometry mints a new output flow id.

* **Colour wheels.** `+x` is red (Cr), `+y` is blue (Cb). The resulting RGB trims are a luma-neutral chroma offset scaled so a full deflection stays inside the trim range. Numeric RGB fields may leave that plane; the wheel position is then the closest Cr/Cb reading.

* **State.** `/config/state.json` (or `CC_STATE_DIR`) stores both A/B slots and presets. If the directory cannot be created the process keeps running and logs a warning.

* **History.** Output domains are created with a 200 ms grain history so a 1080p50 flow stays near 60 MB.

Performance figures are in [docs/performance.md](docs/performance.md). They were measured on the build machine, not on a Dell Precision 3930.
