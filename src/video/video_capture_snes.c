/**
 * SNES / SuperPico Video Capture Module
 *
 * Target-specific capture backend for SHVC/SNES PPU2 digital RGB.
 * The shared HDMI/audio/output pipeline remains NeoPico-HD's runtime path.
 */

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "video_capture.h"
#if NEOPICO_EXP_GENLOCK_DYNAMIC
#include "hardware/timer.h"
#endif
#include "pico/stdlib.h"
#if PICO_RETRODIGITAL_SNES_USB_CAPTURE
#include "pico/stdio_usb.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "capture_profile.h"
#include "line_ring.h"
#include "pico.h"
#include "snes_pins.h"
#include "tusb.h"
#include "video_capture_snes.pio.h"

#ifndef NEOPICO_SNES_CAPTURE_WARMUP_FRAMES
#define NEOPICO_SNES_CAPTURE_WARMUP_FRAMES 60
#endif

#ifndef PICO_RETRODIGITAL_SNES_USB_CAPTURE
#define PICO_RETRODIGITAL_SNES_USB_CAPTURE 0
#endif

// PIXEL_VALID gating is compiled out by default: /OVER is pre-priority
// (PPU1 flags the Mode 7 off-map condition before the sprite/BG mux runs),
// so it stays asserted under sprites or text that legitimately win priority
// over an off-map background (for example, the Pilotwings lesson overlay),
// and it does not distinguish the $211A tile-0-fill submode, where the
// correct output is tile 0's graphic, not black. Gating on that raw signal
// therefore erases legitimate content. The proper fix is hardware (a
// VDB-bus gate on a future QSB rev); until then this stays off in the
// product path and exists only for demos/diagnostics builds that opt in.
// The mask SM, its DMA channel, and its buffer keep running unconditionally
// regardless of this flag: that capture is cheap and stays ready either way.
#ifndef NEOPICO_SNES_PIXEL_VALID_GATE
#define NEOPICO_SNES_PIXEL_VALID_GATE 0
#endif

// SNES source timing.
#define SNES_H_TOTAL 341
#define SNES_CAPTURE_PIO_TARGET_HZ 126000000U

// PIXEL_VALID mask plane: one bit per pixel, LSB-first within each u32 word
// (see video_capture_snes.pio). CAPTURE_ACTIVE_WIDTH is a multiple of 32.
#define MASK_WORDS_PER_LINE (CAPTURE_ACTIVE_WIDTH / 32U)
#define MASK_TOTAL_WORDS (CAPTURE_ACTIVE_HEIGHT * MASK_WORDS_PER_LINE)

// =============================================================================
// State
// =============================================================================

static uint g_snes_height = 0;
static PIO g_pio_snes = pio1;

static uint g_sm_pixel = 0;
static uint g_offset_pixel = 0;
static pio_sm_config g_pio_config;
static float g_capture_pio_clkdiv = 1.0F;

static int g_dma_chan = -1;
static uint32_t g_line_buffers[2][CAPTURE_ACTIVE_WIDTH];
static volatile uint32_t g_frame_count = 0;

// PIXEL_VALID mask capture. pio1 already hosts RGB capture (g_pio_snes,
// above) and pio2 already hosts I2S audio capture (audio_subsystem.c), so
// pio0 is the only free block for this SNES build. (video_capture_mvs.c
// also uses pio0, but that is the mutually exclusive MVS backend, never
// compiled into the same build as this one.) The capture itself always
// runs, independent of whether NEOPICO_SNES_PIXEL_VALID_GATE actually
// applies it (see that flag's definition above for why gating defaults
// off); the standalone diagnostic firmware's runtime 'G' toggle covers the
// same tradeoff at runtime instead of compile time.
static PIO g_pio_mask = pio0;
static uint g_sm_mask = 0;
static uint g_offset_mask = 0;
static pio_sm_config g_mask_pio_config;
static int g_mask_dma_chan = -1;
static uint32_t g_mask_words[MASK_TOTAL_WORDS];

#if NEOPICO_EXP_GENLOCK_DYNAMIC
volatile uint32_t g_mvs_vsync_timestamp = 0;
#endif

