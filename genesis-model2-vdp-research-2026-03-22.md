# Sega Genesis / Mega Drive Model 2 VDP Research

Date: 2026-03-22

Scope: stock Sega Genesis / Mega Drive Model 2 mainboards up to VA3. VA4 is a GOAC and is out of scope except where it helps explain signal continuity.

Method:
- Prefer Sega/JVC service manuals, official development manuals, schematics, and encoder datasheets.
- Use community sources only for board-revision matrices, Toshiba-ASIC pinout reconstruction, and later hardware findings not covered by Sega docs.
- Separate `confirmed`, `strong evidence`, and `open`.

Confidence legend:
- `High`: directly stated in official documentation or repeated in multiple independent primary sources.
- `Medium`: community-reconstructed, but consistent with board surveys/modding practice and other docs.
- `Low`: plausible but not fully documented in a primary source I could verify.

## Executive Summary

High-confidence findings:
- Model 2 consoles do not expose a standalone VDP package. The VDP core is inside the main ASIC at `IC6`.
- Two ASIC lineages matter for Model 2 video work:
  - Yamaha `FC1004` family: `315-5487`, `315-5660`, `315-5660-01`, `315-5660-02`, `315-5708-01`, and likely `315-5700` in some later units.
  - Toshiba ASICs: `315-5786` and `315-5685`.
- On the FC1004 family, Sega/JVC service documentation gives a mostly complete and trustworthy 208-pin pinout. The externally exposed video-related pins are analog `R/G/B`, `CSYNC`, `HSYNC`, `VSYNC`, `YS`, `SBCR`, `EDCLK`, and clock/reset/bus signals. No external digital pixel bus is documented.
- The older discrete VDP service manual for `315-5313` confirms the same VDP-side signal semantics: `R/G/B` are analog outputs, `SBCR` is subcarrier output, `EDCLK` is external dot clock, and `YS` is a transparency/blanking-related output.
- Official Sega manuals document the VDP architecture as `64 KiB VRAM + 64 x 9-bit CRAM + 40 x 10-bit VSRAM`, with line fetch of the next raster occurring about `36 clocks` before display and with active-scan access throttling.
- The Model 2 video encoder stage is fed with analog RGB plus sync/subcarrier, not a digital pixel bus. Official encoder datasheets for `MB3514` and `CXA1645` explicitly require analog RGB + composite sync + subcarrier inputs.

Most important design conclusion:
- On Model 2 boards through VA3, I found no documented external post-compositor, pre-DAC digital video bus. A truly digital-to-digital HDMI mod therefore does **not** currently have an evidence-backed direct tap point equivalent to a simple RGB TTL bus. The nearest practical pre-encoder interception point is analog `R/G/B` at the ASIC outputs.

What remains uncertain:
- Exact retail-board distribution of all FC1004 suffixes by revision, especially `315-5660-01`, `315-5708-01`, and `315-5700`.
- Toshiba-ASIC full pinout. A useful partial pinout exists, but it is explicitly incomplete.
- Whether any exploitable internal/video-adjacent behavior on the external VRAM serial interface can reduce reconstruction complexity enough for a robust all-digital capture path.

## Model 2 VDP / Main-ASIC Identification

Important distinction:
- On Model 2, "the VDP" is functionally a block inside the main ASIC. For hardware-mod purposes, the relevant package is the main ASIC at `IC6`.

### Variant matrix

| ASIC / marking | Family | Evidence | Known Model 2 revisions | Notes | Confidence |
|---|---|---|---|---|---|
| `315-5487` | Yamaha FC1004 | ConsoleMods board survey; FC1004 family docs | Early VA0 | Original FC1004 generation; broken 50 Hz support | Medium |
| `315-5487-10` | Yamaha FC1004-X | ConsoleMods board survey | JP VA0 | 60 Hz only variant | Medium |
| `315-5487•` | Yamaha FC1004 | ConsoleMods board survey | Very early PAL VA0 | Rare fixed-50 Hz variant | Medium |
| `315-5660-R` | Yamaha FC1004 Rev. | Official PAL VA0 service manual | VA0 service documentation | Official `IC6` designation in Sega PAL Model 2 service manual | High |
| `315-5660` | Yamaha FC1004 | Official VA1 and VA3 service manuals; board surveys | VA0 late, VA1, VA1.8, VA3 | Most common FC1004 on Model 2 | High |
| `315-5660-02` | Yamaha FC1004 | Official VA1 service-parts table and VA3 parts list | VA1 service replacement, VA1.8, VA3 | Officially supported replacement; observed on boards | High |
| `315-5660-01` | Yamaha FC1004 | Official VA1 service-parts table | VA1 service replacement | I found official service-part evidence, not direct retail-board proof | Medium |
| `315-5708-01` | Yamaha FC1004 | Official VA1 service-parts table; community identification as Fujitsu-made 5660 | VA0/VA1 service replacement | Official replacement part; retail distribution uncertain | Medium |
| `315-5700` | Yamaha FF1004 | ConsoleMods ASIC info / motherboard survey | Some VA1.8 and VA3 | Not in the VA3 service-parts table I found; treat as observed-board evidence, not Sega-documented default | Medium |
| `315-5786` | Toshiba `T9N13BF` | ConsoleMods motherboard survey, ASIC info, Toshiba pinout | VA2; early VA2.3 | No integrated YM3438; discrete YM2612 present | Medium |
| `315-5685` | Toshiba `TC6158AF` | ConsoleMods motherboard survey, ASIC info, Toshiba pinout | Later VA2.3 | Known-buggy VDP core: shadow/highlight and raster issues | Medium |

