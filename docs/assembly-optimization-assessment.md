# Assembly optimization assessment

Date: 2026-11-05. Static review of the 14 ESP32 emulator projects and shared
runtime. Priorities below are hypotheses from source inspection, not measured
speedups. Host assembly needs separate ESP32-P4 RISC-V and ESP32-S3 Xtensa
implementations where applicable; retain portable C as reference/fallback.

| Project | Potential | First candidates | Existing constraints / next steps |
| --- | --- | --- | --- |
| esp32ami | High | `amiga/display.c:draw` bitplane expansion, `compose`; `blitter.c:blit_area` | Expand multiple plane words with less intermediate traffic. Specialize copy, masked copy and common minterms in C before assembly. HAM pixels have sequential dependencies. |
| esp32arc | Medium | Shared `arm2/arm2.c` dispatch, shifter/ALU/flags; `archimedes/vidc.c:arc_render_line_rgb565` | Interpreter already has computed-goto dispatch, register-held execution state and direct pages. VIDC 4-bit conversion already uses paired lookup/store. Profile actual Tab5 workloads first. |
| esp32cbm | High shared CPU; medium graphics | Shared 6502 execution for machine and drives; `cbm/vic.c:char_pixels`, sprite composition; `sid.c:sample` | VIC uses 64-bit masks and cached character results. Measure SID separately; fixed-point conversion is a numerical change, not merely assembly. Preserve bus cycles and DMA. |
| esp32coco | Medium | `coco/gime/gime_video.c:graphics_line`, `widen`; `vdg/vdg.c:vdg_emit`; shared 6809 addressing/fetch/flags | Fuse decode and widening to remove temporary-buffer traffic before assembly. |
| esp32cpc | Medium | `cpc/video.c:pixels8`, `render_char`, Plus sprites; shared Z80 | Existing lookup tables and paired stores limit straightforward rewrite gains. Default video draws alternate frames. |
| esp32gs | High investigation priority | Shared 65C816 compact execution; `apple2gs/clemens/clem_audio.c:clem_ensoniq_sync`, oscillator processing; `video.c:shr_line` | Core already documents instruction-count and cache-footprint optimization. Excessive inlining was harmful; preserve compact loop. Profile active-voice audio separately. |
| esp32mac | Medium | `macintosh/video.c:mac_render_line_rgb565` monochrome expansion; Musashi/memory boundary | Try table expansion and packed stores first. CPU-bound applications need shared CPU work; simple audio is low priority. |
| esp32mac2 | High graphics | `mac2/video.c:lc_render_line`; `jmfb.c:jmfb_render_line` indexed and direct color | Specialize 1/2/4/8-bit expansion, RGB555/RGB565 and 24-bit conversion. Best depth depends on active workload. |
| esp32mac3 | High graphics | `mac3/video.c:video_render_line`, shared mac2/mac3 converters; Musashi/memory | Preserve alignment, mapping and VRAM change tracking. Static screens already skip work. |
| esp32msx | High for MSX2 graphics | `msx/vdp.c:draw_bitmap`, `cmd_start` copy/fill commands, sprites | Fuse addressing, palette lookup and packed stores. Preserve overlap direction, logical operations and command timing. MSX1 may benefit more from Z80 work. |
| esp32pc | High, complex | Shared x86 dispatch/ModR/M/RAM paths; `ibmpc/vga.c:render`, planar memory operations | Lazy flags and default chain-4 direct memory already exist. Preserve prefetch, protected-mode faults and device semantics. |
| esp32ste | High | `atari_st/video.c:render_line` 1/2/4-plane converters; `blitter.c:blit` | Current renderer extracts plane bits per pixel. Establish table/bit-transpose C baseline; specialize common blitter operations. |
| esp32xl | Medium | `atari_xl/gtia.c:draw_to`, `draw_gtia_mode` player/missile priority and collisions; shared 6502 | No-object paths already use tables and packed output. Preserve ANTIC DMA and mid-scanline timing. |
| esp32zx | Lower | `zx/ula.c:draw_paper_line`; shared Z80 | Unchanged scanlines already skipped. Packed expansion mainly helps animated scenes. Preserve contention and I/O timing. |