// =============================================================================
// Pixel Conversion - RGB555 to RGB565 LUT
// =============================================================================
// Pico RetroDigital maps R0-R4, G0-G4, and B0-B4 in ascending GPIO order.

static inline uint16_t snes_pack_rgb565(uint32_t r5, uint32_t g5, uint32_t b5)
{
    return (uint16_t)((r5 << 11) | (g5 << 6) | ((g5 >> 4) << 5) | b5);
}

static uint16_t g_pixel_lut[32768] __attribute__((aligned(4)));

static void generate_pixel_lut(void)
{
    for (uint32_t idx = 0; idx < 32768U; idx++) {
        uint32_t r5 = idx & 0x1F;
        uint32_t g5 = (idx >> 5) & 0x1F;
        uint32_t b5 = (idx >> 10) & 0x1F;
        g_pixel_lut[idx] = snes_pack_rgb565(r5, g5, b5);
    }
}

static inline void fill_rgb565(uint16_t *dst, uint32_t count, uint16_t color)
{
    for (uint32_t i = 0; i < count; i++) {
        dst[i] = color;
    }
}

static inline void convert_active_pixels(uint16_t *dst, const uint32_t *src, int count)
{
    const uint16_t *lut = g_pixel_lut;
    int remaining = count;
    while (remaining >= 4) {
        dst[0] = lut[src[0] & 0x7FFF];
        dst[1] = lut[src[1] & 0x7FFF];
        dst[2] = lut[src[2] & 0x7FFF];
        dst[3] = lut[src[3] & 0x7FFF];
        dst += 4;
        src += 4;
        remaining -= 4;
    }
    while (remaining-- > 0) {
        *dst++ = lut[*src++ & 0x7FFF];
    }
}

// =============================================================================
// Brightness (QSB CMOD latch, $2100 register mirror)
// =============================================================================
// g_scale5[b][c] scales a 5-bit color channel c by brightness nibble b
// (0-15), so g_scale5[15][c] == c (unchanged) and g_scale5[0][c] == 0
// (black). Shared with the scaling case below; the b==15 and b==0 cases
// use the existing convert_active_pixels() and fill_rgb565() fast paths
// instead of going through the table.

static uint8_t g_scale5[16][32];

static void generate_scale5(void)
{
    for (uint32_t brightness = 0; brightness < 16U; brightness++) {
        for (uint32_t channel = 0; channel < 32U; channel++) {
            g_scale5[brightness][channel] = (uint8_t)((channel * brightness) / 15U);
        }
    }
}

static inline uint32_t read_cmod_nibble(void)
{
    uint32_t brightness = 0;
    brightness |= gpio_get(PIN_SNES_CMOD0) ? 0x1U : 0U;
    brightness |= gpio_get(PIN_SNES_CMOD1) ? 0x2U : 0U;
    brightness |= gpio_get(PIN_SNES_CMOD2) ? 0x4U : 0U;
    brightness |= gpio_get(PIN_SNES_CMOD3) ? 0x8U : 0U;
    return brightness;
}

// Applies the CMOD brightness nibble sampled for this line while converting
// it to RGB565. Sampling happens right after this line's DMA completes (see
// the call site in video_capture_run()), which is one line late relative to
// the ideal HBLANK-edge sample point the standalone firmware's PIO-side
// cmod SM uses. This is equivalent for ordinary frame-level fades (the
// nibble is stable for many consecutive lines) and only matters for rare
// HDMA per-line raster brightness effects, where this line would show the
// previous line's value. PIO-side per-line sampling can follow here once
// the standalone mask/cmod timing is validated on hardware.
static inline void apply_brightness_line(uint16_t *dst, const uint32_t *src, int count, uint32_t brightness)
{
    if (brightness == 15U) {
        convert_active_pixels(dst, src, count);
        return;
    }
    if (brightness == 0U) {
        fill_rgb565(dst, (uint32_t)count, 0x0000);
        return;
    }
    const uint16_t *lut = g_pixel_lut;
    const uint8_t *scale = g_scale5[brightness];
    for (int i = 0; i < count; i++) {
        const uint32_t raw = src[i] & 0x7FFF;
        const uint32_t red = raw & 0x1F;
        const uint32_t green = (raw >> 5) & 0x1F;
        const uint32_t blue = (raw >> 10) & 0x1F;
        const uint32_t scaled = scale[red] | (scale[green] << 5) | (scale[blue] << 10);
        dst[i] = lut[scaled];
    }
}