Notes on disagreement:
- Some community sources distinguish `315-5487` from `315-5487-01` on early US/JP boards, while others collapse them into the same early FC1004 generation. I did not find an official VA0 NTSC Sega service document in this pass that resolves the exact retail marking split.
- I did not find an official Sega VA2/VA2.3 service manual that names `315-5786`/`315-5685`, so Toshiba-era board-to-ASIC mapping remains community-sourced here.

### Board revisions and board part numbers

| Revision | Board part numbers seen in surveyed docs | Main ASIC(s) relevant to VDP | Confidence |
|---|---|---|---|
| VA0 | `171-6349B-01`, `171-6349A-11`, `171-6349C-10`, `171-6349A-21` | `315-5487`, `315-5487-10`, `315-5487•`, later `315-5660`, possibly `315-5708` | Medium |
| VA1 | `171-6534A`, `171-6534A-10`, `171-6534A-11`, `171-6534A-21` | `315-5660`; official service replacements `315-5660-02`, `315-5660-01`, `315-5708-01` | High for service replacements; medium for exact shipped mix |
| VA1.8 | `171-6534A-11`, `171-6534B-10`, `171-6534B-11`, `171-6534B-20`, `171-6534B-21` | `315-5660`, `315-5660-02`, some `315-5700` | Medium |
| VA2 | `171-6535F-10`, `171-6535F-11` | `315-5786` | Medium |
| VA2.3 | `171-7039B-10`, `171-7039C-11` | `315-5786` early, `315-5685` later | Medium |
| VA3 | `171-6615B-13`, `171-6615E-10`, `171-6615F-11`, `171-6615F-13` | `315-5660`, `315-5660-02`, some `315-5700` | High for `5660`/`5660-02`; medium for `5700` |

### Related non-Model-2 parts that matter for inference

| Part | Why it matters |
|---|---|
| `315-5313` / Yamaha `YM7101` | Standalone Model 1 VDP. Its service manual gives the cleanest description of VDP-side signal names, directions, and frequencies later preserved in FC1004-family pin naming. |
| Yamaha `YM6045`, `YM6046`, `YM3438` | JVC technical manual shows FC1004 internally combines these blocks with the VDP. |
| `315-5960` | Later GOAC. Out of scope for Model 2 VA0-VA3, but helpful when comparing signal removal/addition in later Sega ASICs. |

## Pinout Notes

To keep the tables usable, contiguous sequential buses are grouped where the source itself groups them. These groups are exact pin-by-pin mappings, e.g. `154-176 = VA1-VA23`.

### FC1004 family (`315-5660` class) 208-pin package

Status:
- Based primarily on Sega PAL Model 2 VA0 service manual and JVC X'Eye technical manual.
- Very likely valid for `315-5660`, `315-5660-02`, and other FC1004-family drop-ins.
- Some undocumented/test pins remain unclear.

#### FC1004 grouped pinout

