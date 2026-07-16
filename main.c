#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "led_panel.h"

#if HUB75_DRIVER == HUB75_DRIVER_PIO
#include "hardware/pio.h"

// -----------------------------------------------------------------------
// Memory layout (PIO driver)
//   logical_buf  (48 KB)  – uint16_t RGB framebuffer; written by Core0
//   ps_buf_a/b   (64 KB each) – pre-serialised PIO byte stream; double-buffered
//   Total: ~176 KB  (fits in RP2040's 264 KB SRAM)
//
// Pre-serialised format (one byte per PIO clock):
//   bit7=ROW  bit6=LE  bit5=B2  bit4=G2  bit3=R2  bit2=B1  bit1=G1  bit0=R1
//
// Display loop (Core1): streams ps_display → PIO FIFO.
// Content loop (Core0): renders into logical_buf, converts → ps_staging,
//   then atomically swaps ps_display ↔ ps_staging under mutex.
//   Core1 repeats the last complete frame at full speed while Core0 works.
// -----------------------------------------------------------------------

// Logical RGB framebuffer – written exclusively by Core0.
static uint16_t logical_buf[MATRIX_WIDTH * MATRIX_HEIGHT * 3];

// Pre-serialised double buffer – Core0 writes to staging, Core1 reads display.
static uint8_t ps_buf_a[PS_BUF_SIZE];
static uint8_t ps_buf_b[PS_BUF_SIZE];
static uint8_t *ps_display = ps_buf_a;
static uint8_t *ps_staging = ps_buf_b;

// All demo/GIF rendering targets this buffer directly.
#define RENDER_BUF logical_buf

#else // HUB75_DRIVER == HUB75_DRIVER_CPU

// -----------------------------------------------------------------------
// Memory layout (CPU driver)
//   frame_buf_a/b (48 KB each) – uint16_t RGB framebuffers, double-buffered.
//   Core1's display loop bit-bangs display_buf directly (no PIO, no
//   pre-serialisation step); Core0 renders into tmp_buf then swaps.
// -----------------------------------------------------------------------
static uint16_t frame_buf_a[MATRIX_WIDTH * MATRIX_HEIGHT * 3];
static uint16_t frame_buf_b[MATRIX_WIDTH * MATRIX_HEIGHT * 3];
static uint16_t *display_buf = frame_buf_a;  // read by core1_entry()
static uint16_t *tmp_buf     = frame_buf_b;  // written by Core0 demo code

#define RENDER_BUF tmp_buf

#endif // HUB75_DRIVER

// Guards the display-buffer handoff between Core0 (renders) and Core1
// (displays).  Held by Core1 for the full frame in the PIO driver; used
// only for the brief pointer swap in the CPU driver.
static mutex_t ps_mutex;

// -----------------------------------------------------------------------
// Demo selection
// -----------------------------------------------------------------------
#define STATIC_COLORS_DEMO (0U)
#define GIF_FILE_DEMO      (1U)
#define RAINBOW_WAVE_DEMO  (2U)
#define GREY_FILL_DEMO     (3U)

#define DEMO_TYPE GIF_FILE_DEMO

// -----------------------------------------------------------------------
// Gamma correction (universal; applies to all demo types)
// -----------------------------------------------------------------------
// The DP3364S only latches the lower 13 bits of the 16-bit grayscale word.
// Bits 13-15 must be zero.  Table is scaled to max 0x1FFF (13 bits).
#define GAMMA 2.5f
static uint16_t gamma_lut[256];

static void build_gamma_lut(void)
{
    for (int i = 0; i < 256; i++) {
        float normalized = i / 255.0f;
        gamma_lut[i] = (uint16_t)(powf(normalized, GAMMA) * 0x1fff + 0.5f);
    }
}

// -----------------------------------------------------------------------
// GIF demo helpers
// -----------------------------------------------------------------------
#if DEMO_TYPE == GIF_FILE_DEMO
#include "minigif.h"
#include "gif.h"

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} memfile_t;

static long _read_bytes_cb(void *f, uint8_t *val, size_t size)
{
    memfile_t *mem = (memfile_t *)f;
    if (mem->pos + size > mem->size)
        size = mem->size - mem->pos;
    memcpy(val, mem->data + mem->pos, size);
    mem->pos += size;
    return size;
}