Shared `esp32emu` opportunities: Musashi (STE, Mac family, Amiga), Z80
(ZX, CPC, MSX, C128), 6502 (CBM machine/drives, XL), and presenter
`emu/emu.c:scale_row`. Framebuffer bandwidth may limit presenter gains.
`esp32emu-vscode` is debugger tooling, with no compelling assembly target.

Suggested bounded first experiments: STE planar conversion, Amiga bitplane /
blitter kernels, common mac2/mac3 color conversion. The user selected Acorn
Archimedes for the first actual hardware investigation instead.

## Measurement method

- Capture baseline and candidate with the same compiler, settings, workload,
  ROM, CPU model and display mode; repeat and report variation.
- Use `tools/emu_console.py PORT profile ELF 20000 25 1` for emulation and
  repeat with core `0` for the presenter. The ELF must match flashed firmware.
- CBM has `CBM_PROFILE`, CPC has `CPC_PROFILE`, GS has `gs_profile` counters.
- Compare kernel costs and whole-frame time / real-time speed and load.
- Preserve emulated instruction counts, timing, interrupts, memory side
  effects and pixel output; use existing CPU cases and differential tests.
- Compare optimized C and assembly, inspect generated instructions, and
  reject changes that only help microbenchmarks or increase cache pressure.

Hardware findings and Acorn measurements will be recorded in
`../../esp32arc/docs/assembly-experiment-2026-11-05.md`.

## Acorn hardware follow-up (2026-11-05)

Tab5 testing selected CPU execution over static desktop rendering:
`arm2_run` was 35.8% of core 1 time (about 90% of its non-idle samples),
while core 0 was 94.5% idle. A P4 inline assembly subtraction/CMP NZCV
candidate passed 20,162 target checks but gave no useful improvement in
a steady guest ADD/CMP/BNE workload: C 59.5-60.5% sampled CPU residency
across the initial/restored runs, assembly 60.4-61.0%. Both kept real time.
The candidate grew `arm2_run` by 314 bytes and was rejected; the C core
and baseline firmware were restored. See the Acorn experiment document
for the patch, raw summaries, workload and measurement limitations.

The subsequent [operand-decode experiments](../../esp32arc/docs/operand-decode-experiment-2026-11-05.md)
tested three compact C changes on Lander and the guest loop. Grouping the
register operand's shift type and amount into one field was retained:
the minute-long comparison improved Lander from 98.294% to 99.003% real
time (0.721% relative throughput), for 14 bytes more interpreter code.
All 190 CPU checks, 100,000 differential instructions and the deterministic
host Lander replay passed. An immediate fast path and direct conditional
dispatch were rejected for lack of useful gain. This is a small measured
improvement for the tested workload, not a general assembly speedup.

The subsequent [flag-setting dispatch experiment](../../esp32arc/docs/flags-dispatch-experiment-2026-11-05.md)
split data-processing handlers by the S bit already present in the dispatch
index. This removes unused flag work from the generated code. Two fresh-boot
Lander runs per build improved pooled real-time speed from 99.122% to 99.589%
(0.471% relative throughput); paired gains were 0.752% and 0.192%. The change
was retained, with a 6,482-byte internal code RAM cost and no table growth.
All 190 checks, 100,000 differential instructions and the identical host
replay passed. The workload is near its 50 FPS cap, and the small observed
gain varies between runs. Fetch/dispatch is the next target: a merged
dispatch tail accounted for 8.46% of a separate baseline core 1 profile.

The [dispatch-tail compiler experiments](../../esp32arc/docs/dispatch-tail-experiment-2026-11-05.md)
then tested `-fno-crossjumping -fno-gcse` and `-fno-crossjumping` alone.
Both increased code and were rejected: Lander reached 95.753% and 99.628%
real time versus 100.009% for the baseline. The paced ADD/CMP/BNE loop
used 63.4% and 59.7% mean `arm2_run` residency versus 56.8% after restoring
the committed settings. Both passed CPU checks, differential tests and
the host replay; the restored firmware's interpreter exactly matches the
baseline. More dispatch sites did not improve these P4 workloads.