| Pins | Signal(s) | Dir. | Function | HDMI relevance | Confidence | Source basis |
|---|---|---:|---|---|---|---|
| 1-8 | `SD0-SD7` | I | Dual-port VRAM serial data bus | Upstream memory interface only; not documented as final pixel data | High | Sega VA0 manual; JVC manual |
| 9 | `SE1` | O | Dual-port VRAM control | Only relevant if studying VRAM timing | High | Sega VA0 manual |
| 10 | `SE0` | O | Dual-port VRAM control | Same | High | Sega VA0 manual |
| 11 | `SC` | O | Dual-port VRAM control | Same | High | Sega VA0 manual |
| 12 | `RAS1` | O | VRAM row strobe | Useful for VRAM-interface reverse engineering | High | Sega VA0 manual |
| 13 | `CAS1` | O | VRAM column strobe | Same | High | Sega VA0 manual |
| 14 | `WE1` | O | VRAM write enable / one side of VRAM control set | Same | High | Sega VA0 manual |
| 15 | `WE0` | O | VRAM write enable | Same | High | Sega VA0 manual |
| 16 | `OE1` | O | VRAM output enable | Same | High | Sega VA0 manual |
| 17-25 | `RD0-RD7` | I/O | Dual-port VRAM data bus | Upstream memory interface; not sufficient alone for finished video | High | Sega VA0 manual |
| 26-33 | `AD0-AD7` | I/O | Dual-port VRAM address/data bus | Same | High | Sega VA0 manual |
| 34 | `VIDEOAVSS` | - | Video analog ground | Important for probing analog RGB cleanly | High | Sega VA0 manual |
| 35 | `R` | O | Analog red output | Primary pre-encoder tap point | High | Sega VA0 manual; JVC manual |
| 36 | `G` | O | Analog green output | Primary pre-encoder tap point | High | Sega VA0 manual; JVC manual |
| 37 | `B` | O | Analog blue output | Primary pre-encoder tap point | High | Sega VA0 manual; JVC manual |
| 38 | `VIDEOAVDD` | - | Analog video supply | Important reference for probing | High | Sega VA0 manual |
| 39 | `YS` | O | Transparency / blanking-related output to cartridge | Important if studying 32X-style overlay or transparency timing | High | Sega VA0 manual; Model 1 VDP manual; cartridge pinout |
| 40 | `SPA/B` | I/O | Sprite timing / other-VDP related signal | Possibly relevant only for external overlay or multi-VDP use | Medium | JVC manual / Model 1 VDP manual wording |
| 41 | `VSYNC` | O | Vertical sync | Critical timing reference | High | Sega VA0 manual; JVC manual |
| 42 | `CSYNC` | I/O | Composite sync | Critical timing reference; also direct encoder feed on some boards | High | Sega VA0 manual; JVC manual; RGB-bypass tracing |
| 43 | `HSYNC` | I/O | Horizontal sync | Critical timing reference | High | Sega VA0 manual; JVC manual |
| 44 | `VDD` | - | Digital +5 V supply | Power rail | High | Sega VA0 manual |
| 45 | `M3` | I | Mode / Master System-related control from cartridge | Matters for SMS compatibility, less for HDMI capture | High | Sega VA0 manual; connector pinout |
| 46 | `NTSC` | I | NTSC/PAL selection | Important for 50/60 Hz behavior | High | Sega VA0 manual; JVC manual |
| 47 | `VPA` | O | 68000 valid peripheral address | Bus-timing only | High | JVC manual |
| 48 | `HALT` | O | 68000 halt | Bus-timing only | High | JVC manual |
| 49 | `RESET` / `VRES` | O | 68000 reset / system reset relationship | Useful for bring-up and region/bodge study | High | Sega VA0 manual; JVC manual |
| 50-51 | `FC0-FC1` | I | 68000 function code inputs | Bus-timing only | High | JVC manual |
| 52 | `MREQ` | I/O | Z80 memory request | Bus-timing only | High | JVC manual |
| 53 | `VSS` | - | Ground | Power rail | High | Sega VA0 manual |
| 54 | `AUVSS` / `SOUNDVSS` | - | Audio analog ground | Audio only | High | Sega VA0 manual / JVC manual |
| 55 | `MOR` | O | FM analog output | Audio only | High | JVC manual |
| 56 | `MOL` | O | FM analog output | Audio only | High | JVC manual |
| 57 | `SOUNDAVDD` | - | Audio analog supply | Audio only | High | Sega VA0 manual / JVC manual |
| 58 | `SOUND` | I/O | Documented as unused/open in FC1004 docs | Not useful unless later proven otherwise | Medium | Sega VA0 manual |
| 59 | `ZRES` | O | Z80 reset | Bus/control only | High | Sega VA0 manual; JVC manual |
| 60 | `ZBAK` | I | Z80 bus acknowledge | Bus/control only | High | JVC manual |
| 61 | `NMI` | O | Z80 NMI | Bus/control only | High | JVC manual |
| 62 | `ZBR` | I/O | Z80 bus request | Bus/control only | High | Sega VA0 manual; JVC manual |
| 63 | `WAIT` | I/O | Z80 wait / timing | Bus/control only | High | JVC manual |
| 64 | `EOE` | O | PSRAM upper output enable | DMA/bus only | High | JVC manual |
| 65 | `NOE` | O | PSRAM lower output enable | DMA/bus only | High | JVC manual |
| 66 | `ZRAM` | O | Z80 RAM chip enable | Audio/Z80 only | High | JVC manual |
| 67 | `REF` | O | Z80 RAM refresh-related output | Audio/Z80 only | High | JVC manual |
| 68 | `CAS2` | O | Cartridge / external memory timing | External memory only | High | JVC manual |
| 69 | `RAS2` | O | External memory timing | External memory only | High | JVC manual |
| 70 | `ASEL` | O | Cartridge / external memory address select | Important for 32X/Virtua Racing compatibility study | High | JVC manual; motherboard notes |
| 71 | `ROM` | O | External memory control | External memory only | High | JVC manual |
| 72 | `FDC` | O | External memory / disk control | Expansion only | High | JVC manual |
| 73 | `FDWR` | O | External memory write control | Expansion only | High | JVC manual |
| 74 | `CEO` | O | Cartridge chip enable | Cartridge timing | High | JVC manual |
| 75 | `TIME` | O | Cartridge timing signal | External timing | High | JVC manual |
| 76 | `CART` | I | Cartridge detect | Low relevance for HDMI | High | JVC manual |
| 77 | `IA14` | O | PSRAM address / control-related output | DMA/bus only | High | JVC manual |
| 78 | `WRES` | I | Reset button / warm reset input | Bring-up only | High | JVC manual |
| 79 | `DISK` | I/O | Connected low / expansion-related | Expansion only | Medium | JVC manual |
| 80 | `VDD` | - | +5 V | Power rail | High | Sega VA0 manual |
| 81 | `TEST0` | I/O | Test | Normally force low/open per docs | Medium | Sega VA0 manual |
| 82-84 | `TEST1-TEST3` | I/O | Test pins | Leave open per docs | Medium | Sega VA0 manual |
| 85-91 | `PC0-PC6` | I/O | Joypad interface | Not video related | High | JVC manual |
| 92 | `VSS` | - | Ground | Power rail | High | Sega VA0 manual |
| 93-99 | `PB0-PB6` | I/O | Joypad interface | Not video related | High | JVC manual |
| 100-106 | `PA0-PA6` | I/O | Joypad interface | Not video related | High | JVC manual |
| 107 | `JAP` | I/O | Region select (JP/Export) | Important for region mod behavior | High | JVC manual |
| 108 | `FRES` | O | Expansion / disk reset-related | Expansion only | High | JVC manual |
| 109 | `ZV` | I/O | Undocumented; docs say open | Unknown | Medium | Sega VA0 manual |
| 110 | `VZ` | I/O | Undocumented; docs say open | Unknown | Medium | Sega VA0 manual |
| 111 | `IO` | I/O | Undocumented / open in docs | Unknown | Medium | Sega VA0 manual |
| 112-127 | `ZA0-ZA15` | I/O | Z80 address bus | Bus only | High | JVC manual |
| 128 | `SRES` | I | Power-supply reset | Bring-up only | High | JVC manual |
| 129 | `SEL1` | I | Tied low in JVC docs | Clock / CPU interface selection | Medium | JVC manual |
| 130 | `CLK` / `VCLK` | I/O | 68000 clock | Useful for aligning bus captures | High | Sega VA0 manual; JVC manual; 32X clock manual |
| 131 | `SBCR` | O | Color subcarrier output | Key for encoder input, jailbar analysis | High | JVC manual; Model 1 VDP manual; jailbar-fix docs |
| 132 | `ZCLK` | I/O | Z80 clock | Audio/timing only | High | JVC manual; Model 1 VDP manual |
| 133 | `VSS` | - | Ground | Power rail | High | Sega VA0 manual |
| 134 | `MCLK` | I | Master clock input | Primary timing reference | High | JVC manual; Model 1 VDP manual; 32X clock manual |
| 135 | `EDCLK` | I/O | External dot clock | Key pixel-clock reference on cart edge | High | Sega VA0 manual; Model 1 VDP manual; cartridge pinout |
| 136 | `VDD` | - | +5 V | Power rail | High | Sega VA0 manual |
| 137-152 | `VD0-VD15` | I/O | 68000 data bus | Important for register-write timing study, not direct pixels | High | JVC manual |
| 153 | `VSS` | - | Ground | Power rail | High | Sega VA0 manual |
| 154-176 | `VA1-VA23` | I/O | 68000 address bus | Important for VDP register / DMA timing study, not direct pixels | High | JVC manual |
| 177 | `SOUNDAVDD` | - | Audio analog supply | Audio only | High | Sega VA0 manual |
| 178 | `PSG` | O | PSG analog output | Audio only | High | JVC manual |
| 179 | `SOUNDAVSS` | - | Audio analog ground | Audio only | High | Sega VA0 manual |
| 180 | `VSS` | - | Ground | Power rail | High | Sega VA0 manual |
| 181 | `INT` | O | Z80 interrupt request | Bus/control only | High | JVC manual |
| 182 | `BR` | O | 68000 bus request | DMA/bus arbitration | High | JVC manual |
| 183 | `BGACK` / `SGACK` | I/O | 68000 bus grant acknowledge | DMA/bus arbitration | High | JVC manual |
| 184 | `BG` | I | 68000 bus grant | DMA/bus arbitration | High | JVC manual |
| 185-186 | `IPL1-IPL2` | O | 68000 interrupt priority outputs | Timing/control only | High | JVC manual |
| 187 | `IORQ` | I | Z80 I/O request | Bus only | High | JVC manual |
| 188 | `ZRD` | I/O | Z80 read control | Bus only | High | JVC manual |
| 189 | `ZWR` | I/O | Z80 write control | Bus only | High | JVC manual |
| 190 | `M1` | I | Z80 opcode fetch indicator | Bus only | High | JVC manual |
| 191 | `AS` | I/O | 68000 address strobe | Bus timing only | High | JVC manual |
| 192 | `UDS` | I/O | 68000 upper data strobe | Bus timing only | High | JVC manual |
| 193 | `LDS` | I/O | 68000 lower data strobe | Bus timing only | High | JVC manual |
| 194 | `R/W` | I/O | 68000 read/write | Bus timing only | High | JVC manual |
| 195 | `DTAK` | I/O | 68000 data acknowledge | Bus timing only | High | JVC manual |
| 196 | `UWR` | O | PSRAM upper write enable | DMA/bus only | High | Sega VA0 manual |
| 197 | `LWR` | I/O | PSRAM lower write enable | DMA/bus only | High | JVC manual |
| 198 | `CAS0` | I/O | PSRAM column strobe | DMA/bus only | High | JVC manual |
| 199 | `RAS0` | O | PSRAM row strobe | DMA/bus only | High | JVC manual |
| 200-207 | `ZD0-ZD7` | I/O | Z80 data bus | Audio/bus only | High | JVC manual |
| 208 | `VDD` | - | +5 V | Power rail | High | Sega VA0 manual |

