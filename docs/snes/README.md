# SNES Reference Documentation

Imported reference material from the `superpico-digital` exploration project, covering the "2-chip" SNES/Super Famicom hardware and the digital RGB capture technique this project's SNES capture backend is built on. These files are copied byte for byte from their source; only the `SNES_` filename prefix was dropped, since it is redundant inside this directory.

- `2-Chip_Overview.md`: High-level 2-chip SNES architecture, the main ICs and their part numbers, board revisions, clock frequencies, and logic levels.
- `2-Chip_PPU1_5C77.md`: Full pinout table and functional description of the S-PPU1 (5C77), the rendering engine that resolves backgrounds, sprites, and Mode 7, and hands a color index and priority to PPU2.
- `2-Chip_PPU2_5C78.md`: Full pinout table and functional description of the S-PPU2 (5C78), covering CGRAM, color math, windowing, and the final RGB video DAC.
- `2-Chip_CPU_5A22.md`: Full pinout table and functional description of the S-CPU (5A22), the 65C816-based main CPU with its DMA/HDMA and hardware math units.
- `2-Chip_APU_Audio.md`: The S-SMP (SPC700) and S-DSP audio subsystem, their pinouts, the audio signal path to the DAC, and ARAM.
- `2-Chip_Memory_Bus_Interconnect.md`: Pin-level interconnect tables between the CPU, PPU1, PPU2, VRAM, and WRAM, as close to a netlist as the source material provides.
- `2-Chip_Connectors_Pinouts.md`: External connector pinouts: the 62-pin cartridge slot, the 12-pin multi-out AV connector, and the expansion port.
- `2-Chip_Support_ICs.md`: The MAD-1 memory address decoder and CIC region-lock chip, plus the reset supervisor logic that gates them.
- `Digital_AV_Mod_Pin_Reference.md`: The master pin reference for the digital AV mod itself: PPU2's TST digital RGB pins, the PPU1 signals needed for Mode 7 and brightness handling, S-DSP digital audio taps, and an appendix mapping all of it to the QSB daughter board's FPC signal list (PIXEL_VALID, latched brightness, and the capture bus) that this project's own QSB is built from.
- `CAPTURE_BREAKTHROUGH.md`: A running log of the specific firmware/timing breakthroughs behind stable digital capture: hard sync on HBLANK/VBLANK, PIO phase-lock to PCLK, pixel setup-time padding, the frame reset handshake, ping-pong buffering, the RGB555 bit-reversal LUT, and horizontal offset calibration.
- `REFERENCES.md`: External references and prior art for the SNES digital capture mod: SNES hardware documentation sources, related TST-pin capture projects, the shmups.system11.org community thread, and firmware/RP2350 references.
- `shmups_thread.md`: Full 119-post crawl of the "Sharp analog RGB for the 3-Chip SNES using digital signals" thread on shmups.system11.org, the community discussion that worked out the TST-pin capture technique, the OVER/TOUMEI signal behavior, and the Mode 7 off-map handling this project's PIXEL_VALID logic is based on. Some inline image references point at local paths that were never crawled; see the import notes in the parent task report.
