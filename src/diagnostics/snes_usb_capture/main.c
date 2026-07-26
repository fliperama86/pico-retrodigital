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

#define FRAME_MAGIC "PRDF"
#define ERROR_MAGIC "PRDE"
#define FRAME_PROTOCOL_VERSION 1u
#define FRAME_PIXEL_FORMAT_RGB565_LE 1u
#define FRAME_HEADER_BYTES 24u
#define ERROR_HEADER_BYTES 32u
#define FRAME_PAYLOAD_BYTES (CAPTURE_WIDTH * CAPTURE_HEIGHT * sizeof(uint16_t))
#define SIGNAL_SAMPLE_US 50000u
// Worst case is about 33 ms: up to a full frame waiting for VBLANK plus
// 14.3 ms of capture. All frame-start synchronization now runs on the PIO,
// so this is the only timeout capture_frame() needs.
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

static uint32_t raw_frame[CAPTURE_HEIGHT * CAPTURE_WIDTH];
static uint16_t frame_buffer[CAPTURE_HEIGHT][CAPTURE_WIDTH] __attribute__((aligned(4)));
static uint16_t pixel_lut[32768] __attribute__((aligned(4)));
static uint8_t tx_header[ERROR_HEADER_BYTES];

static uint32_t captured_frames;
static bool capture_requested;
static uint32_t tx_offset;
static uint32_t tx_header_bytes;
static uint32_t tx_payload_bytes;

static void pixel_lut_init(void)
{
    for (uint32_t index = 0; index < 32768u; ++index) {
        const uint32_t red = index & 0x1fu;
        const uint32_t green = (index >> 5u) & 0x1fu;
        const uint32_t blue = (index >> 10u) & 0x1fu;
        pixel_lut[index] = (uint16_t)((red << 11u) | (green << 6u) | ((green >> 4u) << 5u) | blue);
    }
}