### Toshiba ASICs (`315-5786`, `315-5685`) 208-pin package

Status:
- The best public source I found is a partial ConsoleMods pinout. It is explicit that several pins remain unknown.
- Treat identified video/timing pins as medium-confidence, and unknown/NC areas as open.

#### Identified Toshiba pins most relevant to video

| Pins | Signal(s) | Dir. | Function | HDMI relevance | Confidence | Source basis |
|---|---|---:|---|---|---|---|
| 31 | `VCLK` | ? | 68000 clock | Bus alignment | Medium | ConsoleMods Toshiba pinout |
| 39 | `EDCLK` | ? | External dot clock | Key timing reference | Medium | ConsoleMods Toshiba pinout |
| 41 | `VSYNC` | ? | Vertical sync | Critical timing reference | Medium | ConsoleMods Toshiba pinout |
| 44 | `HSYNC` | ? | Horizontal sync | Critical timing reference | Medium | ConsoleMods Toshiba pinout |
| 46 | `YS` | ? | Blanking / transparency-related output | Overlay / transparency study | Medium | ConsoleMods Toshiba pinout; cartridge pinout naming |
| 103 | `SBCR` | ? | Subcarrier | Important for encoder input and jailbar issue | Medium | ConsoleMods Toshiba pinout; VA2 jailbar fix docs |
| 104 | `CSYNC` | ? | Composite sync | Critical timing reference; likely encoder input | Medium | ConsoleMods Toshiba pinout |
| 107 | `Red` | ? | Analog red output | Primary pre-encoder tap point | Medium | ConsoleMods Toshiba pinout |
| 108 | `Green` | ? | Analog green output | Primary pre-encoder tap point | Medium | ConsoleMods Toshiba pinout |
| 109 | `Blue` | ? | Analog blue output | Primary pre-encoder tap point | Medium | ConsoleMods Toshiba pinout |
| 112 | `SOUND` | ? | Audio-related | Not video relevant | Medium | ConsoleMods Toshiba pinout |
| 114 | `REF` | ? | Z80 RAM refresh-related | Not video direct | Medium | ConsoleMods Toshiba pinout |
| 130-152 | `ZA0-ZA15`, `ZD6`, etc. | ? | Z80 address/data | Bus only | Medium | ConsoleMods Toshiba pinout |
| 153 | `NTSC/PAL` | ? | Region/video frequency select | Important for 50/60 Hz | Medium | ConsoleMods Toshiba pinout |
| 155 | `MCLK` | ? | Master clock input | Primary timing reference | Medium | ConsoleMods Toshiba pinout |
| 157 | `SRES` | ? | System reset | Bring-up only | Medium | ConsoleMods Toshiba pinout |
| 159 | `WRES` | ? | Warm/reset button | Bring-up only | Medium | ConsoleMods Toshiba pinout |
| 166-179 | `PA*`, `PB*` | ? | Joypad I/O | Not video related | Medium | ConsoleMods Toshiba pinout |
| 182 | `RAS2` | ? | External memory timing | Expansion only | Medium | ConsoleMods Toshiba pinout |
| 184-188 | `FRES`, `FDWR`, `IA14`, `EOE`, `NOE` | ? | External/bus timing | Indirect relevance | Medium | ConsoleMods Toshiba pinout |
| 189 | `RAS0` | ? | Main RAM row strobe | DMA/bus only | Medium | ConsoleMods Toshiba pinout |
| 201-203 | `UDS`, `LDS`, `R/W` | ? | 68000 strobes | Bus timing only | Medium | ConsoleMods Toshiba pinout |