static long _lseek_cb(void *f, long off, int seek)
{
    memfile_t *mem = (memfile_t *)f;
    size_t new_pos = 0;
    switch (seek) {
        case SEEK_SET: new_pos = off; break;
        case SEEK_CUR: new_pos = mem->pos + off; break;
        case SEEK_END: new_pos = mem->size + off; break;
        default: return -1;
    }
    if (new_pos > mem->size) return -1;
    mem->pos = new_pos;
    return (long)mem->pos;
}

static void _painter_cb(uint16_t x, uint16_t y, gif_rgb_t rgb, void *user_data)
{
    size_t idx = (y * MATRIX_WIDTH + x) * 3;
    RENDER_BUF[idx + 0] = gamma_lut[rgb.r];
    RENDER_BUF[idx + 1] = gamma_lut[rgb.g];
    RENDER_BUF[idx + 2] = gamma_lut[rgb.b];
}
#endif // DEMO_TYPE == GIF_FILE_DEMO

// -----------------------------------------------------------------------
// Static diagnostic patterns (non-rainbow / non-GIF builds)
// -----------------------------------------------------------------------
#if DEMO_TYPE != GIF_FILE_DEMO && DEMO_TYPE != RAINBOW_WAVE_DEMO
static void fill_gradient_pattern(uint16_t *buf)
{
    for (uint y = 0; y < MATRIX_HEIGHT; y++) {
        uint16_t g = (uint16_t)((y * 255) / (MATRIX_HEIGHT - 1)) << 5;
        for (uint x = 0; x < MATRIX_WIDTH; x++) {
            uint16_t r = (uint16_t)((x * 255) / (MATRIX_WIDTH - 1)) << 5;
            size_t idx = (y * MATRIX_WIDTH + x) * 3;
            buf[idx + 0] = r;
            buf[idx + 1] = g;
            buf[idx + 2] = 0;
        }
    }
}

static void fill_chip_block_pattern(uint16_t *buf)
{
    static const uint8_t colors[8][3] = {
        {1,0,0},{0,1,0},{0,0,1},{1,1,1},{1,1,0},{0,1,1},{1,0,1},{1,1,1},
    };
    for (uint y = 0; y < MATRIX_HEIGHT; y++) {
        uint16_t scale = (uint16_t)(0x1fff * (y + 1) / MATRIX_HEIGHT);
        for (uint x = 0; x < MATRIX_WIDTH; x++) {
            uint group = x / 16;
            uint16_t s = (group == 7) ? (scale / 3) : scale;
            size_t idx = (y * MATRIX_WIDTH + x) * 3;
            buf[idx + 0] = colors[group][0] * s;
            buf[idx + 1] = colors[group][1] * s;
            buf[idx + 2] = colors[group][2] * s;
        }
    }
}

// Uniform low-brightness solid-colour fill covering the whole 128x64 panel.
// Near-black flicker at column-loop/chain boundaries is much harder to spot
// against a busy gradient/rainbow pattern — a flat, dim field makes any
// per-pixel instability stand out clearly against its otherwise-static
// neighbours.  Restricting to a single channel (e.g. blue-only) also makes
// any WRONG-channel flicker (e.g. a stray white or red pixel) far easier to
// spot than a same-channel brightness flicker would be.
static void fill_solid_pattern(uint16_t *buf, uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t rv = gamma_lut[r], gv = gamma_lut[g], bv = gamma_lut[b];
    for (uint i = 0; i < MATRIX_WIDTH * MATRIX_HEIGHT; i++) {
        buf[i * 3 + 0] = rv;
        buf[i * 3 + 1] = gv;
        buf[i * 3 + 2] = bv;
    }
}
#endif

