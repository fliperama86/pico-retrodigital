/**
 * Pico RetroDigital SNES Pin Configuration
 *
 * Target-specific pins for SHVC/SNES digital video and audio capture.
 */

#ifndef SNES_PINS_H
#define SNES_PINS_H

// =============================================================================
// SNES Video Input Pins - Pico RetroDigital PCB layout
// =============================================================================
// Captures RGB555 + VBLANK + HBLANK from the SNES PPU2.
//
// The FPC J2 pin numbers are reversed at the main-board mating connector:
// FPC pin 32 mates with main-board pin 1, through FPC pin 1 mating with
// main-board pin 32.
//
// Pin mapping (LSB to MSB in captured word):
//   Bits 0-4:   GP33-37 (Red R0-R4)
//   Bits 5-9:   GP38-42 (Green G0-G4)
//   Bits 10-14: GP43-47 (Blue B0-B4)
//
// With PIO GPIOBASE=16, HBLANK/VBLANK/PCLK are GPIO indices 8/9/12.

#define PIN_SNES_HBLANK 24
#define PIN_SNES_VBLANK 25
#define PIN_SNES_PIXEL_VALID 26 // Not used until separately hardware-validated.
#define PIN_SNES_PCLK 28
#define PIN_SNES_BASE 33

#define PIN_SNES_R0 33
#define PIN_SNES_R1 34
#define PIN_SNES_R2 35
#define PIN_SNES_R3 36
#define PIN_SNES_R4 37

#define PIN_SNES_G0 38
#define PIN_SNES_G1 39
#define PIN_SNES_G2 40
#define PIN_SNES_G3 41
#define PIN_SNES_G4 42

#define PIN_SNES_B0 43
#define PIN_SNES_B1 44
#define PIN_SNES_B2 45
#define PIN_SNES_B3 46
#define PIN_SNES_B4 47

#define SNES_CAPTURE_PIN_LAST PIN_SNES_B4
#define SNES_CAPTURE_BITS 15

// =============================================================================
// SNES S-DSP Audio Input Pins
// =============================================================================
// These match the existing NeoPico audio capture GPIOs, but the source is the
// SNES S-DSP digital interface:
//   DSP44 SDATA -> GPIO 0
//   DSP43 LRCK  -> GPIO 1
//   DSP42 BCLK  -> GPIO 2

#define PIN_I2S_DAT 0
#define PIN_I2S_WS 1
#define PIN_I2S_BCK 2

// This PCB has no dedicated direct-input OSD buttons in the SNES capture pin
// group. These aliases must not be enabled for SNES builds.
#define PIN_OSD_BTN_MENU PIN_SNES_HBLANK
#define PIN_OSD_BTN_BACK PIN_SNES_VBLANK

#endif // SNES_PINS_H