#### Toshiba unknowns / caution

The ConsoleMods page explicitly marks many Toshiba pins `Unknown (NC)`. Do not assign them functions in hardware until they are traced on a real board or verified from a better source.

## Signal and Timing Analysis

### Master and derived clocks

Confirmed / high-confidence:
- Sega’s 32X hardware manual states the Mega Drive master clocks are:
  - NTSC: `53.693175 MHz`
  - PAL: `53.203424 MHz`
- The same manual states `Vclk = 7 * Mck`, meaning the 68000 clock is `fosc / 7`, i.e. about:
  - NTSC: `7.670454 MHz`
  - PAL: `7.600489 MHz`
- The discrete `315-5313` VDP manual identifies:
  - `HCK` / master clock input at about `53 MHz`
  - `EDCK` as dot clock input/output at about `13.4 / 10.7 MHz`
  - `SBCR` as subcarrier output at about `4.47 / 3.58 MHz`
  - `CLKO` as Z80 clock at about `3.58 MHz`

Strong evidence / inference:
- Because FC1004 retains the same `MCLK`, `EDCLK`, `SBCR`, `YS`, `HSYNC`, `VSYNC`, and `CSYNC` naming and VDP behavior, the discrete `315-5313` timing semantics are the safest available proxy for FC1004-family Model 2 boards.
- `EDCLK` corresponds to the externally exposed dot clock used on the cartridge edge. From the documented frequencies, the VDP is operating in two dot-clock domains:
  - about `13.423 MHz` (`53.693175 / 4`) for NTSC H40
  - about `10.739 MHz` (`53.693175 / 5`) for NTSC H32
  - PAL equivalents scale from the PAL master clock.