// -----------------------------------------------------------------------
// Rainbow wave (celebratory demo)
// -----------------------------------------------------------------------
#if DEMO_TYPE == RAINBOW_WAVE_DEMO
static void hsv_to_panel(float h, float s, float v,
                         uint16_t *r, uint16_t *g, uint16_t *b)
{
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rf, gf, bf;
    if      (h <  60) { rf = c; gf = x; bf = 0; }
    else if (h < 120) { rf = x; gf = c; bf = 0; }
    else if (h < 180) { rf = 0; gf = c; bf = x; }
    else if (h < 240) { rf = 0; gf = x; bf = c; }
    else if (h < 300) { rf = x; gf = 0; bf = c; }
    else              { rf = c; gf = 0; bf = x; }
    uint8_t ri = (uint8_t)((rf + m) * 255.0f + 0.5f);
    uint8_t gi = (uint8_t)((gf + m) * 255.0f + 0.5f);
    uint8_t bi = (uint8_t)((bf + m) * 255.0f + 0.5f);
    *r = gamma_lut[ri];
    *g = gamma_lut[gi];
    *b = gamma_lut[bi];
}

static void fill_rainbow_wave(uint16_t *buf, float phase)
{
    for (uint y = 0; y < MATRIX_HEIGHT; y++) {
        for (uint x = 0; x < MATRIX_WIDTH; x++) {
            float hue = fmodf(phase
                              + (float)x * (360.0f / MATRIX_WIDTH)
                              + (float)y * (180.0f / MATRIX_HEIGHT), 360.0f);
            uint16_t r, g, b;
            hsv_to_panel(hue, 1.0f, 1.0f, &r, &g, &b);
            size_t idx = (y * MATRIX_WIDTH + x) * 3;
            buf[idx + 0] = r;
            buf[idx + 1] = g;
            buf[idx + 2] = b;
        }
    }
}
#endif // DEMO_TYPE == RAINBOW_WAVE_DEMO

#if HUB75_DRIVER == HUB75_DRIVER_PIO
// -----------------------------------------------------------------------
// Pre-serialisation converter (PIO driver only)
// -----------------------------------------------------------------------
// Converts the uint16_t logical framebuffer into the PIO byte stream stored
// in `ps`.  The output format matches hub75_pio.pio's FIFO expectations:
//   byte = (LE<<6) | (B2_bit<<5)|(G2_bit<<4)|(R2_bit<<3)|
//                    (B1_bit<<2)|(G1_bit<<1)| R1_bit
//
// Chip call order mirrors send_line_and_row_pulse (pre-PIO):
//   outer loop: col 0..15 (column within driver chip)
//   inner loop: ch  0..3  (chip in the chain, 0-indexed)
//   chip_idx = col*4 + ch  →  0..63 in the pre-serialised row buffer
//   col_offset = ch*16+col  →  0..63, pixel position within each half
//   rgb_hi (R2/G2/B2) = left half pixel  (confirmed: drives left 64 px)
//   rgb_lo (R1/G1/B1) = right half pixel (confirmed: drives right 64 px)
//
// For gamma_lut max 0x1FFF (13 bits), bit planes 13-15 are always 0 and
// are skipped in the inner loop; their entries were zeroed by the memset.
static void convert_logical_to_ps(const uint16_t *logical, uint8_t *ps)
{
    // Bit planes 13-15 are always 0 (gamma_lut ≤ 0x1FFF); zero them once.
    memset(ps, 0, PS_BUF_SIZE);

    for (uint row = 0; row < MATRIX_HEIGHT; row++) {
        uint8_t *row_buf = ps + (uint32_t)row * PS_BYTES_PER_ROW;

        for (uint col = 0; col < 16; col++) {
            for (uint ch = 0; ch < CHAINED_DP3364_NUM; ch++) {
                uint chip_idx  = col * CHAINED_DP3364_NUM + ch;  // 0..63
                uint col_off   = ch * 16 + col;                  // 0..63

                // Left half pixel → rgb_hi → R2/G2/B2 pins
                const uint16_t *px_hi =
                    &logical[(row * MATRIX_WIDTH + col_off) * 3];
                // Right half pixel → rgb_lo → R1/G1/B1 pins
                const uint16_t *px_lo =
                    &logical[(row * MATRIX_WIDTH + 64 + col_off) * 3];

                uint16_t r1 = px_lo[0], g1 = px_lo[1], b1 = px_lo[2];
                uint16_t r2 = px_hi[0], g2 = px_hi[1], b2 = px_hi[2];

                // LE fires on the last CLK of the last chip in each column
                // group (ch==3, bit plane 0 = the final serial bit).
                bool latch = (ch == CHAINED_DP3364_NUM - 1);

                uint8_t *chip_buf = row_buf + chip_idx * PS_BIT_PLANES;

                // Bit 15 is sent FIRST (MSB-first serial order to DP3364S).
                //   chip_buf[0]  → bit plane 15 (first CLK,  always 0)
                //   chip_buf[1]  → bit plane 14 (always 0)
                //   chip_buf[2]  → bit plane 13 (always 0)
                //   chip_buf[3]  → bit plane 12 (MSB of 13-bit value)
                //   chip_buf[15] → bit plane  0 (last CLK, LE fires here)
                // Planes 13-15 (indices 0-2) remain 0 from the memset.
                for (int bit = 12; bit >= 0; bit--) {
                    chip_buf[15 - bit] =
                        (uint8_t)(
                            (((r1 >> bit) & 1u) << 0) |
                            (((g1 >> bit) & 1u) << 1) |
                            (((b1 >> bit) & 1u) << 2) |
                            (((r2 >> bit) & 1u) << 3) |
                            (((g2 >> bit) & 1u) << 4) |
                            (((b2 >> bit) & 1u) << 5) |
                            ((latch & (bit == 0)) ? PS_LE_BIT : 0u)
                        );
                }
            }
        }
    }
}