static void convert_frame(void)
{
    uint16_t *destination = &frame_buffer[0][0];
    for (uint32_t pixel = 0; pixel < CAPTURE_HEIGHT * CAPTURE_WIDTH; ++pixel) {
        destination[pixel] = pixel_lut[raw_frame[pixel] & 0x7fffu];
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

static void frame_header_prepare(void)
{
    memcpy(tx_header, FRAME_MAGIC, 4);
    write_u16_le(&tx_header[4], FRAME_PROTOCOL_VERSION);
    write_u16_le(&tx_header[6], FRAME_PIXEL_FORMAT_RGB565_LE);
    write_u16_le(&tx_header[8], CAPTURE_WIDTH);
    write_u16_le(&tx_header[10], CAPTURE_HEIGHT);
    write_u32_le(&tx_header[12], FRAME_PAYLOAD_BYTES);
    write_u32_le(&tx_header[16], captured_frames);
    write_u32_le(&tx_header[20], frame_crc32());
    tx_offset = 0;
    tx_header_bytes = FRAME_HEADER_BYTES;
    tx_payload_bytes = FRAME_PAYLOAD_BYTES;
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
}

static void capture_reset(void)
{
    // The SM free-runs after DMA completes (it keeps capturing lines past
    // the requested frame height until VBLANK), so it is typically stalled
    // mid-push with residual bits in the ISR by the time the next capture
    // starts. pio_sm_restart() clears the ISR/OSR shift state and stall
    // latches so a stale word cannot inject a one-pixel shift; it preserves
    // X/Y, so Y keeps the (CAPTURE_WIDTH - 1) loaded at init. The jmp uses
    // the pioasm-generated wrap_target constant rather than a hardcoded
    // offset so it stays correct if the program layout changes.
    pio_sm_set_enabled(capture_pio, capture_sm, false);
    pio_sm_clear_fifos(capture_pio, capture_sm);
    pio_sm_restart(capture_pio, capture_sm);
    pio_sm_exec(capture_pio, capture_sm, pio_encode_jmp(capture_program_offset + snes_hard_sync_wrap_target));
    pio_sm_set_enabled(capture_pio, capture_sm, true);
}

static void capture_init(void)
{
    pixel_lut_init();

    hard_assert(pio_set_gpio_base(capture_pio, 16u) == PICO_OK);
    pio_clear_instruction_memory(capture_pio);
    const int program_offset = pio_add_program(capture_pio, &snes_hard_sync_program);
    hard_assert(program_offset >= 0);
    capture_program_offset = (uint)program_offset;
    capture_sm = (uint)pio_claim_unused_sm(capture_pio, true);

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

    capture_config = snes_hard_sync_program_get_default_config(capture_program_offset);
    sm_config_set_clkdiv(&capture_config, (float)clock_get_hz(clk_sys) / (float)CAPTURE_PIO_TARGET_HZ);
    sm_config_set_in_pins(&capture_config, PIN_SNES_BASE);
    sm_config_set_in_pin_count(&capture_config, CAPTURE_BITS);
    sm_config_set_in_shift(&capture_config, false, true, CAPTURE_BITS);
    hard_assert(pio_sm_init(capture_pio, capture_sm, capture_program_offset, &capture_config) == PICO_OK);

    pio_sm_set_enabled(capture_pio, capture_sm, true);
    pio_sm_put_blocking(capture_pio, capture_sm, CAPTURE_WIDTH - 1u);

    capture_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config dma_config = dma_channel_get_default_config((uint)capture_dma_channel);
    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, true);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_dreq(&dma_config, pio_get_dreq(capture_pio, capture_sm, false));
    channel_config_set_high_priority(&dma_config, true);
    dma_channel_configure((uint)capture_dma_channel, &dma_config, raw_frame, &capture_pio->rxf[capture_sm],
                          CAPTURE_HEIGHT * CAPTURE_WIDTH, false);
}

static bool wait_for_dma(uint32_t timeout_us)
{
    const absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (dma_channel_is_busy((uint)capture_dma_channel)) {
        if (time_reached(deadline)) {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

static void capture_frame(void)
{
    // Frame-start synchronization (the VBLANK wait) runs on the PIO, so the
    // start line is cycle-exact and independent of CPU/USB timing; the CPU
    // may arm the trigger at any point in the frame. It only resets the SM,
    // arms the whole-frame DMA, and triggers the PIO's irq-4 wait.
    capture_reset();

    dma_channel_set_trans_count((uint)capture_dma_channel, CAPTURE_HEIGHT * CAPTURE_WIDTH, false);
    dma_channel_set_write_addr((uint)capture_dma_channel, raw_frame, true);

    pio_interrupt_clear(capture_pio, 4u);
    pio_sm_exec(capture_pio, capture_sm, pio_encode_irq_set(false, 4u));

    if (!wait_for_dma(FRAME_TIMEOUT_US)) {
        dma_channel_abort((uint)capture_dma_channel);
        pio_sm_set_enabled(capture_pio, capture_sm, false);

        // Stats sampling takes 50ms; only pay for it while preparing an
        // error, never on the happy path.
        const signal_stats_t stats = signal_stats_sample();
        const uint32_t remaining = dma_channel_hw_addr((uint)capture_dma_channel)->transfer_count;
        const uint32_t lines_done = (CAPTURE_HEIGHT * CAPTURE_WIDTH - remaining) / CAPTURE_WIDTH;
        capture_error_prepare(CAPTURE_ERROR_FRAME_TIMEOUT, &stats, lines_done);
        return;
    }

    convert_frame();
    captured_frames++;
    frame_header_prepare();
}

static void usb_rx_task(void)
{
    while (tud_cdc_available() > 0u) {
        const int command = tud_cdc_read_char();
        if (command == 'C' || command == 'c') {
            capture_requested = true;
        }
    }
}

static void usb_tx_task(void)
{
    const uint32_t tx_bytes = tx_header_bytes + tx_payload_bytes;
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
    } else {
        const uint32_t payload_offset = tx_offset - tx_header_bytes;
        source = &((const uint8_t *)frame_buffer)[payload_offset];
        remaining = tx_payload_bytes - payload_offset;
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
            tx_offset = 0;
            tx_header_bytes = 0;
            tx_payload_bytes = 0;
            continue;
        }

        usb_rx_task();
        usb_tx_task();

        if (capture_requested && tx_header_bytes == 0u && tx_payload_bytes == 0u) {
            capture_requested = false;
            capture_frame();
        }
    }
}