Open:
- I did not find an official Sega Model 2 document in this pass that explicitly states the H32/H40 divider relationship in those terms, even though the discrete VDP manual gives the resulting frequencies.

### Vertical cadence

Confirmed:
- Official Sega development docs give total raster counts:
  - NTSC V28: `262` lines total, `224` display + `38` retrace
  - PAL V28: `312` lines total, `224` display + `88` retrace
  - PAL V30: `312` lines total, `240` display + `72` retrace
- The vertical interrupt occurs just after V retrace.

### Horizontal cadence

Confirmed:
- Official docs state the horizontal interrupt occurs just before H retrace.
- Official docs state the VDP fetches the next line’s display information in about `36 clocks`, so writes near H-INT affect the next line, not the current one.

Strong evidence:
- Kabuto’s hardware notes state the VDP runs with `3420 master clock ticks per line` on real hardware. This is consistent with the published master clocks and TV-rate line frequencies, but it is community measurement, not Sega documentation.

### Active-scan memory access timing

Confirmed:
- Sega docs say the CPU and VDP time-share VRAM/CRAM/VSRAM.
- During active scan, accesses are limited; during vertical blank, accesses are continuous.
- Sega’s docs explicitly say that in H32 mode the CPU can access VRAM `16 times` during active display on one line, with VRAM writes byte-wide and CRAM/VSRAM accesses word-wide.
- Sega’s docs give active-scan cycle budgets:
  - `167` cycles in H32
  - `205` cycles in H40
- Sega’s docs give maximum VDP-write wait times:
  - about `5.96 us` in H32
  - about `4.77 us` in H40

Relevance:
- These limits matter if any external reconstruction scheme depends on watching CPU writes rather than observing the video stream directly.

## Video Pipeline Reconstruction

### What the official docs say

Confirmed:
- The VDP stores pattern/tile/sprite data in `VRAM`, colors in `CRAM`, and per-column/per-plane vertical scroll values in `VSRAM`.
- The display consists of sprite, scroll A, scroll B, window, and background composition logic.
- Color comes from `CRAM`: each pixel carries a color code and palette selection; `CRAM` stores `3 bits each for R, G, B`, for `512` possible colors.
- JVC’s technical manual states: the picture data is stored in VRAM and is "converted into an analog signal of Red, Green and Blue" at the game processor outputs.

### Reconstructed external chain

For FC1004-family Model 2:
1. 68000 writes VDP registers/VRAM/CRAM/VSRAM via the 68000 bus and VDP ports.
2. The VDP fetches next-line state and display data from external VRAM and internal CRAM/VSRAM.
3. The VDP performs tile/sprite/window/priority/shadow-highlight composition internally.
4. The VDP outputs analog `R/G/B`, sync, subcarrier, and related timing externally.
5. The encoder (`CXA1145`, `MB3514`, `KA2195`, or later `CXA1645`) converts that analog RGB + sync/subcarrier into composite and/or Y/C and buffers RGB to the AV connector.

For Toshiba-ASIC Model 2:
- Same broad pipeline, but the main ASIC is different and audio is not integrated; the video encoder stage remains downstream of analog `R/G/B` + sync/subcarrier.

### What leaves the ASIC digitally?

Confirmed:
- External buses exist for:
  - main CPU interface
  - Z80 interface
  - external VRAM interface
  - clocks/sync
- The service manuals do **not** document any external digital post-composition color bus.

Implication:
- The documented external digital buses are upstream control/memory buses, not a ready-made digital pixel output.

## Encoder and Analog Path on Model 2

