/**
 * Standalone SNES RGB frame capture over USB CDC.
 *
 * This target deliberately initializes no HSTX, HDMI, audio, settings, OSD,
 * multicore code, or overclock. Send ASCII C over the CDC port to request one
 * 256x224 RGB565 frame.
 */

#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bsp/board_api.h"
#include "snes_pins.h"
#include "tusb.h"
#include "video_capture_snes.pio.h"

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The SNES USB capture protocol requires a little-endian target"
#endif

#define CAPTURE_WIDTH 256u
#define CAPTURE_HEIGHT 224u
#define CAPTURE_BITS 15u
#define CAPTURE_PIO_TARGET_HZ 126000000u

#define MASK_WORDS_PER_LINE (CAPTURE_WIDTH / 32u)
#define MASK_TOTAL_WORDS (CAPTURE_HEIGHT * MASK_WORDS_PER_LINE)
#define MASK_PLANE_BYTES (MASK_TOTAL_WORDS * sizeof(uint32_t))
#define BRIGHTNESS_ARRAY_BYTES (CAPTURE_HEIGHT * sizeof(uint8_t))
#define TRAILER_CRC_BYTES 4u
#define FRAME_TRAILER_BYTES (BRIGHTNESS_ARRAY_BYTES + MASK_PLANE_BYTES + TRAILER_CRC_BYTES)

#define FRAME_MAGIC "PRDF"
#define ERROR_MAGIC "PRDE"
#define FRAME_PROTOCOL_VERSION 3u
#define FRAME_PIXEL_FORMAT_RGB565_LE 1u
// PRDF header layout, little-endian (28 bytes, unchanged since version 2):
//   0  char[4] magic          "PRDF"
//   4  u16     version        FRAME_PROTOCOL_VERSION
//   6  u16     pixel_format   FRAME_PIXEL_FORMAT_RGB565_LE
//   8  u16     width          CAPTURE_WIDTH
//  10  u16     height         CAPTURE_HEIGHT
//  12  u32     payload_bytes  FRAME_PAYLOAD_BYTES
//  16  u32     source_frame   captured_frames
//  20  u32     payload_crc32  CRC-32 of the RGB payload only
//  24  u8      brightness     CMOD nibble sampled right after DMA completes
//  25  u8      flags          bit0 = PIXEL_VALID level at the same moment
//                              bit1 = gating was enabled for this frame's
//                                     RGB payload (see the 'G' command)
//  26  u16     reserved       0
// Version 3 appends a fixed trailer after the RGB565 payload; payload_bytes
// and payload_crc32 keep their version 2 meaning (RGB payload only). The
// trailer is not sized in the header, its layout is implied by the version:
//   +0     u8[224]   per-line brightness, CMOD nibble in the low nibble
//   +224   u32[1792] mask plane, the mask DMA words verbatim (LSB-first
//                     pixels within each word; see video_capture_snes.pio)
//   +7392  u32       CRC-32 of the 7392 appended bytes above
// Version 1 was the original 24-byte header with no brightness/flags/
// reserved fields and no trailer.
#define FRAME_HEADER_BYTES 28u
#define ERROR_HEADER_BYTES 32u
#define FRAME_PAYLOAD_BYTES (CAPTURE_WIDTH * CAPTURE_HEIGHT * sizeof(uint16_t))
#define SIGNAL_SAMPLE_US 50000u
// 'B' status command: replies immediately (no frame capture) with a
// fixed-size PRDB packet, little-endian:
//   0  char[4] magic                    "PRDB"
//   4  u16     version                  FRAME_PROTOCOL_VERSION
//   6  u8      brightness               CMOD nibble sampled now (0-15)
//   7  u8      pixel_valid              PIXEL_VALID level sampled now
//   8  u16     reserved                 0
//  10  u32     pixel_valid_transitions  GP26 transition count over the
//                                       following SIGNAL_SAMPLE_US window
//  14  u8      brightness_after         CMOD nibble sampled after that window
//  15  u8      reserved                 0
#define STATUS_MAGIC "PRDB"
#define STATUS_HEADER_BYTES 16u
// Worst case for the VBLANK arm-wait is about one frame period (~16.6 ms);
// worst case for the multi-SM capture wait is about 14.3 ms. Both reuse this
// constant, each with a comfortable margin over its own worst case.
#define FRAME_TIMEOUT_US 100000u