// -----------------------------------------------------------------------
// Core1 display loop
// -----------------------------------------------------------------------
// Streams pre-serialised data to the PIO FIFO for maximum CLK throughput.
//
// PIO LE mechanism (see hub75_pio.pio for full explanation):
//   The PIO program uses `out pins, 13` to drive GPIO 0-12 from each FIFO word:
//     bits  0-5  → colour (R1/G1/B1/R2/G2/B2)
//     bits  6-11 → 0 (unused / CLK overridden by side-set)
//     bit  12    → LE
//   GPIO 11 (CLK) is inside the OUT range but overridden by the side-set.
//   GPIO 12 (LE) is driven by bit 12 of each FIFO word.
//
//   The PS buffer stores LE at bit 6 (PS_LE_BIT = 0x40).  Before pushing,
//   the CPU moves LE from bit 6 to bit 12:
//     word = (byte & 0x3F) | (((byte >> 6) & 1) << 12)
//   This embeds LE in the data stream with no counter state to drift.
//
// Buffer safety:
//   Core1 holds ps_mutex for the ENTIRE frame, not just for the pointer
//   copy.  This prevents publish_frame() on Core0 from swapping ps_display
//   and immediately writing the next frame into the old display buffer
//   (now ps_staging) while Core1 is still reading it.  Core0 blocks on the
//   mutex for at most one frame period (~5 ms), comfortably within its
//   33 ms content-update rate.
//
// Per-frame timing estimate (PIO at 80 MHz, clkdiv=3 — see hub75_pio.pio
// for the authoritative per-cycle breakdown):
//   Every byte: 9 cycles × 12.5 ns = 112.5 ns → ~8.9 MHz DCLK
//   1024 bytes/row × 112.5 ns ≈ 115.2 µs/row
//   CPU section (ROW + DCK + settle CLKs, run at ~20 MHz via
//   clock_pulse_row(), plus ~2.5 µs + ~0.8 µs pause/resume settle margin):
//   ≈ 4 µs/row
//   Total: ~119.2 µs/row × 64 rows ≈ 7.6 ms/frame → ~130 Hz
static void core1_entry(void)
{
    dp3364_init();       // CPU-based panel init (PIO not yet active)
    dp3364_pio_init();   // Hand GPIO 0-5, 11-13 to PIO; start state machine

    while (1) {
        // Hold the mutex for the FULL frame so Core0 cannot start writing
        // to the old display buffer (now ps_staging) until we are done
        // reading it.  Core0 serialises into ps_staging before the swap, so
        // only the brief pointer-swap step actually needs the lock — but
        // keeping it held prevents the race on the subsequent Core0 write.
        mutex_enter_blocking(&ps_mutex);
        const uint8_t *buf = ps_display;

        // VSYNC once per frame, matching the original CPU display loop.
        // GPIO 11/12 are in PIO mode during normal operation; pause the SM
        // briefly so dp3364_vsync() can use CPU SIO to drive them.
        //
        // Assert OE (active-LOW → gpio HIGH = blank) for the full VSYNC +
        // SM-restart window.  The SM restarts from a cold output-register
        // state which produces a different electrical transient on the first
        // few CLK edges of row 0 compared to mid-stream rows 1-63.  Blanking
        // suppresses any top-row glitch caused by that transient.  The
        // blanking lasts only ~20 µs per 6 ms frame (< 0.4% duty-cycle
        // reduction) so the brightness impact is imperceptible.
        gpio_put(PIN_OE, 1);   // blank display
        dp3364_pio_pause();
        dp3364_vsync();
        dp3364_pio_resume();
        gpio_put(PIN_OE, 0);   // unblank: SM is running, row 0 data follows

        for (uint k = 0; k < MATRIX_HEIGHT; k++) {
            // Stream exactly 1024 pre-serialised bytes via PIO.
            // The PIO `out pins, 13` drives GPIO 0-12 from OSR bits[12:0]:
            //   bits  0-5  → colour (R1/G1/B1/R2/G2/B2)
            //   bits  6-11 → 0 (unused / CLK overridden by side-set)
            //   bit  12    → LE
            // The PS buffer stores LE at bit 6 (PS_LE_BIT = 0x40), so we
            // shift it to bit 12 before pushing.  Colour bits 0-5 are used
            // directly.  This embeds LE directly in the data stream with no
            // counter state that could drift across pause/resume cycles.
            const uint8_t *row = buf + (uint32_t)k * PS_BYTES_PER_ROW;
            for (uint i = 0; i < PS_BYTES_PER_ROW; i++) {
                uint32_t byte = (uint32_t)row[i];
                // Move LE from bit 6 to bit 12; colour stays in bits 0-5.
                uint32_t word = (byte & 0x3Fu) | (((byte >> 6) & 1u) << 12u);
                pio_sm_put_blocking(panel_pio, panel_pio_sm, word);
            }

            // Drain the FIFO and let the SM finish the last word before
            // switching GPIO control back to the CPU.
            // After the FIFO empties the SM takes at most 6 more PIO cycles
            // (75 ns) to complete the last instruction and stall at
            // `pull block` with CLK=0 — the safe pause point.
            //
            // Widened from 120 to 600 CPU cycles (~2.5 us at 240 MHz): the
            // CPU section that follows (ROW pulse + DCK + settle clocks)
            // drives PIN_CLK via slow SIO toggles while GPIO 0-5 (colour)
            // are frozen at 0 by dp3364_pio_pause()'s forced `out`.  Those
            // clocks shift zero-bits into the chain immediately after the
            // real column-14/15 data was latched — right when the chip is
            // electrically freshest and most sensitive to any noise on the
            // shift/latch lines.  A larger settle margin here gives the
            // chip more time to fully complete the LE latch internally
            // before that CPU-driven clocking begins.
            while (!pio_sm_is_tx_fifo_empty(panel_pio, panel_pio_sm))
                tight_loop_contents();
            busy_wait_at_least_cycles(600);

            // CPU section (single pause per row):
            //   ROW pulse  – ROW held high for 12 or 4 CLKs (CPU-only)
            //   DCK        – advances the DP32020 row-driver chain
            //   Extra CLKs – GCLK settling (8 CLK pulses to DP3364S)
            dp3364_pio_pause();
            dp3364_row_pulse_only_pio(k == 0);
            led_panel_advance_row(k);
            for (uint i = 0; i < (CLK_CYCLES_PER_LINE / 8 + 1); i++) {
                clock_pulse_row();
            }
            dp3364_pio_resume();

            // Settle margin after handing CLK/LE back to the PIO and before
            // the next row's real data starts streaming, so the SM's
            // re-enable transient doesn't bleed into column 0's timing.
            busy_wait_at_least_cycles(200);
        }

        // Release the mutex now that Core1 is done reading this buffer.
        // Core0 can now swap ps_display <-> ps_staging and start writing
        // to the new ps_staging (the old display buffer we just finished).
        mutex_exit(&ps_mutex);
    }
}