Confirmed:
- Official `MB3514` datasheet: encoder accepts `analog RGB`, `composite sync`, and `subcarrier` and outputs composite/Y/C while also buffering analog RGB.
- Official `CXA1645` datasheet: same class of behavior; analog RGB + composite sync + subcarrier in, encoded video out.
- ConsoleMods RGB-bypass tracing on a VA1 board shows FC1004 pin `42` (`CSYNC`) going directly to encoder sync-in and to the multi-out.
- ConsoleMods jailbar documentation shows the VDP/ASIC subcarrier pin:
  - `315-5313`: pin `50`
  - `315-5487/5660/5700/5708`: pin `131`
  - `315-5685/5786`: pin `103`
  interferes with RGB quality unless isolated or rerouted, which is strong board-level evidence that the subcarrier path runs directly from ASIC to encoder on Model 2.

Voltage observations:
- Official encoder datasheets confirm a `+5 V` supply environment.
- `MB3514` specifies:
  - RGB input: `0 to 1.0 Vp-p`
  - subcarrier input: `0.3 to 0.7 Vp-p`
  - digital low: up to `0.8 V`
  - digital high: at least `2.0 V`
- I did not find a Sega document specifying the exact FC1004/Toshiba ASIC RGB output amplitude at the package pin, so that should be measured on real boards.

## Candidate Tap Points For a Future HDMI Mod

### 1. Analog RGB directly at the ASIC pins

Pins:
- FC1004 family: `35/36/37`
- Toshiba family: `107/108/109`

Why it matters:
- This is the cleanest documented point before encoder-induced composite/YC loss and before board-level RGB amplification artifacts downstream of the encoder.

Pros:
- Supported by official pin docs on FC1004 and strong community evidence on Toshiba ASICs.
- Avoids encoder coloration and some jailbar sources.

Cons:
- It is already analog. This is not a true digital-to-digital tap.
- Requires high-bandwidth ADC or analog front-end design.

Assessment:
- Best practical pre-encoder interception point.
- Not sufficient to satisfy a purist "before DAC loss" goal, because the DAC is already inside the ASIC.

### 2. Sync/timing taps

Signals:
- `EDCLK`, `CSYNC`, `HSYNC`, `VSYNC`, `MCLK`, `VCLK`, `YS`, `SBCR`

Why they matter:
- `EDCLK` is the most useful directly exposed pixel-related timing reference.
- `CSYNC/HSYNC/VSYNC` are mandatory for validating raster timing.
- `YS` may help if transparency/overlay behavior needs to be replicated or compared against 32X-style external compositing.
- `SBCR` is useful for verifying encoder feed and for identifying interference sources.

Assessment:
- Mandatory probe set for either analog capture or reconstruction-based work.

### 3. External VRAM interface taps

Signals:
- `SD0-SD7`, `RD0-RD7`, `AD0-AD7`, `RAS1`, `CAS1`, `OE1`, `WE0`, `WE1`, `SC`, `SE0`, `SE1`

Why they matter:
- They expose the VDP’s external memory activity and may allow deep timing analysis or partial reconstruction of fetch behavior.

Risks / limitations:
- They are upstream of color lookup, priority resolution, transparency handling, shadow/highlight, and final DAC.
- I found no evidence that these pins carry a simple post-compositor digital pixel stream.

Assessment:
- Valuable for reverse engineering.
- Poor candidate for a first practical HDMI capture implementation unless paired with substantial FPGA reconstruction logic and very careful hardware characterization.

### 4. CPU/VDP-port observation only

Signals:
- `VA*`, `VD*`, `AS`, `UDS`, `LDS`, `R/W`, `DTAK`, DMA/bus-arbitration signals

Assessment:
- Useful for software-side event capture and register-write timing.
- Insufficient alone for trustworthy pixel-perfect reconstruction because too much video behavior happens inside the VDP between writes and output.

## Bottom-Line Feasibility Assessment

Most likely feasible path:
- Pre-encoder analog RGB capture with external sync/dot-clock recovery, then digital processing/scaling to HDMI.

Most likely *not yet* feasible path from current evidence:
- A simple direct digital capture from an exposed post-compositor bus. I did not find one.

Possible but high-risk research path:
- Hybrid reconstruction using external VRAM-interface observation plus bus observation plus calibration against live analog RGB. This could reduce dependence on a high-speed ADC, but it needs substantial new reverse engineering.

## Open Questions

1. Exact retail-board mapping of rare FC1004 suffixes:
- `315-5660-01`
- `315-5708-01`
- `315-5700`

2. Toshiba-ASIC full pinout:
- Many pins remain listed as unknown/NC in the public reconstruction.

3. Exact electrical levels and bandwidth at the ASIC RGB pins:
- Need direct scope measurement on FC1004 and Toshiba boards.

4. `YS` behavior in all display modes:
- Transparency/blanking semantics are documented at a high level, but not characterized cycle-by-cycle for Model 2 ASICs in the sources reviewed here.

5. Whether the external VRAM serial interface can be exploited for line-based digital reconstruction:
- No source I found proves this either way.

## Recommended Validation Plan