enum capture_error {
    CAPTURE_ERROR_FRAME_TIMEOUT = 5,
};

enum signal_bit {
    SIGNAL_HBLANK = 1u << 0u,
    SIGNAL_VBLANK = 1u << 1u,
    SIGNAL_PCLK = 1u << 2u,
};

typedef struct {
    uint32_t levels;
    uint32_t hblank_transitions;
    uint32_t vblank_transitions;
    uint32_t pclk_transitions;
} signal_stats_t;

static PIO capture_pio = pio1;
static uint capture_sm;
static uint capture_program_offset;
static int capture_dma_channel;
static pio_sm_config capture_config;

static uint cmod_sm;
static uint cmod_program_offset;
static int cmod_dma_channel;
static pio_sm_config cmod_config;

static PIO mask_pio = pio2;
static uint mask_sm;
static uint mask_program_offset;
static int mask_dma_channel;
static pio_sm_config mask_config;

static uint32_t raw_frame[CAPTURE_HEIGHT * CAPTURE_WIDTH];
static uint32_t cmod_lines[CAPTURE_HEIGHT];
static uint32_t mask_words[MASK_TOTAL_WORDS];
static uint16_t frame_buffer[CAPTURE_HEIGHT][CAPTURE_WIDTH] __attribute__((aligned(4)));
static uint16_t pixel_lut[32768] __attribute__((aligned(4)));
static uint8_t scale5[16][32];
static uint8_t tx_header[ERROR_HEADER_BYTES];
static uint8_t frame_trailer[FRAME_TRAILER_BYTES];

static uint32_t captured_frames;
static bool capture_requested;
static bool status_requested;
// PIXEL_VALID gating: off by default, toggled by the 'G' command. Reflected
// in the frame header's flags bit1 (see the layout comment above). Off by
// default because /OVER is pre-priority: PPU1 asserts it for the Mode 7
// off-map condition before the sprite/BG mux runs, so it stays asserted
// under sprites or text that win priority over an off-map background (for
// example, the Pilotwings lesson overlay), and it does not distinguish the
// $211A tile-0-fill submode, where the correct output is tile 0's graphic,
// not black. Gating on that raw signal therefore erases legitimate content.
// The proper fix is hardware (a VDB-bus gate on a future QSB rev); until
// then this toggle exists for demos/diagnostics only, not as a default.
static bool gating_enabled = false;
static uint32_t tx_offset;
static uint32_t tx_header_bytes;
static uint32_t tx_payload_bytes;
static uint32_t tx_trailer_bytes;

static void pixel_lut_init(void)
{
    for (uint32_t index = 0; index < 32768u; ++index) {
        const uint32_t red = index & 0x1fu;
        const uint32_t green = (index >> 5u) & 0x1fu;
        const uint32_t blue = (index >> 10u) & 0x1fu;
        pixel_lut[index] = (uint16_t)((red << 11u) | (green << 6u) | ((green >> 4u) << 5u) | blue);
    }
}

// scale5[b][c] scales a 5-bit color channel c by brightness nibble b
// (0-15), so scale5[15][c] == c (unchanged) and scale5[0][c] == 0 (black).
static void scale5_init(void)
{
    for (uint32_t brightness = 0; brightness < 16u; ++brightness) {
        for (uint32_t channel = 0; channel < 32u; ++channel) {
            scale5[brightness][channel] = (uint8_t)((channel * brightness) / 15u);
        }
    }
}