// =============================================================================
// PIXEL_VALID Mask Gating
// =============================================================================
// Compiled out by default; see NEOPICO_SNES_PIXEL_VALID_GATE's definition
// near the top of this file for why (short version: /OVER is pre-priority,
// so gating on it erases legitimate sprites/text drawn over off-map
// regions, and mishandles the $211A tile-0-fill submode). Kept here, behind
// the flag, rather than deleted, so a demo/diagnostics build can opt back
// in with -DNEOPICO_SNES_PIXEL_VALID_GATE=1 pending the hardware fix. The
// standalone diagnostic firmware covers the same tradeoff with a runtime
// 'G' command instead of a compile-time flag.
//
// words holds this line's MASK_WORDS_PER_LINE mask words, LSB-first within
// each word (see video_capture_snes.pio's snes_pixel_valid_mask program), so
// bit "bit" of words[i] corresponds to dst[i * 32 + bit]. Whole words equal
// to 0xFFFFFFFF (the common case, a fully-valid 32-pixel group) are skipped
// without a per-pixel bit-test.
#if NEOPICO_SNES_PIXEL_VALID_GATE
static inline void apply_mask_line(uint16_t *dst, const uint32_t *words, int count)
{
    for (int base = 0; base < count; base += 32) {
        const uint32_t word = *words++;
        if (word == 0xFFFFFFFFU) {
            continue;
        }
        for (int bit = 0; bit < 32; bit++) {
            if (((word >> bit) & 1U) == 0U) {
                dst[base + bit] = 0x0000U;
            }
        }
    }
}
#endif // NEOPICO_SNES_PIXEL_VALID_GATE

#if PICO_RETRODIGITAL_SNES_USB_CAPTURE
// Binary USB frame protocol, all integer fields are little-endian:
//   0  char[4] magic          "PRDF"
//   4  u16     version        1
//   6  u16     pixel_format   1 = RGB565 little-endian
//   8  u16     width          256
//  10  u16     height         224
//  12  u32     payload_bytes  width * height * 2
//  16  u32     source_frame   SNES input frame counter
//  20  u32     payload_crc32  standard CRC-32
#define SNES_USB_FRAME_HEADER_BYTES 24U
#define SNES_USB_FRAME_PROTOCOL_VERSION 1U
#define SNES_USB_PIXEL_FORMAT_RGB565_LE 1U
#define SNES_USB_FRAME_PAYLOAD_BYTES (CAPTURE_ACTIVE_WIDTH * CAPTURE_ACTIVE_HEIGHT * sizeof(uint16_t))

static void write_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8U);
}

