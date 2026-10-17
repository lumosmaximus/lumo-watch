# Traces

Recorded with `tools/trace/capture.py`; each `.trace` has an interactive `.html` (play, scrub) made by `tools/trace/animate.py`. Numbers from `tracelib.py`: fps over the capture, the longest gap between frames, mean and worst frame work, boxes per frame, and time the CPU stalled waiting for the DMA.

| Trace | What | fps | worst gap ms | work ms | worst ms | boxes | stall ms |
|---|---|---|---|---|---|---|---|
| [ab_free-run_gmt_bezel](ab_free-run_gmt_bezel.html) | A/B **free-run**: frames not locked to the panel's TE edge; scenario: GMT face, bezel turned 4 clicks forward and 2 back by script | 1771 | 32.4 | 0.6 | 32.2 | 1 | 0.03 |
| [ab_free-run_gmt_hands_cross](ab_free-run_gmt_hands_cross.html) | A/B **free-run**: frames not locked to the panel's TE edge; scenario: GMT face, clock set to 10:30:27 so the second hand crosses the minute hand | 887 | 19.1 | 1.1 | 19.1 | 2 | 0.06 |
| [ab_free-run_gmt_idle](ab_free-run_gmt_idle.html) | A/B **free-run**: frames not locked to the panel's TE edge; scenario: GMT face, hands spread out (03:50), nothing touched | 10448 | 13.4 | 0.1 | 13.3 | 0 | 0.01 |
| [ab_no-gate_gmt_bezel](ab_no-gate_gmt_bezel.html) | A/B **no-gate**: second-hand stage run in every box (before bounding-box gating); scenario: GMT face, bezel turned 4 clicks forward and 2 back by script | 54 | 50.6 | 16.2 | 33.8 | 28 | 0.76 |
| [ab_no-gate_gmt_hands_cross](ab_no-gate_gmt_hands_cross.html) | A/B **no-gate**: second-hand stage run in every box (before bounding-box gating); scenario: GMT face, clock set to 10:30:27 so the second hand crosses the minute hand | 48 | 33.8 | 15.9 | 19.9 | 33 | 0.90 |
| [ab_no-gate_gmt_idle](ab_no-gate_gmt_idle.html) | A/B **no-gate**: second-hand stage run in every box (before bounding-box gating); scenario: GMT face, hands spread out (03:50), nothing touched | 58 | 33.8 | 13.2 | 21.2 | 32 | 0.88 |
| [ab_no-overlap_gmt_bezel](ab_no-overlap_gmt_bezel.html) | A/B **no-overlap**: each box sent before the next is composed (no DMA overlap); scenario: GMT face, bezel turned 4 clicks forward and 2 back by script | 51 | 50.7 | 17.5 | 36.9 | 27 | 2.06 |
| [ab_no-overlap_gmt_hands_cross](ab_no-overlap_gmt_hands_cross.html) | A/B **no-overlap**: each box sent before the next is composed (no DMA overlap); scenario: GMT face, clock set to 10:30:27 so the second hand crosses the minute hand | 32 | 33.8 | 17.8 | 20.4 | 34 | 2.00 |
| [ab_no-overlap_gmt_idle](ab_no-overlap_gmt_idle.html) | A/B **no-overlap**: each box sent before the next is composed (no DMA overlap); scenario: GMT face, hands spread out (03:50), nothing touched | 58 | 33.8 | 14.0 | 23.0 | 32 | 1.97 |
| [ab_ring-flash_gmt_bezel](ab_ring-flash_gmt_bezel.html) | A/B **ring-flash**: bezel ring strip read from flash (before copying it to RAM); scenario: GMT face, bezel turned 4 clicks forward and 2 back by script | 52 | 50.6 | 16.7 | 34.7 | 26 | 0.75 |
| [ab_ring-flash_gmt_hands_cross](ab_ring-flash_gmt_hands_cross.html) | A/B **ring-flash**: bezel ring strip read from flash (before copying it to RAM); scenario: GMT face, clock set to 10:30:27 so the second hand crosses the minute hand | 41 | 33.8 | 16.8 | 19.6 | 34 | 0.96 |
| [ab_ring-flash_gmt_idle](ab_ring-flash_gmt_idle.html) | A/B **ring-flash**: bezel ring strip read from flash (before copying it to RAM); scenario: GMT face, hands spread out (03:50), nothing touched | 58 | 33.8 | 13.7 | 21.6 | 31 | 0.90 |
| [ab_zephyr-spi_gmt_bezel](ab_zephyr-spi_gmt_bezel.html) | A/B **zephyr-spi**: display writes through the Zephyr SPI driver (before direct SPI); scenario: GMT face, bezel turned 4 clicks forward and 2 back by script | 31 | 50.6 | 19.8 | 43.2 | 31 | 0.00 |
| [ab_zephyr-spi_gmt_hands_cross](ab_zephyr-spi_gmt_hands_cross.html) | A/B **zephyr-spi**: display writes through the Zephyr SPI driver (before direct SPI); scenario: GMT face, clock set to 10:30:27 so the second hand crosses the minute hand | 29 | 33.8 | 19.8 | 22.1 | 34 | 0.00 |
| [ab_zephyr-spi_gmt_idle](ab_zephyr-spi_gmt_idle.html) | A/B **zephyr-spi**: display writes through the Zephyr SPI driver (before direct SPI); scenario: GMT face, hands spread out (03:50), nothing touched | 58 | 33.7 | 15.9 | 24.8 | 31 | 0.00 |
| [chrono_black_idle](chrono_black_idle.html) | black chronograph, chronograph stopped | 59 | 16.9 | 1.6 | 13.2 | 10 | 0.15 |
| [chrono_white_idle](chrono_white_idle.html) | white chronograph, chronograph stopped | 59 | 16.8 | 1.9 | 10.6 | 10 | 0.15 |
| [gmt_bezel](gmt_bezel.html) | GMT face, bezel turned 4 clicks forward and 2 back by script | 55 | 33.8 | 16.2 | 32.1 | 27 | 0.75 |
| [gmt_hands_cross](gmt_hands_cross.html) | GMT face, clock set to 10:30:27 so the second hand crosses the minute hand | 36 | 33.8 | 17.1 | 19.1 | 34 | 0.87 |
| [gmt_idle](gmt_idle.html) | GMT face, hands spread out (03:50), nothing touched | 58 | 33.8 | 13.4 | 21.3 | 32 | 0.91 |
| [remote_knob](remote_knob.html) | remote, knob turned 5 steps up and 3 down by script (no helper app) | 57 | 33.8 | 1.6 | 33.8 | 0 | 0.00 |