// PIXEL_VALID gating is applied here, not as a separate pass: the mask DMA
// buffer is already complete by the time convert_frame() runs (it finishes
// with the same wait_for_captures() call that waits for the RGB and
// brightness DMAs), so each pixel's mask bit is a straight bit-test,
// LSB-first within its u32 word, the same ordering as the wire format.
static void convert_frame(void)
{
    const bool gate = gating_enabled;
    for (uint32_t line = 0; line < CAPTURE_HEIGHT; ++line) {
        const uint32_t brightness = cmod_lines[line] & 0xfu;
        const uint32_t *source = &raw_frame[line * CAPTURE_WIDTH];
        const uint32_t *mask = &mask_words[line * MASK_WORDS_PER_LINE];
        uint16_t *destination = frame_buffer[line];

        if (brightness == 15u) {
            // Fast path: full brightness leaves colors unscaled.
            for (uint32_t pixel = 0; pixel < CAPTURE_WIDTH; ++pixel) {
                if (gate && ((mask[pixel >> 5u] >> (pixel & 31u)) & 1u) == 0u) {
                    destination[pixel] = 0x0000u;
                } else {
                    destination[pixel] = pixel_lut[source[pixel] & 0x7fffu];
                }
            }
        } else if (brightness == 0u) {
            // Fast path: zero brightness is black regardless of color, so
            // gating cannot change anything on this line.
            memset(destination, 0, CAPTURE_WIDTH * sizeof(uint16_t));
        } else {
            for (uint32_t pixel = 0; pixel < CAPTURE_WIDTH; ++pixel) {
                if (gate && ((mask[pixel >> 5u] >> (pixel & 31u)) & 1u) == 0u) {
                    destination[pixel] = 0x0000u;
                    continue;
                }
                const uint32_t raw = source[pixel] & 0x7fffu;
                const uint32_t red = raw & 0x1fu;
                const uint32_t green = (raw >> 5u) & 0x1fu;
                const uint32_t blue = (raw >> 10u) & 0x1fu;
                const uint32_t scaled =
                    scale5[brightness][red] | (scale5[brightness][green] << 5u) | (scale5[brightness][blue] << 10u);
                destination[pixel] = pixel_lut[scaled];
            }
        }
    }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    while (length-- > 0u) {
        crc ^= *data++;
        for (uint32_t bit = 0; bit < 8u; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xedb88320u & mask);
        }
    }
    return crc;
}

static uint32_t frame_crc32(void)
{
    const uint32_t crc = crc32_update(UINT32_MAX, (const uint8_t *)frame_buffer, FRAME_PAYLOAD_BYTES);
    return crc ^ UINT32_MAX;
}

static void write_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
}

static void write_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static void frame_header_prepare(uint32_t brightness, bool pixel_valid)
{
    memcpy(tx_header, FRAME_MAGIC, 4);
    write_u16_le(&tx_header[4], FRAME_PROTOCOL_VERSION);
    write_u16_le(&tx_header[6], FRAME_PIXEL_FORMAT_RGB565_LE);
    write_u16_le(&tx_header[8], CAPTURE_WIDTH);
    write_u16_le(&tx_header[10], CAPTURE_HEIGHT);
    write_u32_le(&tx_header[12], FRAME_PAYLOAD_BYTES);
    write_u32_le(&tx_header[16], captured_frames);
    write_u32_le(&tx_header[20], frame_crc32());
    tx_header[24] = (uint8_t)brightness;
    tx_header[25] = (pixel_valid ? 0x1u : 0u) | (gating_enabled ? 0x2u : 0u);
    write_u16_le(&tx_header[26], 0);
    tx_offset = 0;
    tx_header_bytes = FRAME_HEADER_BYTES;
    tx_payload_bytes = FRAME_PAYLOAD_BYTES;
}

// Builds the version 3 trailer (per-line brightness array + mask plane +
// its own CRC-32) into frame_trailer and arms it for usb_tx_task(). Called
// alongside frame_header_prepare() on a successful capture; tx_offset is
// reset once by frame_header_prepare(), not here.
static void trailer_prepare(void)
{
    for (uint32_t line = 0; line < CAPTURE_HEIGHT; ++line) {
        frame_trailer[line] = (uint8_t)(cmod_lines[line] & 0xfu);
    }
    memcpy(&frame_trailer[BRIGHTNESS_ARRAY_BYTES], mask_words, MASK_PLANE_BYTES);
    const uint32_t crc =
        crc32_update(UINT32_MAX, frame_trailer, BRIGHTNESS_ARRAY_BYTES + MASK_PLANE_BYTES) ^ UINT32_MAX;
    write_u32_le(&frame_trailer[BRIGHTNESS_ARRAY_BYTES + MASK_PLANE_BYTES], crc);
    tx_trailer_bytes = FRAME_TRAILER_BYTES;
}