### Hardware set
- At least one FC1004 board: ideally `VA1` or `VA1.8`
- One Toshiba `315-5786` board: `VA2` or early `VA2.3`
- One `315-5685` board if shadow/highlight compatibility matters

### Instruments
- 4-channel or larger mixed-signal scope, `>= 200 MHz` minimum; `500 MHz+` preferred for cleaner edge timing.
- High-impedance active probes for ASIC pins and encoder pins.
- Logic analyzer with at least `100 MHz` synchronous capture for bus work; more is better.
- Differential or low-capacitance probing for RGB if measuring directly at the ASIC.

### First measurements
1. Verify `MCLK`, `VCLK`, `EDCLK`, `HSYNC`, `VSYNC`, `CSYNC` frequencies on FC1004 and Toshiba boards in both `H32` and `H40`.
2. Probe ASIC `R/G/B` directly and compare against encoder input pins to see what passive network lies between them.
3. Probe `SBCR` at ASIC and encoder input to document routing and interference coupling.
4. Probe `YS` on the ASIC and cartridge port while running:
   - normal opaque scenes
   - shadow/highlight scenes
   - transparency-heavy scenes
   - 32X attached if future compatibility matters
5. Probe VRAM-control pins (`RAS1/CAS1/OE1/WE*`) together with `EDCLK` and `HSYNC` to build a line-by-line fetch timeline.

### Suggested software tests
- 240p Test Suite color bars and sync tests
- Custom ROMs that:
  - switch H32/H40
  - switch 50/60 Hz where supported
  - toggle shadow/highlight
  - perform mid-scanline register writes
  - exercise CRAM writes during display
  - use sprite-heavy and raster-effect scenes

## Source Notes

Primary / official:
- Sega service manual, Genesis II / Mega Drive II PAL No. 001, June 1993.
- Sega service manual, Genesis Model 2 VA1.
- Sega service manual, Genesis II VA3.
- JVC X'Eye technical manual.
- Sega Genesis Software Manual.
- Sega Technical Overview 1.00.
- Sega 32X Hardware Manual.
- Fujitsu `MB3514` datasheet.
- Sony `CXA1645` datasheet.

Community / secondary, used cautiously:
- ConsoleMods motherboard-differences matrix.
- ConsoleMods ASIC-information page.
- ConsoleMods Toshiba ASIC pinout page.
- ConsoleMods jailbar-fix and RGB-bypass tracing pages.
- Sega Retro hardware-revision page.
- Kabuto hardware notes mirror on Plutiedev.

## Source URLs

- Sega PAL Model 2 service manual: https://consolemods.org/wiki/images/b/b5/Sega_Genesis_Model_2_VA0_Service_Manual.pdf
- Sega Model 2 VA1 service manual: https://consolemods.org/wiki/images/9/9e/Sega_Genesis_Model_2_VA1_Service_Manual.pdf
- Sega Model 2 VA3 service manual OCR: https://gamesx.com/files/SEGA_Genesis_model2_VA3_Service_ManualOCR.pdf
- JVC X'Eye technical manual: https://consolemods.org/wiki/images/b/b0/JVC_X%27Eye_Technical_Manual.pdf
- Sega Genesis Software Manual: https://segaretro.org/images/9/95/GenesisSoftwareManual.pdf
- Sega Technical Overview: https://segaretro.org/images/1/18/GenesisTechnicalOverview.pdf
- Sega 32X Hardware Manual: https://consolemods.org/wiki/images/e/e9/32X_Hardware_Manual_1994_Sega_text.pdf
- Fujitsu MB3514 datasheet: https://www.smspower.org/uploads/Development/Fujitsu_MB3514.pdf
- Sony CXA1645 datasheet mirror: https://www.alldatasheet.com/datasheet-pdf/pdf/46646/SONY/CXA1645.html
- PAL Model 1 service manual with discrete `315-5313` pinout: https://consolemods.org/wiki/images/6/6b/PAL_Mega_Drive_Model_1_Service_Manual.pdf
- ConsoleMods motherboard differences: https://consolemods.org/wiki/Genesis:Motherboard_Differences
- ConsoleMods ASIC info: https://consolemods.org/wiki/Genesis:ASIC_Information
- ConsoleMods Toshiba pinout: https://consolemods.org/wiki/Genesis:Toshiba_ASIC_Pinout
- ConsoleMods 315-5685 info: https://consolemods.org/wiki/Genesis:315-5685_Information
- ConsoleMods Model 2 RGB bypass: https://consolemods.org/wiki/Genesis:Model_2_RGB_Bypass
- ConsoleMods jailbar fix: https://consolemods.org/wiki/Genesis:Jailbar_Fix
- ConsoleMods connector pinouts: https://consolemods.org/wiki/Genesis:Connector_Pinouts
- Sega Retro hardware revisions: https://segaretro.org/Sega_Mega_Drive_PCB_revisions
- Kabuto hardware notes mirror: https://plutiedev.com/mirror/kabuto-hardware-notes
