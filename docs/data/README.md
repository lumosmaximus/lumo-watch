# Raw captures from the board

Serial output from core 0, one capture per build, saved as captured. Each
second core 0 prints what core 1 measured over the last second:

```
core 1: <fps> fps, <us per frame>: <px redrawn> px in <boxes> boxes, composite <us>, write <us>
   stages us: bg <us> ring <us> hands <us> sec <us> (<px> px) swap <us>
   spi: <n> underruns, overlap <0|1>          (from 09 on)
```

All values are per-frame averages over that second. Seconds where the bezel
was being turned show a large `ring` value.

| File | Build | What it shows |
|---|---|---|
| 01-lut-drag | polar-LUT bezel, bezel turned by hand | ring stage drops, dips while turning |
| 02-lut-progressive-ring-drag | + progressive ring repaint, shape gating | 59 fps with 30 fps runs when frames pass 16.7 ms |
| 03-async-writer-thread | + writer thread sending band buffers | slower compositing, write not overlapped |
| 04-polling-spi-late-frames | polling SPI, late frames start without waiting for TE | write time unchanged; judder (seen on panel) |
| 05-locked-30fps | every other TE | steady 30 fps |
| 06-pre-lut-restored | back to bilinear bezel (f955da1) | 59 fps, deep dips while turning the bezel |
| 07-direct-spi | qspi_fast.c, DMA overlapped with compositing | write 3.4 ms -> 0.5 ms |
| 08-hand-cache | + cache of pixels under the hands | hands 5.5 ms -> 0.5 ms (panel showed corruption) |
| 09-underruns-overlap-vs-sync | underrun counter; overlap first 25 s, then synchronous | 0 underruns either way |
| 10-cache-verify-strict | cache + recompute check, exact compare (in `spi:` line: count, first y<<16|x) | many 1-LSB rounding differences |
| 11-cache-verify-visible | same, only differences > 8 levels counted | 0 visible mismatches |
| 12-ram-overlap-fix | core 0 RAM carve-out fixed | timing unchanged |
| 13-cache-removed | cache removed, direct SPI kept | ~12 ms frames; dips only when turning the bezel or hands overlap |
| 14-lut-bezel-full-ring-bench | LUT bezel + direct SPI, boot benchmark of one bezel click | whole ring (65k px) repainted in 34.9 ms: two frames too long |

Captured with `tools/capture/cap.py <seconds>` (resets the board through the
USB-serial DTR/RTS lines, then records).