static uint32_t signal_levels(void)
{
    uint32_t levels = 0;
    if (gpio_get(PIN_SNES_HBLANK)) {
        levels |= SIGNAL_HBLANK;
    }
    if (gpio_get(PIN_SNES_VBLANK)) {
        levels |= SIGNAL_VBLANK;
    }
    if (gpio_get(PIN_SNES_PCLK)) {
        levels |= SIGNAL_PCLK;
    }
    return levels;
}

static uint32_t read_brightness_nibble(void)
{
    uint32_t brightness = 0;
    brightness |= gpio_get(PIN_SNES_CMOD0) ? 0x1u : 0u;
    brightness |= gpio_get(PIN_SNES_CMOD1) ? 0x2u : 0u;
    brightness |= gpio_get(PIN_SNES_CMOD2) ? 0x4u : 0u;
    brightness |= gpio_get(PIN_SNES_CMOD3) ? 0x8u : 0u;
    return brightness;
}

static signal_stats_t signal_stats_sample(void)
{
    signal_stats_t stats = {.levels = signal_levels()};
    uint32_t previous = stats.levels;
    const uint32_t started = time_us_32();
    while ((uint32_t)(time_us_32() - started) < SIGNAL_SAMPLE_US) {
        const uint32_t current = signal_levels();
        const uint32_t changed = current ^ previous;
        stats.hblank_transitions += (changed & SIGNAL_HBLANK) != 0u;
        stats.vblank_transitions += (changed & SIGNAL_VBLANK) != 0u;
        stats.pclk_transitions += (changed & SIGNAL_PCLK) != 0u;
        previous = current;
    }
    stats.levels = previous;
    return stats;
}

// Sibling of signal_stats_sample() dedicated to the 'B' status command: only
// PIXEL_VALID is relevant there, and the error-path signal_stats_t layout
// (used by capture_error_prepare()) must stay untouched, so this counts
// GP26 transitions on its own rather than growing that struct.
static uint32_t pixel_valid_transitions_sample(void)
{
    uint32_t transitions = 0;
    bool previous = gpio_get(PIN_SNES_PIXEL_VALID);
    const uint32_t started = time_us_32();
    while ((uint32_t)(time_us_32() - started) < SIGNAL_SAMPLE_US) {
        const bool current = gpio_get(PIN_SNES_PIXEL_VALID);
        if (current != previous) {
            transitions++;
        }
        previous = current;
    }
    return transitions;
}

static void capture_error_prepare(enum capture_error error, const signal_stats_t *stats, uint32_t line)
{
    memset(tx_header, 0, sizeof(tx_header));
    memcpy(tx_header, ERROR_MAGIC, 4);
    write_u16_le(&tx_header[4], FRAME_PROTOCOL_VERSION);
    write_u16_le(&tx_header[6], (uint16_t)error);
    write_u32_le(&tx_header[8], stats->levels);
    write_u32_le(&tx_header[12], stats->hblank_transitions);
    write_u32_le(&tx_header[16], stats->vblank_transitions);
    write_u32_le(&tx_header[20], stats->pclk_transitions);
    write_u32_le(&tx_header[24], line);
    tx_offset = 0;
    tx_header_bytes = ERROR_HEADER_BYTES;
    tx_payload_bytes = 0;
    tx_trailer_bytes = 0;
}