#else // HUB75_DRIVER == HUB75_DRIVER_CPU

// -----------------------------------------------------------------------
// Core1 display loop (CPU driver — ported from the pi-fully-working
// baseline).  No PIO, no pre-serialisation: every CLK/LE/ROW pulse is
// bit-banged directly from the logical uint16_t framebuffer via
// dp3364_panel_draw()/dp3364_row_pulse_only()/led_panel_advance_row().
// -----------------------------------------------------------------------

// Sends one complete row's worth of column data (128px, split across the
// two parallel R1/R2 half-chains) and fires the DP3364S ROW pulse for it.
static void send_line_and_row_pulse(const uint16_t *fb, uint line, bool is_very_first_line)
{
    for (uint col = 0; col < 16; col++) { // column within driver chip
        for (uint ch = 1; ch <= CHAINED_DP3364_NUM; ch++) { // chip in the chain
            uint8_t col_offset = ((ch - 1) * 16) + col;   // 0..63, position within a half

            size_t idx_left_half = (line * MATRIX_WIDTH + col_offset) * 3;
            uint16_t rgb_hi[3] = {
                fb[idx_left_half + 0],
                fb[idx_left_half + 1],
                fb[idx_left_half + 2]
            };

            size_t idx_right_half = (line * MATRIX_WIDTH + 64 + col_offset) * 3;
            uint16_t rgb_lo[3] = {
                fb[idx_right_half + 0],
                fb[idx_right_half + 1],
                fb[idx_right_half + 2]
            };

            bool latch = ch == CHAINED_DP3364_NUM;
            dp3364_panel_draw(rgb_lo, rgb_hi, latch);
        }
    }
    dp3364_row_pulse_only(is_very_first_line);
}