static void write_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8U);
    dst[2] = (uint8_t)(value >> 16U);
    dst[3] = (uint8_t)(value >> 24U);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    while (length-- > 0U) {
        crc ^= *data++;
        for (uint32_t bit = 0; bit < 8U; bit++) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

static const uint16_t *snes_usb_frame_line(uint16_t line)
{
    return line_ring_write_ptr(line) + CAPTURE_ACTIVE_X_OFFSET;
}

static uint32_t snes_usb_frame_crc32(void)
{
    uint32_t crc = UINT32_MAX;
    for (uint16_t line = 0; line < CAPTURE_ACTIVE_HEIGHT; line++) {
        crc = crc32_update(crc, (const uint8_t *)snes_usb_frame_line(line), CAPTURE_ACTIVE_WIDTH * sizeof(uint16_t));
    }
    return crc ^ UINT32_MAX;
}

static bool snes_usb_write(const void *data, size_t length)
{
    return stdio_put_string((const char *)data, (int)length, false, false) == (int)length;
}

static void snes_usb_send_frame(void)
{
    uint8_t header[SNES_USB_FRAME_HEADER_BYTES] = {'P', 'R', 'D', 'F'};
    write_u16_le(&header[4], SNES_USB_FRAME_PROTOCOL_VERSION);
    write_u16_le(&header[6], SNES_USB_PIXEL_FORMAT_RGB565_LE);
    write_u16_le(&header[8], CAPTURE_ACTIVE_WIDTH);
    write_u16_le(&header[10], CAPTURE_ACTIVE_HEIGHT);
    write_u32_le(&header[12], SNES_USB_FRAME_PAYLOAD_BYTES);
    write_u32_le(&header[16], g_frame_count);
    write_u32_le(&header[20], snes_usb_frame_crc32());

    if (!snes_usb_write(header, sizeof(header))) {
        return;
    }
    for (uint16_t line = 0; line < CAPTURE_ACTIVE_HEIGHT; line++) {
        if (!snes_usb_write(snes_usb_frame_line(line), CAPTURE_ACTIVE_WIDTH * sizeof(uint16_t))) {
            return;
        }
    }
    stdio_flush();
}

static void snes_usb_capture_poll(void)
{
    if (!stdio_usb_connected()) {
        return;
    }

    bool capture_requested = false;
    while (true) {
        const int command = stdio_getchar_timeout_us(0);
        if (command == PICO_ERROR_TIMEOUT) {
            break;
        }
        if (command == 'C' || command == 'c') {
            capture_requested = true;
        }
    }

    if (capture_requested) {
        // Core 0 stops advancing the capture ring while this blocking transfer
        // runs, so the complete frame remains stable for the host.
        snes_usb_send_frame();
    }
}
#endif

// =============================================================================
// Hardware Reset
// =============================================================================

static void video_capture_reset_hardware(void)
{
    pio_sm_set_enabled(g_pio_snes, g_sm_pixel, false);
    pio_sm_clear_fifos(g_pio_snes, g_sm_pixel);
    hard_assert(pio_sm_init(g_pio_snes, g_sm_pixel, g_offset_pixel, &g_pio_config) == PICO_OK);

    pio_sm_set_enabled(g_pio_snes, g_sm_pixel, true);
    pio_sm_put_blocking(g_pio_snes, g_sm_pixel, CAPTURE_ACTIVE_WIDTH - 1);
}

// =============================================================================
// Public API
// =============================================================================

void video_capture_init(uint height)
{
    g_snes_height = height;
    generate_pixel_lut();
    generate_scale5();
    g_capture_pio_clkdiv = (float)clock_get_hz(clk_sys) / (float)SNES_CAPTURE_PIO_TARGET_HZ;
    if (g_capture_pio_clkdiv < 1.0F) {
        g_capture_pio_clkdiv = 1.0F;
    }

    hard_assert(pio_set_gpio_base(g_pio_snes, 16) == PICO_OK);
    pio_clear_instruction_memory(g_pio_snes);

    const int program_offset = pio_add_program(g_pio_snes, &snes_hard_sync_program);
    hard_assert(program_offset >= 0);
    g_offset_pixel = (uint)program_offset;
    g_sm_pixel = (uint)pio_claim_unused_sm(g_pio_snes, true);

    const uint sync_pins[] = {PIN_SNES_HBLANK, PIN_SNES_VBLANK, PIN_SNES_PCLK};
    for (size_t index = 0; index < count_of(sync_pins); index++) {
        const uint pin = sync_pins[index];
        pio_gpio_init(g_pio_snes, pin);
        gpio_disable_pulls(pin);
        gpio_set_input_enabled(pin, true);
        gpio_set_input_hysteresis_enabled(pin, true);
    }
    for (uint pin = PIN_SNES_BASE; pin <= SNES_CAPTURE_PIN_LAST; pin++) {
        pio_gpio_init(g_pio_snes, pin);
        gpio_disable_pulls(pin);
        gpio_set_input_enabled(pin, true);
        gpio_set_input_hysteresis_enabled(pin, true);
    }

    // The CMOD brightness nibble is a plain SIO read, not consumed by any
    // PIO program, so it uses gpio_init() rather than pio_gpio_init().
    // PIXEL_VALID setup lives further down, alongside the mask SM it feeds.
    const uint cmod_pins[] = {PIN_SNES_CMOD0, PIN_SNES_CMOD1, PIN_SNES_CMOD2, PIN_SNES_CMOD3};
    for (size_t index = 0; index < count_of(cmod_pins); index++) {
        const uint pin = cmod_pins[index];
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_input_enabled(pin, true);
        gpio_set_input_hysteresis_enabled(pin, true);
    }

    g_pio_config = snes_hard_sync_program_get_default_config(g_offset_pixel);
    sm_config_set_clkdiv(&g_pio_config, g_capture_pio_clkdiv);
    sm_config_set_in_pins(&g_pio_config, PIN_SNES_BASE);
    sm_config_set_in_pin_count(&g_pio_config, SNES_CAPTURE_BITS);
    sm_config_set_in_shift(&g_pio_config, false, true, SNES_CAPTURE_BITS);

    video_capture_reset_hardware();

    g_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(g_dma_chan);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(g_pio_snes, g_sm_pixel, false));
    dma_channel_configure(g_dma_chan, &dc, g_line_buffers[0], &g_pio_snes->rxf[g_sm_pixel], CAPTURE_ACTIVE_WIDTH,
                          false);

    // PIXEL_VALID mask capture (see the g_pio_mask state comment above for
    // why pio0 was chosen). GPIOBASE=0 here: HBLANK/VBLANK/PCLK/PIXEL_VALID
    // are all below GPIO 32, so no WAIT GPIO relocation is needed, matching
    // video_capture_mvs.c's use of the same block. HBLANK, VBLANK, and PCLK
    // are already enabled as inputs above for g_pio_snes; a PIO block's WAIT
    // GPIO/IN PINS instructions read the GPIO input synchronizer directly,
    // independent of which peripheral currently owns the pin's funcsel, so
    // only the not-yet-touched PIXEL_VALID pin needs setup here.
    hard_assert(pio_set_gpio_base(g_pio_mask, 0) == PICO_OK);
    pio_clear_instruction_memory(g_pio_mask);

    const int mask_program_offset = pio_add_program(g_pio_mask, &snes_pixel_valid_mask_program);
    hard_assert(mask_program_offset >= 0);
    g_offset_mask = (uint)mask_program_offset;
    g_sm_mask = (uint)pio_claim_unused_sm(g_pio_mask, true);

    pio_gpio_init(g_pio_mask, PIN_SNES_PIXEL_VALID);
    gpio_disable_pulls(PIN_SNES_PIXEL_VALID);
    gpio_set_input_enabled(PIN_SNES_PIXEL_VALID, true);
    gpio_set_input_hysteresis_enabled(PIN_SNES_PIXEL_VALID, true);

    g_mask_pio_config = snes_pixel_valid_mask_program_get_default_config(g_offset_mask);
    sm_config_set_clkdiv(&g_mask_pio_config, g_capture_pio_clkdiv);
    sm_config_set_in_pins(&g_mask_pio_config, PIN_SNES_PIXEL_VALID);
    sm_config_set_in_pin_count(&g_mask_pio_config, 1);
    sm_config_set_in_shift(&g_mask_pio_config, true, true, 32);

    pio_sm_set_enabled(g_pio_mask, g_sm_mask, false);
    pio_sm_clear_fifos(g_pio_mask, g_sm_mask);
    hard_assert(pio_sm_init(g_pio_mask, g_sm_mask, g_offset_mask, &g_mask_pio_config) == PICO_OK);
    pio_sm_set_enabled(g_pio_mask, g_sm_mask, true);
    pio_sm_put_blocking(g_pio_mask, g_sm_mask, CAPTURE_ACTIVE_WIDTH - 1);

    // Whole-frame transfer (224 lines x 8 words), armed once per frame in
    // video_capture_run() rather than per-line ping-pong like g_dma_chan
    // above: see the arming comment there for why no explicit per-line wait
    // is needed on this channel.
    g_mask_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config mask_dc = dma_channel_get_default_config(g_mask_dma_chan);
    channel_config_set_read_increment(&mask_dc, false);
    channel_config_set_write_increment(&mask_dc, true);
    channel_config_set_dreq(&mask_dc, pio_get_dreq(g_pio_mask, g_sm_mask, false));
    dma_channel_configure(g_mask_dma_chan, &mask_dc, g_mask_words, &g_pio_mask->rxf[g_sm_mask], MASK_TOTAL_WORDS,
                          false);
}