static void status_prepare(void)
{
    // Sample brightness/PIXEL_VALID before AND after the 50ms transition
    // window so the reply shows whether the latch moved across the window,
    // in addition to the live transition count.
    const uint32_t brightness_before = read_brightness_nibble();
    const bool pixel_valid_before = gpio_get(PIN_SNES_PIXEL_VALID);
    const uint32_t transitions = pixel_valid_transitions_sample();
    const uint32_t brightness_after = read_brightness_nibble();

    memset(tx_header, 0, STATUS_HEADER_BYTES);
    memcpy(tx_header, STATUS_MAGIC, 4);
    write_u16_le(&tx_header[4], FRAME_PROTOCOL_VERSION);
    tx_header[6] = (uint8_t)brightness_before;
    tx_header[7] = pixel_valid_before ? 1u : 0u;
    write_u16_le(&tx_header[8], 0);
    write_u32_le(&tx_header[10], transitions);
    tx_header[14] = (uint8_t)brightness_after;
    tx_header[15] = 0;
    tx_offset = 0;
    tx_header_bytes = STATUS_HEADER_BYTES;
    tx_payload_bytes = 0;
    tx_trailer_bytes = 0;
}

// The SM free-runs after DMA completes (it keeps producing words past the
// requested frame height until the CPU resets it here), so it is typically
// stalled mid-push with residual bits in the ISR by the time the next
// capture starts. pio_sm_restart() clears the ISR/OSR shift state and stall
// latches so a stale word cannot inject a one-pixel (or one-line) shift; it
// preserves X/Y, so the capture and mask SMs keep their
// (CAPTURE_WIDTH - 1) loaded at init. The jmp uses the pioasm-generated
// wrap_target constant rather than a hardcoded offset so it stays correct
// if a program layout changes.
static void reset_sm(PIO pio, uint sm, uint program_offset, uint wrap_target)
{
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(program_offset + wrap_target));
    pio_sm_set_enabled(pio, sm, true);
}

static void capture_reset(void)
{
    reset_sm(capture_pio, capture_sm, capture_program_offset, snes_hard_sync_wrap_target);
    reset_sm(capture_pio, cmod_sm, cmod_program_offset, snes_cmod_wrap_target);
    reset_sm(mask_pio, mask_sm, mask_program_offset, snes_pixel_valid_mask_wrap_target);
}