// Per-frame timing estimate (CPU clock_pulse() ≈ 33 ns half-period, the
// baseline rate — see clock_delay() in led_panel.h):
//   Every bit:  ~66 ns (CLK high + CLK low)
//   16 bits/word × 64 chip-calls/row × 66 ns ≈ 67.6 µs/row (colour data)
//   Row CPU:    ROW + DCK + settle clocks ≈ few µs/row
//   Total: ~72 µs/row × 64 rows ≈ 4.6 ms/frame → ~217 Hz (upper bound;
//   actual rate is lower due to gpio_put/gpio_put_masked call overhead).
static void core1_entry(void)
{
    dp3364_init();

    while (1) {
        mutex_enter_blocking(&ps_mutex);
        const uint16_t *fb = display_buf;

        dp3364_vsync();
        for (uint k = 0; k < MATRIX_HEIGHT; k++) {
            send_line_and_row_pulse(fb, k, k == 0);
            led_panel_advance_row(k);
            for (uint i = 0; i < (CLK_CYCLES_PER_LINE / 8 + 1); i++) {
                clock_pulse();
            }
        }
        mutex_exit(&ps_mutex);
    }
}

#endif // HUB75_DRIVER

// -----------------------------------------------------------------------
// Core0 helpers
// -----------------------------------------------------------------------
static uint32_t get_time_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