void video_capture_run(void)
{
#if NEOPICO_SNES_CAPTURE_WARMUP_FRAMES > 0
    // Let the SNES signal settle before the first real capture. Capture
    // alignment no longer needs a CPU-side VBLANK poll (the PIO's own
    // VBLANK wait handles that, cycle-exact and independent of CPU/USB
    // timing), so this plain VBLANK falling-edge poll exists purely to
    // pace warmup iterations at the source frame rate; its jitter is
    // harmless here since no capture is armed against it.
    while (g_frame_count < NEOPICO_SNES_CAPTURE_WARMUP_FRAMES) {
        while (!gpio_get(PIN_SNES_VBLANK)) {
            tight_loop_contents();
        }
        while (gpio_get(PIN_SNES_VBLANK)) {
            tight_loop_contents();
        }
        g_frame_count++;
        tud_task();
    }
#endif

    while (1) {
        // Reset pixel capture SM for frame alignment. The SM free-runs past
        // the last captured line until this reset (it keeps waiting for
        // more HBLANK pulses with nothing draining its FIFO), so it is
        // typically stalled mid-push with residual bits in the ISR by the
        // time we get here. pio_sm_restart() clears that ISR/shift state
        // and any stall latch so a stale word cannot inject a one-pixel
        // shift; it preserves X/Y, so Y keeps the (CAPTURE_ACTIVE_WIDTH - 1)
        // loaded at init. The jmp uses the pioasm-generated wrap_target
        // constant rather than a hardcoded offset.
        pio_sm_set_enabled(g_pio_snes, g_sm_pixel, false);
        pio_sm_clear_fifos(g_pio_snes, g_sm_pixel);
        pio_sm_restart(g_pio_snes, g_sm_pixel);
        pio_sm_exec(g_pio_snes, g_sm_pixel, pio_encode_jmp(g_offset_pixel + snes_hard_sync_wrap_target));
        pio_sm_set_enabled(g_pio_snes, g_sm_pixel, true);

        // Same reset, same reasoning, for the mask SM on its own (pio0)
        // block.
        pio_sm_set_enabled(g_pio_mask, g_sm_mask, false);
        pio_sm_clear_fifos(g_pio_mask, g_sm_mask);
        pio_sm_restart(g_pio_mask, g_sm_mask);
        pio_sm_exec(g_pio_mask, g_sm_mask, pio_encode_jmp(g_offset_mask + snes_pixel_valid_mask_wrap_target));
        pio_sm_set_enabled(g_pio_mask, g_sm_mask, true);

        dma_channel_set_trans_count(g_dma_chan, CAPTURE_ACTIVE_WIDTH, false);
        dma_channel_set_write_addr(g_dma_chan, g_line_buffers[0], true);

        // Mask DMA is armed once for the whole frame (224 lines x 8 words),
        // unlike the pixel channel's per-line ping-pong above: the mask SM
        // produces far less data per line, so the per-line loop below reads
        // straight out of g_mask_words[] right after each line's pixel DMA
        // completes, trusting the two SMs' shared HBLANK/PCLK cadence to
        // stay in lockstep rather than waiting on this channel explicitly.
        dma_channel_set_trans_count(g_mask_dma_chan, MASK_TOTAL_WORDS, false);
        dma_channel_set_write_addr(g_mask_dma_chan, g_mask_words, true);

        line_ring_vsync();

        // Frame-start sync (the VBLANK wait) now runs entirely on the PIO;
        // the CPU only triggers it. Frame pacing still comes for free: with
        // no vertical skip, the capture window is source lines 1-224, so
        // the last line's DMA completes right around vertical blank onset.
        // Re-arming right here still lands at or just before VBLANK's
        // rise, so the PIO's "wait 1 gpio 25" passes immediately or within
        // a few lines and "wait 0 gpio 25" catches the next frame-start
        // edge, so no source frame is skipped and 60 fps is preserved.
        //
        // Both SMs are triggered together, right here, right after the
        // previous frame's last line just finished (we are in vertical
        // blank now, per the paragraph above): the pixel SM via irq 4 of
        // g_pio_snes (pio1) and the mask SM via irq 4 of g_pio_mask (pio0),
        // a physically distinct flag despite sharing the number 4, since
        // PIO irq flags are per-block. Because both triggers land inside
        // the same vertical blank window, both SMs' "wait 1 gpio 25 / wait
        // 0 gpio 25" catch the same upcoming VBLANK falling edge, so the
        // pixel and mask planes this frame always describe the same source
        // frame.
        pio_interrupt_clear(g_pio_snes, 4);
        pio_sm_exec(g_pio_snes, g_sm_pixel, pio_encode_irq_set(false, 4));
        pio_interrupt_clear(g_pio_mask, 4);
        pio_sm_exec(g_pio_mask, g_sm_mask, pio_encode_irq_set(false, 4));

        g_frame_count++;

#if NEOPICO_EXP_GENLOCK_DYNAMIC
        // Dedicated CPU poll purely for the genlock timestamp, placed AFTER
        // the trigger above so it references the SAME upcoming frame-start
        // edge the PIO is now waiting on, instead of consuming the edge for
        // a different frame. We just armed during vertical blank (see the
        // comment above), so the PIO's "wait 1 gpio 25" falls through and
        // "wait 0 gpio 25" blocks on the next falling edge; this poll's
        // first while also falls through (VBLANK is already high) and its
        // second while blocks on that same falling edge. The timestamp
        // still carries CPU-poll jitter, but it now stamps the same frame
        // the PIO captures, and the poll consumes no extra frame. (Polling
        // before the trigger instead would let the CPU consume this
        // frame's edge while the PIO, armed afterwards, waits for the
        // next one, dropping capture to half rate.) This also holds on the
        // first post-warmup iteration: if armed mid-active-frame, both the
        // PIO and this poll wait through the same vertical blank to the
        // same edge.
        while (!gpio_get(PIN_SNES_VBLANK)) {
            tight_loop_contents();
        }
        while (gpio_get(PIN_SNES_VBLANK)) {
            tight_loop_contents();
        }
        g_mvs_vsync_timestamp = timer_hw->timerawl;
#endif

        uint8_t buf_idx = 0;
        for (uint16_t line = 0; line < g_snes_height; line++) {
            uint16_t *dst = line_ring_write_ptr(line);

            dma_channel_wait_for_finish_blocking(g_dma_chan);

            // Sampled one line late relative to the ideal HBLANK point (see
            // apply_brightness_line()'s comment); fine for frame-level
            // fades, which is what this build targets.
            const uint32_t brightness = read_cmod_nibble();

            uint32_t *buf = g_line_buffers[buf_idx];
            buf_idx ^= 1U;

            if (line + 1 < g_snes_height) {
                dma_channel_set_trans_count(g_dma_chan, CAPTURE_ACTIVE_WIDTH, false);
                dma_channel_set_write_addr(g_dma_chan, g_line_buffers[buf_idx], true);
            }

            fill_rgb565(dst, CAPTURE_ACTIVE_X_OFFSET, 0x0000);
            apply_brightness_line(dst + CAPTURE_ACTIVE_X_OFFSET, buf, CAPTURE_ACTIVE_WIDTH, brightness);
#if NEOPICO_SNES_PIXEL_VALID_GATE
            if (brightness != 0U) {
                // b==0 above already filled this line with 0x0000, so
                // masking would be a no-op; skip it, matching
                // apply_brightness_line()'s own b==0 fast path.
                apply_mask_line(dst + CAPTURE_ACTIVE_X_OFFSET, &g_mask_words[line * MASK_WORDS_PER_LINE],
                                CAPTURE_ACTIVE_WIDTH);
            }
#endif // NEOPICO_SNES_PIXEL_VALID_GATE
            fill_rgb565(dst + CAPTURE_ACTIVE_X_OFFSET + CAPTURE_ACTIVE_WIDTH,
                        LINE_WIDTH - CAPTURE_ACTIVE_X_OFFSET - CAPTURE_ACTIVE_WIDTH, 0x0000);

            line_ring_commit(line + 1);
        }

#if PICO_RETRODIGITAL_SNES_USB_CAPTURE
        snes_usb_capture_poll();
#endif
    }
}

uint32_t video_capture_get_frame_count(void)
{
    return g_frame_count;
}