static void capture_init(void)
{
    pixel_lut_init();
    scale5_init();

    // pio1 hosts capture (RGB555, irq 4) and cmod (brightness, irq 5).
    // GPIOBASE=16 reaches GP33-47 (RGB) and GP29-32 (CMOD); both ranges fall
    // within the 16-47 window pio1 addresses with this base.
    hard_assert(pio_set_gpio_base(capture_pio, 16u) == PICO_OK);
    pio_clear_instruction_memory(capture_pio);

    const int program_offset = pio_add_program(capture_pio, &snes_hard_sync_program);
    hard_assert(program_offset >= 0);
    capture_program_offset = (uint)program_offset;
    capture_sm = (uint)pio_claim_unused_sm(capture_pio, true);

    const int cmod_offset = pio_add_program(capture_pio, &snes_cmod_program);
    hard_assert(cmod_offset >= 0);
    cmod_program_offset = (uint)cmod_offset;
    cmod_sm = (uint)pio_claim_unused_sm(capture_pio, true);

    // pio2 hosts the PIXEL_VALID mask (irq 4, this block's own flag).
    // HBLANK/VBLANK/PCLK/PIXEL_VALID are all below GPIO 32, so GPIOBASE=0
    // needs no relocation there.
    hard_assert(pio_set_gpio_base(mask_pio, 0u) == PICO_OK);
    pio_clear_instruction_memory(mask_pio);

    const int mask_offset = pio_add_program(mask_pio, &snes_pixel_valid_mask_program);
    hard_assert(mask_offset >= 0);
    mask_program_offset = (uint)mask_offset;
    mask_sm = (uint)pio_claim_unused_sm(mask_pio, true);

    const uint sync_pins[] = {PIN_SNES_HBLANK, PIN_SNES_VBLANK, PIN_SNES_PCLK};
    for (size_t index = 0; index < count_of(sync_pins); ++index) {
        const uint pin = sync_pins[index];
        pio_gpio_init(capture_pio, pin);
        gpio_disable_pulls(pin);
        gpio_set_input_enabled(pin, true);
        gpio_set_input_hysteresis_enabled(pin, true);
    }
    for (uint pin = PIN_SNES_BASE; pin <= SNES_CAPTURE_PIN_LAST; ++pin) {
        pio_gpio_init(capture_pio, pin);
        gpio_disable_pulls(pin);
        gpio_set_input_enabled(pin, true);
        gpio_set_input_hysteresis_enabled(pin, true);
    }

    // PIXEL_VALID and the CMOD brightness nibble are plain SIO reads AND
    // now also PIO "in"/"wait gpio" sources for the mask and cmod programs.
    // Do not call pio_gpio_init() for them, or for pio2's use of HBLANK/
    // VBLANK/PCLK above: PIO WAIT GPIO and IN instructions read the pad
    // input regardless of a pin's funcsel, so leaving funcsel at SIO here
    // keeps gpio_get() working for the header sample and the B command
    // while the mask and cmod SMs read the same pads independently, and
    // pio2 needs no separate routing for HBLANK/VBLANK/PCLK either.
    const uint telemetry_pins[] = {PIN_SNES_PIXEL_VALID, PIN_SNES_CMOD0, PIN_SNES_CMOD1, PIN_SNES_CMOD2,
                                   PIN_SNES_CMOD3};
    for (size_t index = 0; index < count_of(telemetry_pins); ++index) {
        const uint pin = telemetry_pins[index];
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_input_enabled(pin, true);
        gpio_set_input_hysteresis_enabled(pin, true);
    }

    capture_config = snes_hard_sync_program_get_default_config(capture_program_offset);
    sm_config_set_clkdiv(&capture_config, (float)clock_get_hz(clk_sys) / (float)CAPTURE_PIO_TARGET_HZ);
    sm_config_set_in_pins(&capture_config, PIN_SNES_BASE);
    sm_config_set_in_pin_count(&capture_config, CAPTURE_BITS);
    sm_config_set_in_shift(&capture_config, false, true, CAPTURE_BITS);
    hard_assert(pio_sm_init(capture_pio, capture_sm, capture_program_offset, &capture_config) == PICO_OK);
    pio_sm_set_enabled(capture_pio, capture_sm, true);
    pio_sm_put_blocking(capture_pio, capture_sm, CAPTURE_WIDTH - 1u);

    cmod_config = snes_cmod_program_get_default_config(cmod_program_offset);
    sm_config_set_clkdiv(&cmod_config, (float)clock_get_hz(clk_sys) / (float)CAPTURE_PIO_TARGET_HZ);
    sm_config_set_in_pins(&cmod_config, PIN_SNES_CMOD0);
    sm_config_set_in_pin_count(&cmod_config, 4u);
    sm_config_set_in_shift(&cmod_config, false, true, 4u);
    hard_assert(pio_sm_init(capture_pio, cmod_sm, cmod_program_offset, &cmod_config) == PICO_OK);
    pio_sm_set_enabled(capture_pio, cmod_sm, true);

    mask_config = snes_pixel_valid_mask_program_get_default_config(mask_program_offset);
    sm_config_set_clkdiv(&mask_config, (float)clock_get_hz(clk_sys) / (float)CAPTURE_PIO_TARGET_HZ);
    sm_config_set_in_pins(&mask_config, PIN_SNES_PIXEL_VALID);
    sm_config_set_in_pin_count(&mask_config, 1u);
    sm_config_set_in_shift(&mask_config, true, true, 32u);
    hard_assert(pio_sm_init(mask_pio, mask_sm, mask_program_offset, &mask_config) == PICO_OK);
    pio_sm_set_enabled(mask_pio, mask_sm, true);
    pio_sm_put_blocking(mask_pio, mask_sm, CAPTURE_WIDTH - 1u);

    capture_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config dma_config = dma_channel_get_default_config((uint)capture_dma_channel);
    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, true);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_dreq(&dma_config, pio_get_dreq(capture_pio, capture_sm, false));
    channel_config_set_high_priority(&dma_config, true);
    dma_channel_configure((uint)capture_dma_channel, &dma_config, raw_frame, &capture_pio->rxf[capture_sm],
                          CAPTURE_HEIGHT * CAPTURE_WIDTH, false);

    cmod_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config cmod_dma_config = dma_channel_get_default_config((uint)cmod_dma_channel);
    channel_config_set_read_increment(&cmod_dma_config, false);
    channel_config_set_write_increment(&cmod_dma_config, true);
    channel_config_set_transfer_data_size(&cmod_dma_config, DMA_SIZE_32);
    channel_config_set_dreq(&cmod_dma_config, pio_get_dreq(capture_pio, cmod_sm, false));
    dma_channel_configure((uint)cmod_dma_channel, &cmod_dma_config, cmod_lines, &capture_pio->rxf[cmod_sm],
                          CAPTURE_HEIGHT, false);

    mask_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config mask_dma_config = dma_channel_get_default_config((uint)mask_dma_channel);
    channel_config_set_read_increment(&mask_dma_config, false);
    channel_config_set_write_increment(&mask_dma_config, true);
    channel_config_set_transfer_data_size(&mask_dma_config, DMA_SIZE_32);
    channel_config_set_dreq(&mask_dma_config, pio_get_dreq(mask_pio, mask_sm, false));
    dma_channel_configure((uint)mask_dma_channel, &mask_dma_config, mask_words, &mask_pio->rxf[mask_sm],
                          MASK_TOTAL_WORDS, false);
}