#if HUB75_DRIVER == HUB75_DRIVER_PIO
// Convert logical_buf → ps_staging and atomically swap ps_display.
static void publish_frame(void)
{
    convert_logical_to_ps(logical_buf, ps_staging);

    mutex_enter_blocking(&ps_mutex);
    uint8_t *tmp  = ps_display;
    ps_display    = ps_staging;
    ps_staging    = tmp;
    mutex_exit(&ps_mutex);
}
#else
// Atomically swap display_buf <-> tmp_buf (CPU driver — no conversion step).
static void publish_frame(void)
{
    mutex_enter_blocking(&ps_mutex);
    uint16_t *tmp = display_buf;
    display_buf   = tmp_buf;
    tmp_buf       = tmp;
    mutex_exit(&ps_mutex);
}
#endif

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------
int main(void)
{
    set_sys_clock_hz(240 * MHZ, true);
    stdio_init_all();

    // Onboard LED (GPIO25) – blink 3× at startup, then solid ON.
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    for (int _b = 0; _b < 3; _b++) {
        gpio_put(25, 1); sleep_ms(150);
        gpio_put(25, 0); sleep_ms(150);
    }
    gpio_put(25, 1);

    // Initialise GPIO 0-13 as outputs (PIO will take 0-5, 11-13 later).
    const uint32_t init_mask = 0x3FFF;
    gpio_init_mask(init_mask);
    gpio_set_dir_out_masked(init_mask);
    gpio_put_masked(init_mask, 0);

    for (uint i = 0; i <= 13; i++) {
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
    }

    // Initialise buffers and synchronisation.
#if HUB75_DRIVER == HUB75_DRIVER_PIO
    memset(logical_buf, 0, sizeof(logical_buf));
    memset(ps_buf_a, 0, sizeof(ps_buf_a));
    memset(ps_buf_b, 0, sizeof(ps_buf_b));
#else
    memset(frame_buf_a, 0, sizeof(frame_buf_a));
    memset(frame_buf_b, 0, sizeof(frame_buf_b));
#endif
    mutex_init(&ps_mutex);
    build_gamma_lut();

#if DEMO_TYPE == GIF_FILE_DEMO
    static memfile_t gif_mem = {
        .data = gif_file,
        .size = sizeof(gif_file),
        .pos  = 0
    };
    static uint8_t gif_buf[sizeof(gif_t)];
    gif_cb_t gif_cb = {
        .read      = _read_bytes_cb,
        .lseek     = _lseek_cb,
        .painter   = _painter_cb,
        .user_data = NULL,
    };
    gif_handle_t gif = minigif_init(gif_buf, &gif_mem, gif_cb);
    assert(gif != NULL);
#elif DEMO_TYPE == RAINBOW_WAVE_DEMO
    fill_rainbow_wave(RENDER_BUF, 0.0f);
#elif DEMO_TYPE == GREY_FILL_DEMO
    // Dim, blue-only fill: low brightness makes marginal-timing flicker
    // easiest to trigger, and single-channel makes any stray wrong-channel
    // pixel (white / other colour) stand out starkly against the blue field.
    fill_solid_pattern(RENDER_BUF, 0, 0, 16); // ~16/255 ≈ 1/16 brightness, blue only
#else
    fill_chip_block_pattern(RENDER_BUF);
#endif

    // Get the initial frame into the buffer Core1 will read from.
#if HUB75_DRIVER == HUB75_DRIVER_PIO
    // Pre-serialise directly into ps_display so Core1 has something to
    // display immediately on startup (no swap needed: RENDER_BUF ==
    // logical_buf, the single source buffer for conversion).
    convert_logical_to_ps(logical_buf, ps_display);
#else
    // RENDER_BUF == tmp_buf here; swap it into display_buf before Core1
    // starts (safe — Core1 hasn't launched yet, so the mutex is uncontended).
    publish_frame();
#endif

    // Launch Core1 with a 4 KB stack (dp3364_init is deeply nested).
    static uint32_t core1_stack[1024];
    multicore_launch_core1_with_stack(core1_entry, core1_stack, sizeof(core1_stack));

    // Core0 content loop
    while (1) {
#if DEMO_TYPE == GIF_FILE_DEMO
        uint32_t t0 = get_time_ms();

        gif_status_t ret = minigif_render_frame(gif);
        if (ret == GIF_STATUS_GIF_END) {
            minigif_rewind(gif);
            minigif_render_frame(gif);
        }

        // Publish the just-rendered frame, then clear RENDER_BUF for the
        // next frame (disposal=2 fix: transparent pixels show black, not
        // the previous frame's content).
        publish_frame();
        memset(RENDER_BUF, 0,
               MATRIX_WIDTH * MATRIX_HEIGHT * 3 * sizeof(uint16_t));

        uint32_t delta        = get_time_ms() - t0;
        uint32_t gif_delay_ms = minigif_get_frame_delay(gif) * 10;
        uint32_t task_delay   = gif_delay_ms > delta ? gif_delay_ms - delta : 1;
        busy_wait_ms(task_delay);

#elif DEMO_TYPE == RAINBOW_WAVE_DEMO
        static float phase = 0.0f;
        phase = fmodf(phase + 1.2f, 360.0f);

        fill_rainbow_wave(RENDER_BUF, phase);
        publish_frame();

        busy_wait_ms(33); // ~30 fps content update rate

#elif DEMO_TYPE == GREY_FILL_DEMO
        sleep_ms(1000); // static pattern, nothing to update

#else
        sleep_ms(1000);
#endif
    }

    return 0;
}