static bool wait_for_captures(uint32_t timeout_us)
{
    const absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (dma_channel_is_busy((uint)capture_dma_channel) || dma_channel_is_busy((uint)cmod_dma_channel) ||
           dma_channel_is_busy((uint)mask_dma_channel)) {
        if (time_reached(deadline)) {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

static void capture_frame(void)
{
    // Arming race fix: the three SMs align to the same physical VBLANK
    // falling edge only if all are armed before it. Waiting here for VBLANK
    // to read low means the reset+arm+trigger sequence below (several
    // individual register writes across two PIO blocks) starts right after
    // a VBLANK fall, giving it a full frame (~16 ms) of margin before the
    // next one. Without this, the one-in-ten-thousand chance that the edge
    // lands mid-arming would let some SMs catch this frame while others,
    // armed a moment later, catch the next one, misaligning the RGB,
    // brightness, and mask captures by a frame.
    const absolute_time_t arm_deadline = make_timeout_time_us(FRAME_TIMEOUT_US);
    while (gpio_get(PIN_SNES_VBLANK)) {
        if (time_reached(arm_deadline)) {
            const signal_stats_t stats = signal_stats_sample();
            capture_error_prepare(CAPTURE_ERROR_FRAME_TIMEOUT, &stats, 0);
            return;
        }
        tight_loop_contents();
    }

    capture_reset();

    dma_channel_set_trans_count((uint)capture_dma_channel, CAPTURE_HEIGHT * CAPTURE_WIDTH, false);
    dma_channel_set_write_addr((uint)capture_dma_channel, raw_frame, true);
    dma_channel_set_trans_count((uint)cmod_dma_channel, CAPTURE_HEIGHT, false);
    dma_channel_set_write_addr((uint)cmod_dma_channel, cmod_lines, true);
    dma_channel_set_trans_count((uint)mask_dma_channel, MASK_TOTAL_WORDS, false);
    dma_channel_set_write_addr((uint)mask_dma_channel, mask_words, true);

    pio_interrupt_clear(capture_pio, 4u);
    pio_sm_exec(capture_pio, capture_sm, pio_encode_irq_set(false, 4u));
    pio_interrupt_clear(capture_pio, 5u);
    pio_sm_exec(capture_pio, cmod_sm, pio_encode_irq_set(false, 5u));
    pio_interrupt_clear(mask_pio, 4u);
    pio_sm_exec(mask_pio, mask_sm, pio_encode_irq_set(false, 4u));

    if (!wait_for_captures(FRAME_TIMEOUT_US)) {
        dma_channel_abort((uint)capture_dma_channel);
        dma_channel_abort((uint)cmod_dma_channel);
        dma_channel_abort((uint)mask_dma_channel);
        pio_sm_set_enabled(capture_pio, capture_sm, false);
        pio_sm_set_enabled(capture_pio, cmod_sm, false);
        pio_sm_set_enabled(mask_pio, mask_sm, false);

        // Stats sampling takes 50ms; only pay for it while preparing an
        // error, never on the happy path.
        const signal_stats_t stats = signal_stats_sample();
        const uint32_t remaining = dma_channel_hw_addr((uint)capture_dma_channel)->transfer_count;
        const uint32_t lines_done = (CAPTURE_HEIGHT * CAPTURE_WIDTH - remaining) / CAPTURE_WIDTH;
        capture_error_prepare(CAPTURE_ERROR_FRAME_TIMEOUT, &stats, lines_done);
        return;
    }

    // Sample brightness and PIXEL_VALID right after DMA completes, so the
    // header's frame-level telemetry corresponds to the frame just
    // captured. The per-line brightness array in the trailer comes from
    // cmod_lines instead, sampled by the PIO at each line's HBLANK edge.
    const uint32_t brightness = read_brightness_nibble();
    const bool pixel_valid = gpio_get(PIN_SNES_PIXEL_VALID);

    convert_frame();
    trailer_prepare();
    captured_frames++;
    frame_header_prepare(brightness, pixel_valid);
}

static void usb_rx_task(void)
{
    while (tud_cdc_available() > 0u) {
        const int command = tud_cdc_read_char();
        if (command == 'C' || command == 'c') {
            capture_requested = true;
        } else if (command == 'B' || command == 'b') {
            status_requested = true;
        } else if (command == 'G' || command == 'g') {
            // No reply packet; the new state shows up in the next frame's
            // header flags bit1.
            gating_enabled = !gating_enabled;
        }
    }
}

static void usb_tx_task(void)
{
    const uint32_t tx_bytes = tx_header_bytes + tx_payload_bytes + tx_trailer_bytes;
    if (tx_bytes == 0u || !tud_cdc_connected()) {
        return;
    }

    uint32_t available = tud_cdc_write_available();
    if (available == 0u) {
        return;
    }

    const uint8_t *source;
    uint32_t remaining;
    if (tx_offset < tx_header_bytes) {
        source = &tx_header[tx_offset];
        remaining = tx_header_bytes - tx_offset;
    } else if (tx_offset < tx_header_bytes + tx_payload_bytes) {
        const uint32_t payload_offset = tx_offset - tx_header_bytes;
        source = &((const uint8_t *)frame_buffer)[payload_offset];
        remaining = tx_payload_bytes - payload_offset;
    } else {
        const uint32_t trailer_offset = tx_offset - tx_header_bytes - tx_payload_bytes;
        source = &frame_trailer[trailer_offset];
        remaining = tx_trailer_bytes - trailer_offset;
    }
    if (remaining > available) {
        remaining = available;
    }

    tx_offset += tud_cdc_write(source, remaining);
    tud_cdc_write_flush();
    if (tx_offset == tx_bytes) {
        tx_offset = 0;
        tx_header_bytes = 0;
        tx_payload_bytes = 0;
        tx_trailer_bytes = 0;
    }
}

int main(void)
{
    board_init();
    set_sys_clock_khz(CAPTURE_PIO_TARGET_HZ / 1000u, true);
    capture_init();

    tud_init(BOARD_TUD_RHPORT);
    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    while (true) {
        tud_task();

        if (!tud_cdc_connected()) {
            capture_requested = false;
            status_requested = false;
            tx_offset = 0;
            tx_header_bytes = 0;
            tx_payload_bytes = 0;
            tx_trailer_bytes = 0;
            continue;
        }

        usb_rx_task();
        usb_tx_task();

        if (capture_requested && tx_header_bytes == 0u && tx_payload_bytes == 0u) {
            capture_requested = false;
            capture_frame();
        }

        // Same gating as a capture request: never disturb a reply already
        // in flight (frame or status).
        if (status_requested && tx_header_bytes == 0u && tx_payload_bytes == 0u) {
            status_requested = false;
            status_prepare();
        }
    }
}
