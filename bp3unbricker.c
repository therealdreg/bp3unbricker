/*
MIT License - bp3unbricker
---------------------------------------------------------------------------
Copyright (c) [2026] by David Reguera Garcia aka Dreg
https://github.com/therealdreg/bp3unbricker
https://www.rootkit.es
X @therealdreg
dreg@rootkit.es
---------------------------------------------------------------------------
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/*
 * bp3unbricker: firmware for a Raspberry Pi Pico or Pico 2 that programs the
 * PIC24FJ64GA002 of a Bus Pirate v3 over plain 2-wire ICSP (DS39768D). It writes the
 * bootloader and the firmware, even to a blank chip, and verifies every word. A USB
 * serial console drives it, and the BOOTSEL button runs a flash without a computer.
 * Everything about it is in README.md.
 *
 * The whole firmware is this one file:
 *    1. configuration    the wiring
 *    2. CRC-32
 *    3. images           the built-in image, two bootloaders alone, an uploaded image
 *    4. pins and delays  the Pico's SIO and SysTick
 *    5. ICSP             bit level: key, SIX, REGOUT
 *    6. PIC24            device ID, chip erase, row write, read
 *    7. operations       flash, bootloader, verify, dump, test, jumper and the rest
 *    8. upload           an image that the computer sends row by row, each with a CRC-32
 *    9. operation log    the output of the last run, also of a BOOTSEL run
 *   10. console          commands, LED, BOOTSEL button, main()
 *   11. embedded image   the built-in image and two bootloaders, made from stuff/
 *
 * Pico build: CMakeLists.txt (see README.md).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BP3_HOST_TEST
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/systick.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"
#include "pico/rand.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#endif

/* ======================================================== 1. configuration */
/*
 * The wiring of a Raspberry Pi Pico or Pico 2 to the ICSP header (JP2) of the
 * Bus Pirate v3, the same pins on both boards:
 *
 *   Pico GP10 (pin 14)   JP2-1 PGC
 *   Pico GP11 (pin 15)   JP2-2 PGD
 *   Pico GND  (pin 13)   JP2-3 GND
 *   nothing              JP2-4 3V3: the Bus Pirate runs from its own USB cable
 *   Pico GP12 (pin 16)   JP2-5 MCLR
 */

#define PIN_PGC 10
#define PIN_PGD 11
#define PIN_MCLR 12

/* BP3_VERSION and BP3_BOARD come from CMakeLists.txt. */

/* =============================================================== 2. CRC-32 */
/* CRC-32 (IEEE 802.3, same as zlib.crc32). Pass 0 as crc to start. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* =============================================================== 3. images */
/*
 * A complete program-flash image of the PIC24FJ64GA002: 22016 instructions,
 * 3 bytes each (little-endian). There are four: the bootloader and firmware built
 * into the Pico (section 11, made from its .hex file in stuff/), two bootloaders
 * alone (also in section 11: the ds30 bootloader of that image, and bp3mitloader),
 * and an image sent by the computer (section 8).
 */

#define PIC_FLASH_WORDS 22016u   /* PC 0x000000..0x00ABFE */
#define PIC_BLANK_WORD 0xFFFFFFu /* an erased instruction */
#define IMAGE_BYTES (PIC_FLASH_WORDS * 3u)

/* A Bus Pirate v3 bootloader (buzzpirat's firmware/bootloader/ds30loader.s, or
 * bp3mitloader.s) is the reset GOTO 0x00A800 at 0x000000 and the whole last page
 * 0x00A800..0x00ABFE: its code, the BRA at 0x00ABF8, the version word, CW2 and CW1.
 * 0x00A7FC holds the bootloader's GOTO to the firmware, which buzzloader writes with
 * the firmware. While it is erased, the bootloader has nothing to start. */
#define IMAGE_RESET_WORDS 2u
#define IMAGE_USRAPP_INDEX (0xA7FCu / 2u)
#define IMAGE_BL_PAGE_INDEX (0xA800u / 2u)
/* The words of a bootloader alone: the reset GOTO, then the bootloader page. */
#define IMAGE_BL_WORDS (IMAGE_RESET_WORDS + PIC_FLASH_WORDS - IMAGE_BL_PAGE_INDEX)
#define IMAGE_BL_BYTES (IMAGE_BL_WORDS * 3u)

typedef struct {
    const char *name;
    const uint8_t *data;  /* IMAGE_BYTES, or IMAGE_BL_BYTES for a bootloader alone */
    uint32_t crc32;       /* CRC-32 of the words image_word() gives, 3 bytes each */
    uint32_t used_rows;   /* rows with at least one programmed word */
    bool bootloader_only; /* a bootloader alone: data holds its words only, the others read as erased */
} image_t;

extern const image_t embedded_image;
/* The bootloaders alone: the ds30 bootloader of embedded_image ('bootloader ds30'),
 * and bp3mitloader ('bootloader'). */
extern const image_t ds30_image, mit_image;

/* The words a bootloader alone keeps: the reset GOTO and the bootloader page. */
static inline bool word_in_bootloader(uint32_t index) {
    return index < IMAGE_RESET_WORDS || index >= IMAGE_BL_PAGE_INDEX;
}

/* 24-bit instruction at PC address 2 * index. */
static inline uint32_t image_word(const image_t *img, uint32_t index) {
    if (img->bootloader_only) {
        if (!word_in_bootloader(index)) return PIC_BLANK_WORD;
        if (index >= IMAGE_BL_PAGE_INDEX) index -= IMAGE_BL_PAGE_INDEX - IMAGE_RESET_WORDS;
    }
    const uint8_t *p = &img->data[index * 3u];
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/* The inverse of image_word(): a word as its 3 image bytes, little-endian. */
static inline void word_to_bytes(uint32_t word, uint8_t *bytes) {
    bytes[0] = (uint8_t)word;
    bytes[1] = (uint8_t)(word >> 8);
    bytes[2] = (uint8_t)(word >> 16);
}

/* What crc32 holds: the CRC-32 of the words image_word() gives, 3 bytes each. */
static uint32_t image_crc32(const image_t *img) {
    uint32_t crc = 0;
    for (uint32_t i = 0; i < PIC_FLASH_WORDS; i++) {
        uint8_t bytes[3];
        word_to_bytes(image_word(img, i), bytes);
        crc = crc32_update(crc, bytes, sizeof bytes);
    }
    return crc;
}

/* True if the image exists and its words still match its CRC-32. */
static bool image_ok(const image_t *img) { return img && image_crc32(img) == img->crc32; }

/* ====================================================== 4. pins and delays */
/*
 * Pin-level access to the ICSP header of the Bus Pirate v3. On the Pico, each pin
 * operation is one or two SIO register accesses and SysTick paces the delays, all
 * inlined, so a PGC phase lasts what the ICSP code asks for, with no function call
 * overhead on top.
 *
 * MCLR: driven push-pull, or released with a weak pull-down so that the board's
 * 2k pull-up controls it (and an unconnected wire reads low).
 * PGC: always driven while a session is open. It is released without any pull,
 * because the bootloader reads a low PGC as its jumper.
 * PGD: both ways. Driven for commands, released (weak pull-down) while the PIC
 * answers.
 * On the Pico, hal_delay_ns() busy-waits the requested time minus the pin-access
 * overhead (PHASE_OVERHEAD_CYCLES), so the whole PGC phase lasts about that time.
 */

/* Set up the delays and release every target line: PGC without a pull, PGD and
 * MCLR with weak pull-downs. */
void hal_init(void);
/* Longer busy waits and a millisecond clock. */
void hal_delay_us(uint32_t us);
void hal_delay_ms(uint32_t ms);
uint32_t hal_now_ms(void);

#define SYSTICK_MASK 0x00FFFFFFu /* SysTick is a 24-bit down counter */

#ifdef BP3_HOST_TEST
#include "bp3unbricker_host.h" /* the HAL and the Pico SDK of a host build */
#else
/* The bit-level code runs from RAM: in flash, the reads of the 66 KB image would keep
 * evicting it from the XIP cache. icsp_six() and icsp_regout() must also never be
 * inlined into their callers, which run from flash, so they use the no-inline variant. */
#define HAL_RAMFUNC(name) __not_in_flash_func(name)
#define HAL_NOINLINE_RAMFUNC(name) __no_inline_not_in_flash_func(name)

static inline void hal_drive_pin(uint32_t pin, bool level) {
    uint32_t mask = 1u << pin;
    if (level) {
        sio_hw->gpio_set = mask;
    } else {
        sio_hw->gpio_clr = mask;
    }
    sio_hw->gpio_oe_set = mask; /* level first, then the driver: no glitch */
}

static inline void hal_mclr_drive(bool level) { hal_drive_pin(PIN_MCLR, level); }
static inline void hal_mclr_release(void) { sio_hw->gpio_oe_clr = 1u << PIN_MCLR; }
static inline bool hal_mclr_read(void) { return (sio_hw->gpio_in >> PIN_MCLR) & 1u; }
static inline void hal_pgc_drive(bool level) { hal_drive_pin(PIN_PGC, level); }
static inline void hal_pgc_release(void) { sio_hw->gpio_oe_clr = 1u << PIN_PGC; }
static inline bool hal_pgc_read(void) { return (sio_hw->gpio_in >> PIN_PGC) & 1u; }
static inline void hal_pgd_drive(bool level) { hal_drive_pin(PIN_PGD, level); }
static inline void hal_pgd_release(void) { sio_hw->gpio_oe_clr = 1u << PIN_PGD; }
static inline bool hal_pgd_read(void) { return (sio_hw->gpio_in >> PIN_PGD) & 1u; }

/* Cycles spent between two delays on pin accesses and loop bookkeeping, measured
 * with the 'bench' command and the Saleae. Each delay is that much shorter, so a
 * PGC phase lasts close to the requested time and never much less. */
#define PHASE_OVERHEAD_CYCLES 12u

static uint32_t cycles_per_ns_q20; /* cycles per ns, 20-bit fixed point */

/* The last two conversions, which hal_init() empties (UINT32_MAX). The ICSP code
 * keeps asking for the same PGC low and high times, so the 64-bit multiply runs only
 * when they change. */
static uint32_t delay_cache_ns[2];
static uint32_t delay_cache_cycles[2];

/* The slow path of hal_delay_ns(): convert, then remember. It is not static on
 * purpose: when it is, GCC compiles icsp_enter_key_only() differently. */
uint32_t hal_cycles_for_ns(uint32_t ns) {
    uint32_t cycles = (uint32_t)(((uint64_t)ns * cycles_per_ns_q20) >> 20) + 1u;
    cycles = cycles > PHASE_OVERHEAD_CYCLES ? cycles - PHASE_OVERHEAD_CYCLES : 1u;
    delay_cache_ns[1] = delay_cache_ns[0];
    delay_cache_cycles[1] = delay_cache_cycles[0];
    delay_cache_ns[0] = ns;
    delay_cache_cycles[0] = cycles;
    return cycles;
}

static inline void hal_delay_ns(uint32_t ns) {
    uint32_t start = systick_hw->cvr;
    uint32_t cycles = ns == delay_cache_ns[0]   ? delay_cache_cycles[0]
                      : ns == delay_cache_ns[1] ? delay_cache_cycles[1]
                                                : hal_cycles_for_ns(ns);
    /* SysTick counts down at clk_sys; the 24-bit wrap is 112 ms even at 150 MHz. */
    while (((start - systick_hw->cvr) & SYSTICK_MASK) < cycles) {
    }
}

static void pin_setup(uint pin) {
    gpio_init(pin);              /* SIO function, input, output latch low */
    gpio_disable_pulls(pin);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_SLOW);  /* softer edges on jumper wires */
    gpio_set_input_hysteresis_enabled(pin, true);
}

void hal_init(void) {
    uint32_t cycles_per_us = clock_get_hz(clk_sys) / 1000000u;
    cycles_per_ns_q20 = (uint32_t)(((uint64_t)cycles_per_us << 20) / 1000u) + 1u;
    delay_cache_ns[0] = delay_cache_ns[1] = UINT32_MAX;
    /* SysTick as a free-running 24-bit down counter at clk_sys (no interrupt). */
    systick_hw->rvr = SYSTICK_MASK;
    systick_hw->cvr = 0;
    systick_hw->csr = 0x5u; /* ENABLE | CLKSOURCE = processor clock */
    pin_setup(PIN_PGC);      /* no pull: a low PGC at boot is the bootloader's jumper */
    pin_setup(PIN_PGD);
    pin_setup(PIN_MCLR);
    /* Pull-downs of about 50k, so an open wire reads low. Against the board's 2k
     * pull-up, MCLR still floats to about 3.2 V, well above VIH (0.8 VDD). */
    gpio_pull_down(PIN_PGD);
    gpio_pull_down(PIN_MCLR);
}

void hal_delay_us(uint32_t us) { busy_wait_us_32(us); }

void hal_delay_ms(uint32_t ms) { busy_wait_ms(ms); }

uint32_t hal_now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
#endif

/* ================================================================= 5. ICSP */
/*
 * 2-wire ICSP (not Enhanced ICSP) for the PIC24FJXXXGA0XX family, DS39768D
 * section 3: the SIX and REGOUT control codes, LSb first. Data changes while PGC
 * is low, and the PIC latches it on the rising edge.
 */

#define ICSP_KEY 0x4D434851u  /* "MCHQ": ICSP, not Enhanced ICSP ("MCHP") */
#define OP_NOP 0x000000u
#define OP_GOTO_200 0x040200u /* GOTO 0x200; its second word is 0x000000 */

/* PGC low and high time of 'speed slow|normal|fast'. Normal is the power-up
 * default: 500 ns + 500 ns, a nominal 1 MHz (about 850 kHz measured), well
 * below the 10 MHz limit. The PICkit 4 uses 1.6 MHz on this chip. */
#define ICSP_SLOW_PHASE_NS 2000u
#define ICSP_NORMAL_PHASE_NS 500u
#define ICSP_FAST_PHASE_NS 200u
/* icsp_set_timing() raises a shorter phase to this. The datasheet minimums are
 * 40 ns per phase and a 100 ns period. */
#define ICSP_MIN_PHASE_NS 100u

typedef struct {
    uint32_t low_ns;   /* PGC low time; PGD is set at its start (setup time) */
    uint32_t high_ns;  /* PGC high time; PGD is sampled at its end */
} icsp_timing_t;

/* ICSP entry delays (DS39768 P19/P7) and the pause after the forced first SIX. */
typedef struct {
    uint32_t p19_us;          /* from the last key clock to MCLR high, >= 1 ms */
    uint32_t p7_us;           /* from MCLR high to the first clock, >= 25 ms */
    uint32_t first_pause_us;  /* pause after the forced 9-bit SIX + NOP */
} icsp_entry_delays_t;

static icsp_timing_t timing = {.low_ns = ICSP_NORMAL_PHASE_NS, .high_ns = ICSP_NORMAL_PHASE_NS};
/* Measured with the PICkit 4: P19 1.0 ms, P7 25.0 ms, 0.85 ms after the first
 * instruction. Ours keep a margin on all three. */
static icsp_entry_delays_t entry_delays = {.p19_us = 1500, .p7_us = 30000, .first_pause_us = 1000};
/* PGC clocks sent. Nothing reads it, but the count is part of the machine code of
 * the bit helpers below, whose timing was checked on the bench: keep it. */
static uint32_t clocks;

void icsp_set_timing(uint32_t low_ns, uint32_t high_ns) {
    timing.low_ns = low_ns < ICSP_MIN_PHASE_NS ? ICSP_MIN_PHASE_NS : low_ns;
    timing.high_ns = high_ns < ICSP_MIN_PHASE_NS ? ICSP_MIN_PHASE_NS : high_ns;
}

icsp_timing_t icsp_get_timing(void) { return timing; }

void icsp_set_entry_delays(icsp_entry_delays_t delays) { entry_delays = delays; }

icsp_entry_delays_t icsp_get_entry_delays(void) { return entry_delays; }

/* PGC is low when every helper below starts and when it returns. */

static void HAL_RAMFUNC(bit_out)(uint32_t bit) {
    hal_pgd_drive(bit & 1u);       /* data changes while PGC is low */
    hal_delay_ns(timing.low_ns);   /* setup time (P2 >= 15 ns) */
    hal_pgc_drive(true);           /* the PIC latches on the rising edge */
    hal_delay_ns(timing.high_ns);  /* hold time (P3 >= 15 ns) */
    hal_pgc_drive(false);
    clocks++;
}

/* bit_out() that also checks that the pads follow: PGD settled, PGC low, then high.
 * Only the key uses it, and the key runs from flash, so it needs no HAL_RAMFUNC. */
static bool bit_out_checked(uint32_t bit) {
    bool level = (bit & 1u) != 0;
    hal_pgd_drive(level);
    hal_delay_ns(timing.low_ns);
    bool ok = hal_pgd_read() == level && !hal_pgc_read();
    hal_pgc_drive(true);
    hal_delay_ns(timing.high_ns);
    ok = ok && hal_pgc_read();
    hal_pgc_drive(false);
    clocks++;
    return ok;
}

static void HAL_RAMFUNC(clock_idle)(void) {
    hal_delay_ns(timing.low_ns);
    hal_pgc_drive(true);
    hal_delay_ns(timing.high_ns);
    hal_pgc_drive(false);
    clocks++;
}

static uint32_t HAL_RAMFUNC(bit_in)(void) {
    hal_delay_ns(timing.low_ns);
    hal_pgc_drive(true);
    hal_delay_ns(timing.high_ns);
    /* The PIC changes PGD about 25 ns after each falling edge: sample it just
     * before the next falling edge, when the data has been stable longest. */
    uint32_t bit = hal_pgd_read() ? 1u : 0u;
    hal_pgc_drive(false);
    clocks++;
    return bit;
}

/* Execute one 24-bit instruction. */
void HAL_NOINLINE_RAMFUNC(icsp_six)(uint32_t instr) {
    for (int i = 0; i < 4; i++) {
        bit_out(0); /* SIX = 0000b */
    }
    for (int i = 0; i < 24; i++) {
        bit_out(instr >> i);
    }
}

/* Clock the VISI register out of the PIC. */
uint16_t HAL_NOINLINE_RAMFUNC(icsp_regout)(void) {
    bit_out(1); /* REGOUT = 0001b, LSb first */
    bit_out(0);
    bit_out(0);
    bit_out(0);
    /* The PIC starts driving PGD right after the falling edge of the 8th
     * idle clock, so let go of the line before the idle clocks. */
    hal_pgd_release();
    for (int i = 0; i < 8; i++) {
        clock_idle();
    }
    uint16_t value = 0;
    for (int i = 0; i < 16; i++) {
        value |= (uint16_t)(bit_in() << i);
    }
    /* The PIC releases PGD on the falling edge of the 16th data clock. */
    hal_delay_ns(timing.low_ns / 2);
    hal_pgd_drive(false);
    return value;
}

/* The first part of icsp_enter(), also the bring-up command 'raw key': the MCLR
 * pulse, the key, P19, MCLR high, P7 and the 5 extra clocks of the forced 9-bit
 * SIX. Every key bit is read back from the pads. On a mismatch, MCLR stays low and
 * the function returns false. */
bool icsp_enter_key_only(void) {
    hal_mclr_drive(false);   /* reset: every PIC pin becomes an input */
    hal_delay_ms(2);
    hal_pgc_drive(false);
    hal_pgd_drive(false);
    hal_delay_us(10);
    hal_mclr_drive(true);    /* "MCLR is briefly driven high, then low" */
    hal_delay_us(15);
    hal_mclr_drive(false);
    hal_delay_us(100);       /* P18 >= 40 ns */
    bool ok = true;
    for (int i = 31; i >= 0; i--) {
        ok = bit_out_checked(ICSP_KEY >> i) && ok;  /* the key goes MSb first */
    }
    hal_pgd_drive(false);
    if (!ok) {
        return false;        /* stay in reset */
    }
    hal_delay_us(entry_delays.p19_us);  /* P19 >= 1 ms */
    hal_mclr_drive(true);
    hal_delay_us(entry_delays.p7_us);   /* P7 >= 25 ms */
    for (int i = 0; i < 5; i++) {
        bit_out(0);          /* the first SIX after reset is 9 bits long */
    }
    return true;
}

/* Enter ICSP mode: icsp_enter_key_only(), then the NOP of the forced SIX and
 * GOTO 0x200, which parks the PC at 0x200. The caller checked that the target
 * is powered. */
bool icsp_enter(void) {
    if (!icsp_enter_key_only()) {
        return false;
    }
    /* Measured on B8 silicon: the first instruction after the forced SIX is
     * not executed but still advances the PC, and the PIC resets (leaving
     * ICSP) as soon as the PC reaches the interrupt vector table at 0x0004.
     * So the GOTO 0x200 must be the second instruction: NOP, GOTO 0x200, NOP
     * exactly as in every table of DS39768 (the PICkit 4 sends GOTO, GOTO, NOP). */
    icsp_six(OP_NOP);        /* discarded by the PIC */
    hal_delay_us(entry_delays.first_pause_us); /* the PICkit 4 pauses about 0.85 ms here */
    icsp_six(OP_GOTO_200);
    icsp_six(OP_NOP);        /* second word of GOTO */
    return true;
}

/* Leave ICSP mode; the PIC stays in reset (MCLR driven low). */
void icsp_exit(void) {
    hal_pgc_drive(false);
    hal_pgd_drive(false);
    hal_delay_us(10);        /* P16 >= 0 */
    hal_mclr_drive(false);
    hal_delay_ms(1);
}

/* ================================================================ 6. PIC24 */
/*
 * PIC24FJ64GA002 operations over ICSP (DS39768D tables 3-4 to 3-9).
 */

#define PIC_DEVID_GA002 0x0447u
#define PIC_DEVID_ADDR 0xFF0000u /* DEVID; DEVREV follows at +2 */
#define PIC_ROW_WORDS 64u
#define PIC_ROWS (PIC_FLASH_WORDS / PIC_ROW_WORDS) /* 344 */
#define PIC_CW2_INDEX (0xABFCu / 2u)
#define PIC_CW1_INDEX (0xABFEu / 2u)
/* The diagnostic and FRC calibration words of executive memory, at
 * 0x8007F0..0x8007FE. They are only ever read. */
#define PIC_EXEC_CAL_INDEX (0x8007F0u / 2u)
#define PIC_EXEC_CAL_WORDS 8u

typedef enum {
    PIC_OK = 0,
    PIC_ERR_NO_TARGET,   /* MCLR does not float high: board unpowered or held in reset */
    PIC_ERR_LINES,       /* a line does not follow the programmer: short or another programmer */
    PIC_ERR_NO_RESPONSE, /* VISI echo failed: the PIC is not in ICSP mode */
    PIC_ERR_DEVID,       /* no PIC24FJ64GA002 answered */
    PIC_ERR_WR_TIMEOUT,  /* NVMCON.WR never cleared */
    PIC_ERR_WRERR,       /* NVMCON.WRERR set after an erase or a write */
    PIC_ERR_NOT_BLANK,
    PIC_ERR_VERIFY,
    PIC_ERR_IMAGE,       /* the image (built-in or uploaded) failed its CRC */
} pic_status_t;

/* Compare masks: the config words' upper byte is unimplemented (reads 0xFF),
 * and CW1<15> is reserved ("program as 0") and reads 0 even when erased
 * (DS39768 table 3-6: 7FFFh). */
static inline uint32_t pic_word_mask(uint32_t index) {
    return index == PIC_CW1_INDEX ? 0x007FFFu : index == PIC_CW2_INDEX ? 0x00FFFFu : 0xFFFFFFu;
}

#define SFR_TBLPAG 0x0032u
#define SFR_NVMCON 0x0760u
#define SFR_VISI 0x0784u

#define NVMCON_WR 0x8000u
#define NVMCON_WRERR 0x2000u
/* Whole NVMCON values (DS39768 tables 3-2 and 3-3): WREN plus the operation. */
#define NVMCON_CHIP_ERASE 0x404Fu
#define NVMCON_ROW_WRITE 0x4001u

#define OP_BSET_NVMCON_WR 0xA8E761u    /* BSET NVMCON, #WR          */
#define OP_CLR_W6 0xEB0300u            /* CLR W6                    */
#define OP_TBLWTL_W0_IW0 0xBB0800u     /* TBLWTL W0, [W0]           */
#define OP_TBLWTL_W6PP_IW7 0xBB0BB6u   /* TBLWTL [W6++], [W7]       */
#define OP_TBLWTHB_W6PP_W7PP 0xBBDBB6u /* TBLWTH.B [W6++], [W7++]   */
#define OP_TBLWTHB_W6PP_PPW7 0xBBEBB6u /* TBLWTH.B [W6++], [++W7]   */
#define OP_TBLWTL_W6PP_W7PP 0xBB1BB6u  /* TBLWTL [W6++], [W7++]     */
#define OP_TBLRDL_IW6_IW7 0xBA0B96u    /* TBLRDL [W6], [W7]         */
#define OP_TBLRDHB_W6PP_W7PP 0xBADBB6u /* TBLRDH.B [W6++], [W7++]   */
#define OP_TBLRDHB_PPW6_W7MM 0xBAD3D6u /* TBLRDH.B [++W6], [W7--]   */
#define OP_TBLRDL_W6PP_IW7 0xBA0BB6u   /* TBLRDL [W6++], [W7]       */

/* MOV #lit16, Wn */
static uint32_t op_mov_lit(uint32_t lit, unsigned wn) {
    return 0x200000u | ((lit & 0xFFFFu) << 4) | (wn & 0xFu);
}

/* MOV Wn, f */
static uint32_t op_mov_w_f(unsigned wn, uint32_t f) {
    return 0x880000u | ((f >> 1) << 4) | (wn & 0xFu);
}

/* MOV f, Wn */
static uint32_t op_mov_f_w(uint32_t f, unsigned wn) {
    return 0x800000u | ((f >> 1) << 4) | (wn & 0xFu);
}

static void nop(void) { icsp_six(OP_NOP); }

/* TBLRD, TBLWT and BSET NVMCON, #WR: two NOPs follow, as in the DS39768 tables
 * (3.2.1.1: a table instruction needs a second cycle, and ICSP inserts no
 * pipeline stall). */
static void six_nop_nop(uint32_t op) {
    icsp_six(op);
    nop();
    nop();
}

/* Every REGOUT is followed by a NOP (DS39768 tables 3-4 to 3-9). */
static uint16_t regout_nop(void) {
    uint16_t value = icsp_regout();
    nop();
    return value;
}

/* The PC keeps incrementing in ICSP mode; park it at 0x200 regularly so it
 * never reaches unimplemented memory (that would reset the device). */
static void reset_pc(void) {
    icsp_six(OP_GOTO_200);
    nop(); /* second word of GOTO */
}

/* Step 1 of every DS39768 table: exit the reset vector. */
static void exit_reset_vector(void) {
    nop();
    reset_pc();
}

/* Step 2 of DS39768 table 3-9 (steps 2 and 3 of table 3-8): the read pointer
 * TBLPAG:W6 = addr, the write pointer W7 = VISI. */
static void init_table_read(uint32_t addr) {
    icsp_six(op_mov_lit(addr >> 16, 0));
    icsp_six(op_mov_w_f(0, SFR_TBLPAG));
    icsp_six(op_mov_lit(addr & 0xFFFFu, 6));
    icsp_six(op_mov_lit(SFR_VISI, 7));
    nop();
}

const char *pic_status_str(pic_status_t st) {
    switch (st) {
        case PIC_OK: return "OK";
        case PIC_ERR_NO_TARGET:
            return "no target (MCLR not pulled up: Bus Pirate unpowered, not connected or held in reset)";
        case PIC_ERR_LINES:
            return "MCLR/PGC/PGD do not follow the programmer (short circuit, or another programmer driving them)";
        case PIC_ERR_NO_RESPONSE: return "the PIC does not answer over ICSP (check PGC/PGD/MCLR/GND wiring)";
        case PIC_ERR_DEVID: return "wrong or missing device ID (check wiring)";
        case PIC_ERR_WR_TIMEOUT: return "flash operation timed out (WR never cleared)";
        case PIC_ERR_WRERR: return "flash operation failed (WRERR)";
        case PIC_ERR_NOT_BLANK: return "chip is not blank";
        case PIC_ERR_VERIFY: return "verify failed";
        case PIC_ERR_IMAGE: return "image is corrupt (CRC mismatch)";
    }
    return "?";
}

/* The line that the Pico keeps holding low between operations: PGC for the bootloader's
 * jumper (pic_jumper_reset(): MCLR is released and the bootloader waits in bootloader
 * mode), or MCLR after a failed write (pic_try_hold_in_reset(): PGC and PGD are
 * released). The next function that drives or releases the lines ends it:
 * pic_target_present() (at the start of every session), pic_session_end() and
 * pic_release_target(). pic_pgc_release() ends the jumper. */
static enum { HOLD_NONE, HOLD_JUMPER, HOLD_RESET } hold;

/* True if MCLR reads high twice, 200 us apart: the board pulls it up. */
static bool mclr_pulled_up(void) {
    bool high = hal_mclr_read();
    hal_delay_us(200);
    return high && hal_mclr_read();
}

/* True when the board pulls the released MCLR line high (powered, not held in
 * reset). It releases every line, so call it only outside a session. It ends a
 * jumper or a hold. */
bool pic_target_present(void) {
    hold = HOLD_NONE;
    hal_pgc_release();
    hal_pgd_release();
    hal_mclr_release();
    hal_delay_ms(2);
    return mclr_pulled_up();
}

/* Leave ICSP; the PIC stays in reset (MCLR driven low) with PGC and PGD driven low,
 * so a jumper or a hold is over too. */
void pic_session_end(void) {
    hold = HOLD_NONE;
    icsp_exit();
}

/* Leave ICSP but keep the PIC in reset (MCLR driven low), with PGC and PGD released:
 * after a failed write, the PIC must not run what is on it. The exception is something
 * else driving MCLR (a short, another programmer): if MCLR still reads high 1 ms after
 * it was driven low, holding it low would fight that driver, so MCLR is released too
 * and the function returns false. A hold that works lasts until the lines change
 * (pic_reset_held()). */
bool pic_try_hold_in_reset(void) {
    pic_session_end();
    hal_pgc_release();
    hal_pgd_release();
    if (!hal_mclr_read()) {
        hold = HOLD_RESET;
        return true;
    }
    hal_mclr_release();
    return false;
}

/* The PIC is still held in reset by pic_try_hold_in_reset(). */
bool pic_reset_held(void) { return hold == HOLD_RESET; }

/* Restart the PIC with every line released: it runs. */
void pic_release_target(void) {
    hold = HOLD_NONE;
    hal_mclr_drive(false);
    /* PGC without a pull: the bootloader reads PGC low as its
     * "stay in bootloader" jumper. */
    hal_pgc_release();
    hal_pgd_release();
    hal_delay_ms(1);
    hal_mclr_release();
}

/* Restart the PIC with PGC held low: the bootloader reads that as its PGC-PGD
 * jumper. PGC stays low until the next session or pic_release_target() (see
 * hold for the rest). 'status' leaves it in place. */
void pic_jumper_reset(void) {
    hal_mclr_drive(false);
    hal_pgd_release(); /* the bootloader drives PGD low itself while it tests the jumper */
    hal_pgc_drive(false);
    hal_delay_ms(1);
    hal_mclr_release();
    hold = HOLD_JUMPER;
}

/* PGC is still held low by pic_jumper_reset(). */
bool pic_jumper_held(void) { return hold == HOLD_JUMPER; }

/* pic_target_present() that keeps the jumper: MCLR is already released then, so
 * it is only read, and PGC and PGD stay as they are. */
bool pic_target_present_keep_jumper(void) {
    return hold == HOLD_JUMPER ? mclr_pulled_up() : pic_target_present();
}

/* Release PGC alone, without a pull (for the bring-up command 'bench'). This ends a
 * jumper, but not a hold in reset (MCLR low). */
void pic_pgc_release(void) {
    if (hold == HOLD_JUMPER) hold = HOLD_NONE;
    hal_pgc_release();
}

/* With the PIC held in reset (all its pins are inputs), every line must read
 * back what the Pico drives. Anything else means a short or a second programmer.
 * PGC is only driven low here: a rising edge with MCLR low would clock a
 * stray bit into the PIC's key receiver. icsp_enter() checks PGC high on
 * every key bit. */
static bool lines_follow(void) {
    hal_mclr_drive(false);
    hal_delay_ms(1);
    bool ok = !hal_mclr_read();
    hal_pgc_drive(false);
    hal_pgd_drive(true);
    hal_delay_us(20);
    ok = ok && !hal_pgc_read() && hal_pgd_read();
    hal_pgd_drive(false);
    hal_delay_us(20);
    ok = ok && !hal_pgc_read() && !hal_pgd_read();
    return ok;
}

/* Echo two patterns through VISI. This proves that the PIC executes the SIX
 * instructions and that REGOUT data comes back bit-exact. */
static const uint16_t echo_patterns[2] = {0xA55A, 0x5AA5};

static bool visi_echo(uint16_t echoed[2]) {
    bool ok = true;
    for (int i = 0; i < 2; i++) {
        icsp_six(op_mov_lit(echo_patterns[i], 0));
        icsp_six(op_mov_w_f(0, SFR_VISI));
        nop();
        uint16_t v = regout_nop();
        if (echoed) echoed[i] = v;
        ok = ok && v == echo_patterns[i];
    }
    return ok;
}

/* Enter ICSP and echo two patterns through VISI; 'echoed' (may be NULL) gets
 * what came back. On PIC_OK and PIC_ERR_NO_RESPONSE the session is open. */
pic_status_t pic_ping(uint16_t echoed[2]) {
    if (!pic_target_present()) {
        return PIC_ERR_NO_TARGET;
    }
    if (!lines_follow() || !icsp_enter()) {
        pic_release_target();
        return PIC_ERR_LINES;
    }
    exit_reset_vector();
    return visi_echo(echoed) ? PIC_OK : PIC_ERR_NO_RESPONSE;
}

/* Open an ICSP session and read DEVID/DEVREV. On PIC_OK the session is open;
 * on PIC_ERR_DEVID it is open too (close it with pic_session_end). On any other
 * status no session is open and every line is released, so the PIC can run. */
pic_status_t pic_session_begin(uint16_t *devid, uint16_t *devrev) {
    pic_status_t st = pic_ping(NULL);
    if (st == PIC_ERR_NO_RESPONSE) {
        pic_session_end();
        pic_release_target();
    }
    if (st != PIC_OK) {
        return st;
    }
    init_table_read(PIC_DEVID_ADDR);
    six_nop_nop(OP_TBLRDL_W6PP_IW7); /* DEVID @ 0xFF0000 */
    uint16_t id = regout_nop();
    six_nop_nop(OP_TBLRDL_W6PP_IW7); /* DEVREV @ 0xFF0002 */
    uint16_t rev = regout_nop();
    reset_pc();
    if (devid) *devid = id;
    if (devrev) *devrev = rev;
    return id == PIC_DEVID_GA002 ? PIC_OK : PIC_ERR_DEVID;
}

static uint16_t read_nvmcon(void) {
    reset_pc();
    icsp_six(op_mov_f_w(SFR_NVMCON, 2));
    icsp_six(op_mov_w_f(2, SFR_VISI));
    nop();
    return regout_nop();
}

static pic_status_t wait_wr(uint32_t timeout_ms, uint32_t *elapsed_ms) {
    uint32_t start_ms = hal_now_ms();
    for (;;) {
        uint16_t nvmcon = read_nvmcon();
        uint32_t elapsed = hal_now_ms() - start_ms;
        if (!(nvmcon & NVMCON_WR)) {
            if (elapsed_ms) *elapsed_ms = elapsed;
            return (nvmcon & NVMCON_WRERR) ? PIC_ERR_WRERR : PIC_OK;
        }
        if (elapsed > timeout_ms) {
            return PIC_ERR_WR_TIMEOUT;
        }
        hal_delay_us(200);
    }
}

/* Chip erase of user memory only (TBLPAG = 0): code and configuration words.
 * Executive memory and the FRC calibration word are not touched. */
pic_status_t pic_chip_erase(uint32_t *elapsed_ms) {
    exit_reset_vector();
    icsp_six(op_mov_lit(NVMCON_CHIP_ERASE, 10));
    icsp_six(op_mov_w_f(10, SFR_NVMCON));
    /* TBLPAG < 0x80: erase user memory only, never executive memory and
     * its FRC calibration word. */
    icsp_six(op_mov_lit(0x0000, 0));
    icsp_six(op_mov_w_f(0, SFR_TBLPAG));
    icsp_six(op_mov_lit(0x0000, 0));
    nop(); /* not in table 3-4: W0 is used as a pointer next */
    six_nop_nop(OP_TBLWTL_W0_IW0);
    six_nop_nop(OP_BSET_NVMCON_WR);
    /* 211 ms measured. DS39768 P11 says >= 400 ms, so allow plenty. */
    pic_status_t st = wait_wr(3000, elapsed_ms);
    reset_pc();
    return st;
}

/* Program one 64-instruction row (row 0..343). */
pic_status_t pic_write_row(uint32_t row, const uint32_t words[PIC_ROW_WORDS]) {
    uint32_t addr = row * (PIC_ROW_WORDS * 2u);
    exit_reset_vector();
    icsp_six(op_mov_lit(NVMCON_ROW_WRITE, 10));
    icsp_six(op_mov_w_f(10, SFR_NVMCON));
    icsp_six(op_mov_lit(addr >> 16, 0));
    icsp_six(op_mov_w_f(0, SFR_TBLPAG));
    icsp_six(op_mov_lit(addr & 0xFFFFu, 7));
    for (uint32_t i = 0; i < PIC_ROW_WORDS; i += 4) {
        const uint32_t *w = &words[i];
        /* Packed format of DS39768 figure 3-6. */
        icsp_six(op_mov_lit(w[0] & 0xFFFFu, 0));
        icsp_six(op_mov_lit(((w[1] >> 8) & 0xFF00u) | ((w[0] >> 16) & 0xFFu), 1));
        icsp_six(op_mov_lit(w[1] & 0xFFFFu, 2));
        icsp_six(op_mov_lit(w[2] & 0xFFFFu, 3));
        icsp_six(op_mov_lit(((w[3] >> 8) & 0xFF00u) | ((w[2] >> 16) & 0xFFu), 4));
        icsp_six(op_mov_lit(w[3] & 0xFFFFu, 5));
        icsp_six(OP_CLR_W6);
        nop();
        six_nop_nop(OP_TBLWTL_W6PP_IW7);
        six_nop_nop(OP_TBLWTHB_W6PP_W7PP);
        six_nop_nop(OP_TBLWTHB_W6PP_PPW7);
        six_nop_nop(OP_TBLWTL_W6PP_W7PP);
        six_nop_nop(OP_TBLWTL_W6PP_IW7);
        six_nop_nop(OP_TBLWTHB_W6PP_W7PP);
        six_nop_nop(OP_TBLWTHB_W6PP_PPW7);
        six_nop_nop(OP_TBLWTL_W6PP_W7PP);
    }
    six_nop_nop(OP_BSET_NVMCON_WR);
    pic_status_t st = wait_wr(100, NULL); /* P13 >= 2 ms */
    reset_pc();
    return st;
}

/* Read 'count' instructions starting at instruction index 'first' (both even). */
void pic_read_words(uint32_t first, uint32_t count, uint32_t *out) {
    uint32_t addr = first * 2u;
    exit_reset_vector();
    init_table_read(addr);
    for (uint32_t i = 0; i < count; i += 2) {
        six_nop_nop(OP_TBLRDL_IW6_IW7);
        uint16_t lsw0 = regout_nop();
        six_nop_nop(OP_TBLRDHB_W6PP_W7PP);
        six_nop_nop(OP_TBLRDHB_PPW6_W7MM);
        uint16_t msb1_msb0 = regout_nop();
        six_nop_nop(OP_TBLRDL_W6PP_IW7);
        uint16_t lsw1 = regout_nop();
        reset_pc();
        out[i] = lsw0 | ((uint32_t)(msb1_msb0 & 0xFFu) << 16);
        out[i + 1] = lsw1 | ((uint32_t)(msb1_msb0 >> 8) << 16);
    }
}

/* =========================================================== 7. operations */
/*
 * The operations, one per console command. Each one reports on stdout and leaves
 * the target in a defined state. Those that talk to the PIC open their own ICSP
 * session.
 */

#define MAX_REPORTED_MISMATCHES 8u
#define FLASH_ATTEMPTS 3

/* Intel HEX record types */
#define HEX_DATA 0u
#define HEX_END_OF_FILE 1u
#define HEX_EXTENDED_LINEAR_ADDRESS 4u

typedef struct {
    const char *name;
    uint32_t (*word)(const void *ctx, uint32_t index); /* expected 24-bit word at instruction index */
    const void *ctx;
} word_source_t;

typedef struct {
    uint32_t seed;
    bool zeros;
} pattern_t;

static void (*on_busy)(void);
static uint32_t row_buf[PIC_ROW_WORDS];

/* hook (NULL for none) runs once per row of a long operation: the console blinks its LED. */
void ops_set_busy_hook(void (*hook)(void)) { on_busy = hook; }

static void busy(void) {
    if (on_busy) {
        on_busy();
    }
}

/* ------------------------------------------------------------------ sources */

static uint32_t image_source_word(const void *ctx, uint32_t index) {
    uint32_t w = image_word(ctx, index);
    if (index == PIC_CW1_INDEX || index == PIC_CW2_INDEX) {
        /* DS39881 24.1: the unimplemented upper byte should be 1111 1111. */
        w |= 0xFF0000u;
    }
    return w;
}

/* Erased words. CW1's reserved bit 15 reads 0 even then: pic_word_mask() leaves it out. */
static uint32_t blank_source_word(const void *ctx, uint32_t index) {
    (void)ctx;
    (void)index;
    return PIC_BLANK_WORD;
}

/* lowbias32 integer hash (C. Wellons): the random pattern is a function of
 * (seed, index), so the verify pass regenerates it without storing it. */
static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* Configuration values that keep the chip readable and reprogrammable:
 * CW1: r15=0, JTAGEN=0, GCP=1, GWRP=1, DEBUG=1, r10=1, ICS=11,
 *      WINDIS=1 (WINDIS=0 needs FWDTEN=1), r5=1.
 * CW2: WUTSEL/SOSCSEL never x0, r3=1, FNOSC never 110. */
static uint32_t safe_cw1(uint32_t v) {
    v |= (1u << 13) | (1u << 12) | (1u << 11) | (1u << 10) | (3u << 8) | (1u << 6) | (1u << 5);
    v &= ~((1u << 15) | (1u << 14));
    return 0xFF0000u | (v & 0xFFFFu);
}

static uint32_t safe_cw2(uint32_t v) {
    v |= (1u << 13) | (1u << 11) | (1u << 3);
    if (((v >> 8) & 7u) == 6u) {
        v &= ~(1u << 9);
    }
    return 0xFF0000u | (v & 0xFFFFu);
}

static uint32_t pattern_source_word(const void *ctx, uint32_t index) {
    const pattern_t *p = ctx;
    uint32_t w = p->zeros ? 0u : (mix32(p->seed ^ (index * 0x9E3779B9u)) & 0xFFFFFFu);
    if (index == PIC_CW2_INDEX) return safe_cw2(w);
    if (index == PIC_CW1_INDEX) return safe_cw1(w);
    return w;
}

static const word_source_t blank_source = {"blank", blank_source_word, NULL};

/* ------------------------------------------------------------------ helpers */

static void print_progress(const char *what, uint32_t done, uint32_t total, uint32_t *last_pct) {
    uint32_t pct = done * 100u / total;
    if (pct / 10u != *last_pct / 10u || done == total) {
        printf("  %s %3lu%%\n", what, (unsigned long)pct);
        *last_pct = pct;
    }
}

/* Silicon revision of a DEVREV value (errata DS80000470J table 1). */
static const char *rev_name(uint16_t rev) {
    switch (rev) {
        case 0x3003: return "A3/A4";
        case 0x3042: return "B4";
        case 0x3043: return "B5";
        case 0x3046: return "B8";
        default: return "unknown";
    }
}

/* Print "ERROR: <status>", then the hint, what to do about it (whole lines, or ""). Returns st. */
static pic_status_t print_error(pic_status_t st, const char *hint) {
    printf("ERROR: %s\n", pic_status_str(st));
    printf("%s", hint);
    return st;
}

/* The hint of every operation that finds no target (PIC_ERR_NO_TARGET). */
static const char no_target_hint[] = "  Power the Bus Pirate from its own USB cable, check GND and the MCLR wire,\n"
                                     "  and make sure no other programmer is holding the PIC in reset.\n";

/* What a failed flash, self-test or erase says once pic_try_hold_in_reset() has run. */
static void print_hold_result(bool held) {
    printf("%s", held ? "  The PIC is held in reset. Fix the problem and run the command again.\n"
                      : "  The PIC could not be held in reset: something else drives MCLR.\n");
}

/* Open a session and print the target, or what went wrong and what to do about it.
 * A session that does not open releases every line, unless held is set. A flash
 * attempt sets held once the chip is no longer what it was: the PIC is then held in
 * reset at once, *held says whether that worked, and no hint says "Nothing was written." */
static pic_status_t open_session(bool *held) {
    uint16_t id = 0, rev = 0;
    pic_status_t st = pic_session_begin(&id, &rev);
    if (held && st != PIC_OK) {
        *held = pic_try_hold_in_reset();
    }
    if (st == PIC_ERR_NO_TARGET) {
        return print_error(st, no_target_hint);
    }
    const char *nothing_written = held ? "" : "Nothing was written. ";
    if (st == PIC_ERR_LINES) {
        print_error(st, "");
        printf("  %sCheck for shorts; if a PICkit is still connected,\n"
               "  disconnect it or release it from reset (MPLAB IPE / ipecmd -OL).\n", nothing_written);
        return st;
    }
    if (st == PIC_ERR_NO_RESPONSE) {
        print_error(st, "");
        printf("  %sCheck that PGC and PGD are not swapped, the GND wire,\n"
               "  and try 'speed slow'.\n", nothing_written);
        return st;
    }
    printf("Target: DEVID=0x%04X DEVREV=0x%04X", id, rev);
    if (st == PIC_ERR_DEVID) {
        if (!held) {
            pic_session_end();
            pic_release_target(); /* nothing was written: leave the board as it was */
        }
        printf("\nERROR: expected DEVID 0x%04X (PIC24FJ64GA002).\n", PIC_DEVID_GA002);
        printf("  Check PGC/PGD (not swapped), GND and that the board is a Bus Pirate v3.\n");
        return st;
    }
    printf(" = PIC24FJ64GA002 rev %s\n", rev_name(rev));
    return PIC_OK;
}

static pic_status_t program_rows(const word_source_t *src, bool skip_blank, uint32_t *written) {
    uint32_t last_pct = 0, count = 0;
    for (uint32_t row = 0; row < PIC_ROWS; row++) {
        bool blank = true;
        for (uint32_t k = 0; k < PIC_ROW_WORDS; k++) {
            row_buf[k] = src->word(src->ctx, row * PIC_ROW_WORDS + k);
            blank = blank && row_buf[k] == PIC_BLANK_WORD;
        }
        if (!(skip_blank && blank)) {
            pic_status_t st = pic_write_row(row, row_buf);
            if (st != PIC_OK) {
                printf("  row %lu (PC 0x%06lX): %s\n", (unsigned long)row,
                       (unsigned long)(row * PIC_ROW_WORDS * 2u), pic_status_str(st));
                return st;
            }
            count++;
        }
        busy();
        print_progress("programming", row + 1, PIC_ROWS, &last_pct);
    }
    *written = count;
    return PIC_OK;
}

static pic_status_t compare_chip(const word_source_t *src, const char *what, uint32_t *mismatches) {
    uint32_t last_pct = 0, count = 0;
    for (uint32_t first = 0; first < PIC_FLASH_WORDS; first += PIC_ROW_WORDS) {
        pic_read_words(first, PIC_ROW_WORDS, row_buf);
        for (uint32_t k = 0; k < PIC_ROW_WORDS; k++) {
            uint32_t index = first + k;
            uint32_t mask = pic_word_mask(index);
            uint32_t expected = src->word(src->ctx, index) & mask;
            uint32_t actual = row_buf[k] & mask;
            if (expected != actual) {
                if (count < MAX_REPORTED_MISMATCHES) {
                    printf("  mismatch at PC 0x%06lX: expected %06lX, read %06lX\n",
                           (unsigned long)(index * 2u), (unsigned long)expected,
                           (unsigned long)actual);
                }
                count++;
            }
        }
        busy();
        print_progress(what, first + PIC_ROW_WORDS, PIC_FLASH_WORDS, &last_pct);
    }
    *mismatches = count;
    return count ? PIC_ERR_VERIFY : PIC_OK;
}

/* Compare the whole chip with erased words. *programmed gets how many are not erased. */
static pic_status_t blank_check(uint32_t *programmed) {
    return compare_chip(&blank_source, "blank check", programmed) == PIC_OK ? PIC_OK : PIC_ERR_NOT_BLANK;
}

static pic_status_t erase_and_blank_check(void) {
    uint32_t erase_ms = 0, programmed = 0;
    printf("Erasing (user memory only, calibration preserved)...\n");
    pic_status_t st = pic_chip_erase(&erase_ms);
    if (st != PIC_OK) {
        printf("  erase failed: %s\n", pic_status_str(st));
        return st;
    }
    printf("  erase done in %lu ms\n", (unsigned long)erase_ms);
    printf("Blank check of %u words...\n", (unsigned)PIC_FLASH_WORDS);
    st = blank_check(&programmed);
    if (st != PIC_OK) {
        printf("  NOT BLANK: %lu words differ\n", (unsigned long)programmed);
        return st;
    }
    printf("  blank\n");
    return PIC_OK;
}

static pic_status_t write_and_verify(const word_source_t *src, bool skip_blank) {
    uint32_t rows = 0, mismatches = 0;
    uint32_t start_ms = hal_now_ms();
    printf("Programming %s...\n", src->name);
    pic_status_t st = program_rows(src, skip_blank, &rows);
    if (st != PIC_OK) {
        return st;
    }
    printf("  %lu rows written in %lu ms\n", (unsigned long)rows, (unsigned long)(hal_now_ms() - start_ms));
    start_ms = hal_now_ms();
    printf("Verifying all %u words...\n", (unsigned)PIC_FLASH_WORDS);
    st = compare_chip(src, "verify", &mismatches);
    if (st != PIC_OK) {
        printf("  VERIFY FAILED: %lu words differ\n", (unsigned long)mismatches);
        return st;
    }
    printf("  verify OK in %lu ms (%u words, %u bytes)\n", (unsigned long)(hal_now_ms() - start_ms),
           (unsigned)PIC_FLASH_WORDS, (unsigned)IMAGE_BYTES);
    return PIC_OK;
}

/* Erase, blank check, write src and verify every word. */
static pic_status_t erase_write_verify(const word_source_t *src, bool skip_blank) {
    pic_status_t st = erase_and_blank_check();
    if (st == PIC_OK) {
        st = write_and_verify(src, skip_blank);
    }
    return st;
}

/* Found when the session opens, and a retry cannot fix them: power, wiring, another chip. */
static bool session_refused(pic_status_t st) {
    return st == PIC_ERR_NO_TARGET || st == PIC_ERR_LINES || st == PIC_ERR_DEVID;
}

/* Up to FLASH_ATTEMPTS sessions, each at half the PGC clock of the one before. The
 * clock is restored at the end. *written is set when an attempt starts the erase, or
 * before that by the caller. From then on, every failure holds the PIC in reset as
 * soon as it is seen, because the PIC must not run a half-written flash. *held says
 * whether the last hold worked. */
static pic_status_t flash_with_retries(const image_t *img, bool *written, bool *held) {
    const word_source_t src = {img->name, image_source_word, img};
    const icsp_timing_t saved = icsp_get_timing();
    pic_status_t st = PIC_ERR_NO_TARGET;
    for (int attempt = 1; attempt <= FLASH_ATTEMPTS; attempt++) {
        if (attempt > 1) {
            if (!image_ok(img)) {
                st = PIC_ERR_IMAGE; /* it changed during the last attempt: never write it again */
                printf("Attempt %d not started: %s\n", attempt, pic_status_str(st));
                break;
            }
            icsp_timing_t t = icsp_get_timing();
            icsp_set_timing(t.low_ns * 2u, t.high_ns * 2u);
            printf("Attempt %d of %d with a slower clock (%lu ns per phase)...\n", attempt,
                   FLASH_ATTEMPTS, (unsigned long)icsp_get_timing().low_ns);
        }
        st = open_session(*written ? held : NULL);
        if (session_refused(st)) {
            break;
        }
        if (st != PIC_OK) {
            printf("Attempt %d failed: %s\n", attempt, pic_status_str(st));
            continue; /* no answer: try again with a slower clock */
        }
        *written = true;
        st = erase_write_verify(&src, true);
        if (st == PIC_OK && !image_ok(img)) {
            /* The image changed in memory while it was written: the verify proved nothing. */
            st = PIC_ERR_IMAGE;
        }
        if (st == PIC_OK) {
            pic_session_end();
            break;
        }
        *held = pic_try_hold_in_reset();
        printf("Attempt %d failed: %s\n", attempt, pic_status_str(st));
        if (st == PIC_ERR_IMAGE) {
            break;
        }
    }
    icsp_set_timing(saved.low_ns, saved.high_ns);
    return st;
}

/* A bootloader with nothing to start: reset jumps to it and its GOTO to the
 * firmware (0x00A7FC) is still erased. */
static bool bootloader_without_firmware(const image_t *img) {
    return image_word(img, 0) == 0x04A800u && image_word(img, 1) == 0u &&
           image_word(img, IMAGE_USRAPP_INDEX) == PIC_BLANK_WORD &&
           image_word(img, IMAGE_USRAPP_INDEX + 1u) == PIC_BLANK_WORD;
}

static void print_hex_record(uint8_t type, uint16_t addr, const uint8_t *data, uint8_t len) {
    uint8_t sum = (uint8_t)(len + (addr >> 8) + (addr & 0xFFu) + type);
    printf(":%02X%04X%02X", len, addr, type);
    for (uint8_t i = 0; i < len; i++) {
        printf("%02X", data[i]);
        sum = (uint8_t)(sum + data[i]);
    }
    printf("%02X\n", (uint8_t)(0u - sum));
}

/* ------------------------------------------------------------------ public */

pic_status_t ops_identify(void) {
    pic_status_t st = open_session(NULL);
    if (st != PIC_OK) {
        return st;
    }
    uint32_t cw[2];
    pic_read_words(PIC_CW2_INDEX, 2, cw);
    printf("Config: CW2=0x%04lX CW1=0x%04lX\n", (unsigned long)(cw[0] & 0xFFFFu),
           (unsigned long)(cw[1] & 0xFFFFu));
    /* Read-only look at executive memory: these words must never change. */
    uint32_t cal[PIC_EXEC_CAL_WORDS];
    pic_read_words(PIC_EXEC_CAL_INDEX, PIC_EXEC_CAL_WORDS, cal);
    printf("Diagnostic/calibration words @0x8007F0:");
    for (uint32_t i = 0; i < PIC_EXEC_CAL_WORDS; i++) {
        printf(" %06lX", (unsigned long)cal[i]);
    }
    printf("\n");
    pic_session_end();
    pic_release_target();
    return PIC_OK;
}

/* The unbrick: erase, blank check, program the image, verify every word,
 * release the PIC so the Bus Pirate boots. Retries at a slower clock. The
 * image's CRC is checked before and after writing. written: the chip has lost
 * its contents already (the self-test's patterns), so a failure holds the PIC
 * in reset from the first attempt on. */
static pic_status_t flash_image(const image_t *img, bool written) {
    if (!image_ok(img)) {
        return print_error(PIC_ERR_IMAGE, "");
    }
    uint32_t start_ms = hal_now_ms();
    printf("Image: %s (crc32 %08lX, %lu rows used)\n", img->name, (unsigned long)img->crc32,
           (unsigned long)img->used_rows);
    bool held = false;
    pic_status_t st = flash_with_retries(img, &written, &held);
    if (st != PIC_OK) {
        printf("FAILED: %s\n", pic_status_str(st));
        if (held && st == PIC_ERR_NO_TARGET) {
            /* MCLR read low before the Pico drove it: whether its wire reaches the PIC is unknown. */
            printf("  MCLR is held low: the PIC stays in reset if the MCLR wire is connected.\n"
                   "  Fix the problem and run the command again.\n");
        } else if (written) {
            print_hold_result(held);
        }
        return st;
    }
    pic_release_target();
    printf("SUCCESS: %s written and verified in %lu ms.", img->name, (unsigned long)(hal_now_ms() - start_ms));
    if (bootloader_without_firmware(img)) {
        printf(" Only the bootloader is on the PIC:\n"
               "  no firmware runs yet. To load one with buzzloader: bp3mitloader waits for it; the\n"
               "  ds30 bootloader needs 'jumper' (or a PGC-PGD jumper) first, then 'release'.\n");
    } else {
        printf(" The Bus Pirate is running.\n");
    }
    return PIC_OK;
}

pic_status_t ops_flash(const image_t *img) { return flash_image(img, false); }

/* ops_flash() with a bootloader alone (section 3): its reset GOTO and its page,
 * every other word erased. Refused for any other image: never a whole image, and
 * never one sent by the computer. */
pic_status_t ops_bootloader(const image_t *bl) {
    if (!image_ok(bl)) {
        return print_error(PIC_ERR_IMAGE, "");
    }
    if (!bl->bootloader_only) {
        printf("ERROR: %s is not a bootloader alone: not written\n", bl->name);
        return PIC_ERR_IMAGE;
    }
    uint32_t words = 0;
    for (uint32_t i = 0; i < PIC_FLASH_WORDS; i++) {
        words += image_word(bl, i) != PIC_BLANK_WORD;
    }
    printf("Bootloader: %s, %lu words; every other word stays erased\n", bl->name, (unsigned long)words);
    return ops_flash(bl);
}

pic_status_t ops_verify(const image_t *img) {
    if (!image_ok(img)) {
        return print_error(PIC_ERR_IMAGE, "");
    }
    pic_status_t st = open_session(NULL);
    if (st != PIC_OK) {
        return st;
    }
    uint32_t mismatches = 0;
    const word_source_t src = {img->name, image_source_word, img};
    printf("Verifying against %s...\n", img->name);
    st = compare_chip(&src, "verify", &mismatches);
    pic_session_end();
    pic_release_target();
    if (st == PIC_OK) {
        printf("VERIFY OK: all %u words match %s\n", (unsigned)PIC_FLASH_WORDS, img->name);
    } else {
        printf("VERIFY FAILED: %lu words differ\n", (unsigned long)mismatches);
    }
    return st;
}

pic_status_t ops_blank_check(void) {
    pic_status_t st = open_session(NULL);
    if (st != PIC_OK) {
        return st;
    }
    uint32_t programmed = 0;
    st = blank_check(&programmed);
    pic_session_end();
    pic_release_target();
    if (st == PIC_OK) {
        printf("BLANK: all %u words erased (CW1 reads 0x7FFF: reserved bit 15)\n", (unsigned)PIC_FLASH_WORDS);
    } else {
        printf("NOT BLANK: %lu words programmed\n", (unsigned long)programmed);
    }
    return st;
}

/* Erase and blank check. The Bus Pirate is left without firmware. A failed erase holds
 * the PIC in reset, like a failed flash: what is left of its program must not run. */
pic_status_t ops_erase(void) {
    pic_status_t st = open_session(NULL);
    if (st != PIC_OK) {
        return st;
    }
    st = erase_and_blank_check();
    if (st != PIC_OK) {
        bool held = pic_try_hold_in_reset();
        printf("ERASE FAILED: %s\n", pic_status_str(st));
        print_hold_result(held);
        return st;
    }
    pic_session_end();
    pic_release_target();
    printf("ERASED: the Bus Pirate has no firmware now (run 'flash')\n");
    return PIC_OK;
}

/* Read the whole flash and print it as Intel HEX (MPLAB layout). */
pic_status_t ops_dump(void) {
    pic_status_t st = open_session(NULL);
    if (st != PIC_OK) {
        return st;
    }
    uint32_t crc = 0;
    uint8_t upper[2] = {0, 0}; /* extended linear address: the upper 16 bits of the byte address */
    printf("BEGIN HEX\n");
    print_hex_record(HEX_EXTENDED_LINEAR_ADDRESS, 0, upper, 2); /* the first one before any read */
    for (uint32_t first = 0; first < PIC_FLASH_WORDS; first += PIC_ROW_WORDS) {
        pic_read_words(first, PIC_ROW_WORDS, row_buf);
        for (uint32_t k = 0; k < PIC_ROW_WORDS; k += 4) {
            uint8_t line[16];
            for (uint32_t j = 0; j < 4; j++) {
                uint32_t index = first + k + j;
                uint32_t w = row_buf[k + j];
                if (index == PIC_CW1_INDEX || index == PIC_CW2_INDEX) {
                    w &= 0xFFFFu; /* same layout as an MPLAB IPE read */
                }
                word_to_bytes(w, &line[4 * j]);
                line[4 * j + 3] = 0;
                crc = crc32_update(crc, &line[4 * j], 3);
            }
            uint32_t byte_addr = (first + k) * 4u;
            if ((byte_addr & 0xFFFFu) == 0 && byte_addr) { /* every further 64 KB */
                upper[0] = (uint8_t)(byte_addr >> 24);
                upper[1] = (uint8_t)(byte_addr >> 16);
                print_hex_record(HEX_EXTENDED_LINEAR_ADDRESS, 0, upper, 2);
            }
            print_hex_record(HEX_DATA, (uint16_t)byte_addr, line, 16);
        }
        busy();
    }
    print_hex_record(HEX_END_OF_FILE, 0, NULL, 0);
    printf("END HEX words=%u crc32=%08lX (embedded image crc32=%08lX)\n", (unsigned)PIC_FLASH_WORDS,
           (unsigned long)crc, (unsigned long)embedded_image.crc32);
    pic_session_end();
    pic_release_target();
    return PIC_OK;
}

/* Full-flash test: random words, then all zeros (every bit programmed), each pass
 * erased, written and verified. It ends with a flash of the built-in image. */
pic_status_t ops_selftest(uint32_t seed) {
    uint32_t start_ms = hal_now_ms();
    printf("Full-flash self test, random seed 0x%08lX\n", (unsigned long)seed);
    pic_status_t st = open_session(NULL);
    if (st != PIC_OK) {
        return st;
    }
    const pattern_t random_words = {seed, false}, zero_words = {0, true};
    const word_source_t passes[] = {{"random pattern", pattern_source_word, &random_words},
                                    {"all-zeros pattern", pattern_source_word, &zero_words}};
    for (unsigned p = 0; p < sizeof passes / sizeof passes[0] && st == PIC_OK; p++) {
        printf("--- pass %u: %s over 0x000000..0x00ABFE ---\n", p + 1, passes[p].name);
        st = erase_write_verify(&passes[p], false); /* every row, even a blank one */
    }
    if (st != PIC_OK) {
        bool held = pic_try_hold_in_reset();
        printf("SELFTEST FAILED: %s\n", pic_status_str(st));
        print_hold_result(held);
        return st;
    }
    pic_session_end();
    printf("SELFTEST PASSED in %lu ms: every word erased, written and read back twice.\n",
           (unsigned long)(hal_now_ms() - start_ms));
    printf("--- restoring bootloader + firmware ---\n");
    return flash_image(&embedded_image, true); /* the chip holds the test pattern */
}

/* Hold PGC low, like the Bus Pirate's PGC-PGD jumper, and restart the PIC: its
 * bootloader stays in bootloader mode (for buzzloader) until ops_release() or the
 * next operation. 'status' leaves it in place. */
pic_status_t ops_jumper(void) {
    if (!pic_target_present()) {
        return print_error(PIC_ERR_NO_TARGET, no_target_hint);
    }
    pic_jumper_reset();
    printf("PGC held low like the PGC-PGD jumper, PIC restarted: its bootloader waits in\n"
           "bootloader mode (MODE LED on). Run buzzloader now, then 'release'.\n");
    return PIC_OK;
}

/* Release every line and let the PIC run: it does if the board pulls MCLR up (powered). */
pic_status_t ops_release(void) {
    pic_release_target();
    hal_delay_ms(2); /* as pic_target_present(): the pull-up has raised MCLR by then */
    if (mclr_pulled_up()) {
        printf("Lines released, Bus Pirate running\n");
    } else {
        printf("Lines released, but %s\n", pic_status_str(PIC_ERR_NO_TARGET));
    }
    return PIC_OK;
}

/* =============================================================== 8. upload */
/*
 * Image upload from the computer (bp3unbricker.py), one text line at a time:
 *
 *   load name                         start (the console command, section 10),
 *                                     answered with "LOAD READY 344 64"
 *                                     (PIC_ROWS, PIC_ROW_WORDS)
 *   R rrr wwwwww...wwwwww cccccccc    row rrr (3 hex digits), its 64 words
 *                                     (6 hex digits each) and the CRC-32 of
 *                                     those words, 3 bytes each, little-endian
 *   E cccccccc                        end: the CRC-32 of the whole image (IMAGE_BYTES)
 *   X                                 abort
 *
 * Every line gets one reply: "OK rrr" or "ERR rrr <reason>" (send the row
 * again) for a row, "ERR END format" for a malformed end line, "ERR ??? format"
 * for a row whose rrr cannot be read and for any other line (the upload goes on
 * after all of these), and "LOAD OK ...", "LOAD ERR ..." or "LOAD ABORTED" at
 * the end. Unsent rows stay erased (0xFFFFFF). A row's CRC does not cover rrr:
 * the final CRC, over every word, catches a missing row or one stored under the
 * wrong number before the image can be used.
 */

#define LOADER_LINE_MAX 512 /* longest line accepted, newline excluded */

typedef enum { LOADER_RECEIVING, LOADER_DONE, LOADER_FAILED } loader_state_t;

/* Image names: what safe_name() in bp3unbricker.py sends (letters, digits and
 * ._-+(), at most 47 characters). bp3unbricker.py expects the name back in
 * "LOAD OK <name>". */
#define NAME_MAX_LEN 47
#define ROW_BYTES (PIC_ROW_WORDS * 3u)
#define ROW_HEX_DIGITS (PIC_ROW_WORDS * 6u)
#define ROW_WORDS_POS 6u                                  /* after "R rrr " */
#define ROW_CRC_POS (ROW_WORDS_POS + ROW_HEX_DIGITS + 1u) /* after the words and a space */
#define ROW_LINE_LEN (ROW_CRC_POS + 8u)
#define END_LINE_LEN 10u                                  /* "E cccccccc" */

/* The upload in progress, or the last one. Zero-initialized, so the 66 KB stay
 * in .bss: loader_begin() points image at name and data. */
static struct {
    bool receiving; /* from loader_begin() to the end line, "X" or loader_abort() */
    bool valid;     /* the last upload ended with a matching image CRC */
    image_t image;
    char name[NAME_MAX_LEN + 1];
    uint8_t data[IMAGE_BYTES];
} upload;

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Exactly n hex digits at s. */
static bool parse_hex(const char *s, unsigned n, uint32_t *out) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        int d = hex_digit(s[i]);
        if (d < 0) return false;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

static bool name_char_ok(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' ||
           c == '_' || c == '-' || c == '+' || c == '(' || c == ')';
}

/* Forget the previous image and start receiving a new one. */
void loader_begin(const char *name) {
    size_t n = 0;
    for (const char *p = name ? name : ""; *p && n < NAME_MAX_LEN; p++) {
        upload.name[n++] = name_char_ok(*p) ? *p : '_';
    }
    upload.name[n] = 0;
    if (!n) strcpy(upload.name, "uploaded-image");
    memset(upload.data, 0xFF, sizeof upload.data);
    upload.image = (image_t){.name = upload.name, .data = upload.data};
    upload.valid = false;
    upload.receiving = true;
}

/* Give up on the upload in progress (the computer stopped sending). */
void loader_abort(void) { upload.receiving = false; }

/* The uploaded image, or NULL until an upload has ended with a matching CRC. */
const image_t *loader_image(void) { return upload.valid ? &upload.image : NULL; }

/* The 64 words of a row line, 6 hex digits each, as 3 bytes little-endian each. */
static bool parse_row_words(const char *hex, uint8_t bytes[ROW_BYTES]) {
    for (uint32_t k = 0; k < PIC_ROW_WORDS; k++) {
        uint32_t w;
        if (!parse_hex(hex + 6 * k, 6, &w)) return false;
        word_to_bytes(w, &bytes[3 * k]);
    }
    return true;
}

/* True if the row has at least one programmed byte (not 0xFF). */
static bool row_used(uint32_t row) {
    for (uint32_t i = 0; i < ROW_BYTES; i++) {
        if (upload.data[row * ROW_BYTES + i] != 0xFFu) return true;
    }
    return false;
}

static loader_state_t handle_row_line(const char *line, char *reply, size_t reply_size) {
    size_t len = strlen(line);
    uint32_t row = 0, crc = 0;
    uint8_t bytes[ROW_BYTES];
    if (len < 5 || line[1] != ' ' || !parse_hex(line + 2, 3, &row)) {
        snprintf(reply, reply_size, "ERR ??? format");
        return LOADER_RECEIVING;
    }
    if (row >= PIC_ROWS) {
        snprintf(reply, reply_size, "ERR %03lX row out of range", (unsigned long)row);
        return LOADER_RECEIVING;
    }
    if (len != ROW_LINE_LEN || line[ROW_WORDS_POS - 1] != ' ' || line[ROW_CRC_POS - 1] != ' ' ||
        !parse_hex(line + ROW_CRC_POS, 8, &crc) || !parse_row_words(line + ROW_WORDS_POS, bytes)) {
        snprintf(reply, reply_size, "ERR %03lX format", (unsigned long)row);
        return LOADER_RECEIVING;
    }
    if (crc32_update(0, bytes, sizeof bytes) != crc) {
        snprintf(reply, reply_size, "ERR %03lX crc", (unsigned long)row);
        return LOADER_RECEIVING;
    }
    memcpy(&upload.data[row * ROW_BYTES], bytes, sizeof bytes);
    snprintf(reply, reply_size, "OK %03lX", (unsigned long)row);
    return LOADER_RECEIVING;
}

static loader_state_t handle_end_line(const char *line, char *reply, size_t reply_size) {
    uint32_t expected = 0;
    if (strlen(line) != END_LINE_LEN || line[1] != ' ' || !parse_hex(line + 2, 8, &expected)) {
        snprintf(reply, reply_size, "ERR END format");
        return LOADER_RECEIVING;
    }
    upload.receiving = false;
    uint32_t crc = crc32_update(0, upload.data, sizeof upload.data);
    if (crc != expected) {
        snprintf(reply, reply_size, "LOAD ERR crc32 %08lX, expected %08lX", (unsigned long)crc,
                 (unsigned long)expected);
        return LOADER_FAILED;
    }
    uint32_t used = 0;
    for (uint32_t row = 0; row < PIC_ROWS; row++) {
        used += row_used(row);
    }
    upload.image.crc32 = crc;
    upload.image.used_rows = used;
    upload.valid = true;
    snprintf(reply, reply_size, "LOAD OK %s crc32 %08lX, %lu rows used", upload.name, (unsigned long)crc,
             (unsigned long)used);
    return LOADER_DONE;
}

/* Handle one line (without its newline) and write the reply (without one). */
loader_state_t loader_line(const char *line, char *reply, size_t reply_size) {
    if (!upload.receiving) {
        snprintf(reply, reply_size, "LOAD ERR not receiving");
        return LOADER_FAILED;
    }
    if (line[0] == 'R') return handle_row_line(line, reply, reply_size);
    if (line[0] == 'E') return handle_end_line(line, reply, reply_size);
    if (!strcmp(line, "X")) {
        upload.receiving = false;
        snprintf(reply, reply_size, "LOAD ABORTED");
        return LOADER_FAILED;
    }
    snprintf(reply, reply_size, "ERR ??? format");
    return LOADER_RECEIVING;
}

/* ======================================================== 9. operation log */
#ifdef BP3_HOST_TEST
/* A host build runs sections 9 and 10 on copies of the built-in images. */
extern image_t host_embedded_image, host_ds30_image, host_mit_image;
#define embedded_image host_embedded_image
#define ds30_image host_ds30_image
#define mit_image host_mit_image
#endif

/*
 * Result and console output of the last operation, kept in RAM so that a run
 * started with the BOOTSEL button while no terminal was open can be read
 * afterwards: a summary line when a terminal connects, the full text with 'log'.
 */

#define OPLOG_SIZE 16384u /* power of two; a longer output keeps its end */

static char ring[OPLOG_SIZE];
static uint32_t ring_total; /* bytes printed by the operation */
static bool capturing;

static struct {
    bool valid;
    const char *name;
    const char *source;
    pic_status_t status;
    uint32_t duration_ms;
    uint32_t end_ms;
} last_op;

/* Gets the raw text (crlf_enabled is false): the replay goes through the
 * normal CR/LF translation of the USB driver. */
static void capture_out_chars(const char *buf, int len) {
    if (!capturing) return;
    for (int i = 0; i < len; i++) ring[ring_total++ % OPLOG_SIZE] = buf[i];
}

static stdio_driver_t capture_driver = {.out_chars = capture_out_chars};

/* Register the stdio driver that records the output. Call after stdio_init_all(). */
void oplog_init(void) { stdio_set_driver_enabled(&capture_driver, true); }

/* Start recording. Only the pointers are kept: name and source must be string literals. */
void oplog_begin(const char *name, const char *source) {
    last_op.valid = false;
    last_op.name = name;
    last_op.source = source;
    ring_total = 0;
    capturing = true;
}

/* Stop recording and keep the result for oplog_print_summary() and oplog_replay(). */
void oplog_end(pic_status_t st, uint32_t duration_ms) {
    capturing = false;
    last_op.status = st;
    last_op.duration_ms = duration_ms;
    last_op.end_ms = hal_now_ms();
    last_op.valid = true;
}

static void print_ago(void) {
    uint32_t s = (hal_now_ms() - last_op.end_ms) / 1000u;
    if (s < 120u) printf("%lu s ago", (unsigned long)s);
    else if (s < 7200u) printf("%lu min ago", (unsigned long)(s / 60u));
    else printf("%lu h ago", (unsigned long)(s / 3600u));
}

/* "Last operation: flash (BOOTSEL button), finished 35 s ago after 22.9 s: OK".
 * Print nothing and return false if no operation has run since power-up. */
bool oplog_print_summary(void) {
    if (!last_op.valid) return false;
    printf("Last operation: %s (%s), finished ", last_op.name, last_op.source);
    print_ago();
    printf(" after %lu.%lu s: ", (unsigned long)(last_op.duration_ms / 1000u),
           (unsigned long)(last_op.duration_ms % 1000u / 100u));
    if (last_op.status == PIC_OK) printf("OK\n");
    else printf("FAILED, %s\n", pic_status_str(last_op.status));
    return true;
}

/* Print everything the last operation printed. */
void oplog_replay(void) {
    if (!last_op.valid) {
        printf("No operation has run since the Pico was powered.\n");
        return;
    }
    printf("--- output of '%s' (%s), finished ", last_op.name, last_op.source);
    print_ago();
    printf(" ---\n");
    uint32_t kept = ring_total < OPLOG_SIZE ? ring_total : OPLOG_SIZE;
    if (ring_total > kept) printf("(the first %lu bytes were not kept)\n", (unsigned long)(ring_total - kept));
    for (uint32_t i = ring_total - kept; i < ring_total;) {
        uint32_t pos = i % OPLOG_SIZE;
        uint32_t n = OPLOG_SIZE - pos;
        if (n > ring_total - i) n = ring_total - i;
        stdio_put_string(&ring[pos], (int)n, false, true);
        i += n;
    }
}

/* ============================================================= 10. console */
/* The USB serial console, the LED codes and the BOOTSEL button. */

/* Whether each built-in image passed its CRC check at power-up. A command that would
 * write a corrupt one refuses to run. Each image is checked on its own: a corrupt
 * bootloader alone does not stop 'flash' or the other bootloader. */
static bool embedded_image_ok;
static bool ds30_image_ok, mit_image_ok;

/* ------------------------------------------------------------------ LED */

typedef enum { LED_IDLE, LED_BUSY, LED_OK, LED_FAIL } led_mode_t;

static led_mode_t led_mode = LED_IDLE;
static unsigned led_blinks; /* LED_FAIL: blinks per cycle */

/* The error code that the LED shows without a computer: 1 to 5 in the order in which a
 * flash session meets the errors, 6 for a corrupt image. */
static unsigned led_error_blinks(pic_status_t st) {
    switch (st) {
        case PIC_OK: return 0;              /* not an error: the LED stays on */
        case PIC_ERR_NO_TARGET: return 1;   /* Bus Pirate unpowered, MCLR or GND not wired */
        case PIC_ERR_LINES: return 2;       /* short, or another programmer connected */
        case PIC_ERR_NO_RESPONSE:
        case PIC_ERR_DEVID: return 3;       /* PGC/PGD not wired or swapped */
        case PIC_ERR_WR_TIMEOUT:
        case PIC_ERR_WRERR:
        case PIC_ERR_NOT_BLANK: return 4;   /* erase or write failed */
        case PIC_ERR_VERIFY: return 5;
        case PIC_ERR_IMAGE: return 6;       /* image corrupt (built-in or uploaded) */
    }
    return 6;
}

static void led_result(pic_status_t st) {
    led_mode = st == PIC_OK ? LED_OK : LED_FAIL;
    led_blinks = led_error_blinks(st);
}

static void led_update(bool force_on) {
    uint32_t now_ms = hal_now_ms();
    bool on = force_on;
    if (!force_on) {
        switch (led_mode) {
            case LED_IDLE: on = (now_ms % 1000u) < 50u; break;  /* short blink every second */
            case LED_BUSY: on = (now_ms / 100u) & 1u; break;    /* 5 Hz while working */
            case LED_OK: on = true; break;                      /* steady: success */
            case LED_FAIL: {                                    /* N blinks and a pause: error N */
                uint32_t blinks_ms = led_blinks * 400u;
                uint32_t phase_ms = now_ms % (blinks_ms + 1200u);
                on = phase_ms < blinks_ms && (phase_ms % 400u) < 200u;
                break;
            }
        }
    }
    gpio_put(PICO_DEFAULT_LED_PIN, on);
}

static void busy_hook(void) { led_update(false); }

/* ------------------------------------------------------------------ BOOTSEL */

#define BOOTSEL_HOLD_MS 1000u

/* QSPI chip select as seen in SIO gpio_hi_in. */
#if PICO_RP2040
#define QSPI_SS_IN_BIT (1u << 1)
#else
#define QSPI_SS_IN_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif

/* From pico-examples/picoboard/button: the BOOTSEL button pulls the QSPI chip
 * select low. Nothing may touch the flash while this function reads the button. */
static bool __no_inline_not_in_flash_func(bootsel_pressed)(void) {
    const uint cs_index = 1;
    uint32_t flags = save_and_disable_interrupts();
    hw_write_masked(&ioqspi_hw->io[cs_index].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i) {
    }
    bool pressed = !(sio_hw->gpio_hi_in & QSPI_SS_IN_BIT);
    hw_write_masked(&ioqspi_hw->io[cs_index].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}

/* ------------------------------------------------------------------ banner and help */

/* "RP2040 B2", "RP2350 A2": the boot ROM version gives the stepping. */
static const char *chip_name(void) {
    static char name[12];
#if PICO_RP2040
    snprintf(name, sizeof name, "RP2040 B%u", (unsigned)rp2040_rom_version() - 1u);
#else
    snprintf(name, sizeof name, "RP2350 A%u", (unsigned)rp2350_rom_version());
#endif
    return name;
}

static void print_loaded(void) {
    const image_t *img = loader_image();
    if (img) {
        printf("Loaded: %s, crc32 %08lX, %lu rows used (sent by the computer; 'flash loaded' writes it)\n",
               img->name, (unsigned long)img->crc32, (unsigned long)img->used_rows);
    }
}

/* The project's banner, the same as bp3unbricker.py's except for its second line, then
 * what this Pico holds. */
static void print_banner(void) {
    static const char rule[] = "------------------------------------------------------------------------------";
    printf("\n%s\n"
           " bp3unbricker %s for %s (%s)\n"
           " Bus Pirate v3 unbricker: programs its PIC24FJ64GA002 over ICSP\n"
           " https://github.com/therealdreg/bp3unbricker\n"
           " MIT License - Copyright (c) 2026 David Reguera Garcia aka Dreg\n"
           " X @therealdreg - https://www.rootkit.es - dreg@rootkit.es\n"
           "%s\n",
           rule, BP3_VERSION, BP3_BOARD, chip_name(), rule);
    printf("Wiring: GP%d->PGC  GP%d->PGD  GP%d->MCLR  GND->GND  (Bus Pirate powered by its own USB)\n",
           PIN_PGC, PIN_PGD, PIN_MCLR);
    printf("Image : %s, crc32 %08lX %s\n", embedded_image.name, (unsigned long)embedded_image.crc32,
           embedded_image_ok ? "OK" : "CORRUPT - flashing disabled");
    printf("        'bootloader ds30' writes only its bootloader, crc32 %08lX %s\n",
           (unsigned long)ds30_image.crc32, ds30_image_ok ? "OK" : "CORRUPT");
    printf("        'bootloader' writes only %s, crc32 %08lX %s\n", mit_image.name,
           (unsigned long)mit_image.crc32, mit_image_ok ? "OK" : "CORRUPT");
    print_loaded();
    printf("Type 'help' for commands, 'flash' to unbrick. BOOTSEL held 1 s = flash.\n");
    if (oplog_print_summary()) printf("Type 'log' to see its output.\n");
}

static void cmd_help(void) {
    printf("Commands:\n"
           "  flash         unbrick: erase, program bootloader+firmware, verify all, run\n"
           "  flash loaded  the same with the image sent by the computer ('load')\n"
           "  bootloader    the same, but only bp3mitloader ('bootloader ds30': only the\n"
           "                ds30 bootloader of the embedded image)\n"
           "  jumper        hold PGC low like the PGC-PGD jumper and restart the Bus Pirate:\n"
           "                its bootloader waits for buzzloader until 'release'\n"
           "  verify        compare the PIC with the embedded image ('verify loaded': the computer's)\n"
           "  load [name]   receive an image from the computer (bp3unbricker.py flash FILE.hex)\n"
           "  id            show device ID, revision, config and calibration words\n"
           "  blank         check that the PIC is erased\n"
           "  erase         erase the PIC (leaves the Bus Pirate without firmware)\n"
           "  dump          read the whole PIC and print it as Intel HEX\n"
           "  test [seed]   full-flash self test (random + zeros, every word), then flash\n"
           "  release       release all lines and let the Bus Pirate run\n"
           "  status        wiring, timing, whether the Bus Pirate is detected, last result\n"
           "  log           everything the last operation printed (also BOOTSEL runs)\n"
           "  speed slow|normal|fast   PGC clock: ~240 kHz / ~850 kHz / ~1.5 MHz\n"
           "  bootsel       reboot the Pico into its USB bootloader (UF2 update)\n"
           "Bring-up: ping (ICSP entry + VISI echo), pins, bench, entry, pgdpull,"
           " raw enter|key|six|regout|echo|exit|release\n");
}

/* ------------------------------------------------------------------ status and wiring check */

#define HEADER_GPIOS 29u /* GP0..GP28 */

/* Header pin of each GPIO on a Pico / Pico 2 (0: not on the header). */
static const uint8_t header_pin[HEADER_GPIOS] = {1,  2,  4,  5,  6,  7,  9,  10, 11, 12, 14, 15, 16, 17, 19,
                                                 20, 21, 22, 24, 25, 26, 27, 29, 0,  0,  0,  31, 32, 34};

/* Free header GPIOs that read high with a pull-down: something outside pulls them up. */
static uint32_t pulled_up_pins(void) {
    uint32_t found = 0;
    for (uint pin = 0; pin < HEADER_GPIOS; pin++) {
        if (!header_pin[pin] || pin == PIN_PGC || pin == PIN_PGD || pin == PIN_MCLR) continue;
        gpio_init(pin); /* input */
        gpio_pull_down(pin);
        busy_wait_us(200);
        if (gpio_get(pin)) found |= 1u << pin;
    }
    return found;
}

/* The Bus Pirate pulls MCLR up through 2k: when GP12 does not see it, find
 * the pin the wire went to instead. */
static void print_wiring_hint(void) {
    uint32_t up = pulled_up_pins();
    if (!up) {
        printf("  No free Pico pin sees the Bus Pirate's MCLR pull-up either: the MCLR wire does not\n"
               "  reach JP2-5 (loose or broken), or the Bus Pirate has no power.\n");
        return;
    }
    for (uint pin = 0; pin < HEADER_GPIOS; pin++) {
        if (up & (1u << pin)) {
            printf("  GP%u (pin %u) is pulled up from outside: is the MCLR wire there? It belongs on GP%d (pin %u).\n",
                   pin, header_pin[pin], PIN_MCLR, header_pin[PIN_MCLR]);
        }
    }
}

static void cmd_status(void) {
    icsp_timing_t t = icsp_get_timing();
    printf("bp3unbricker %s for %s (%s), image %s crc32 %08lX (%s, %lu rows used)\n", BP3_VERSION, BP3_BOARD,
           chip_name(), embedded_image.name, (unsigned long)embedded_image.crc32, embedded_image_ok ? "OK" : "CORRUPT",
           (unsigned long)embedded_image.used_rows);
    printf("Bootloader mit: %s, crc32 %08lX (%s, %lu rows used)\n", mit_image.name,
           (unsigned long)mit_image.crc32, mit_image_ok ? "OK" : "CORRUPT", (unsigned long)mit_image.used_rows);
    printf("Bootloader ds30: %s, crc32 %08lX (%s, %lu rows used)\n", ds30_image.name,
           (unsigned long)ds30_image.crc32, ds30_image_ok ? "OK" : "CORRUPT", (unsigned long)ds30_image.used_rows);
    print_loaded();
    printf("Pins: PGC=GP%d PGD=GP%d MCLR=GP%d, PGC low/high %lu/%lu ns\n", PIN_PGC, PIN_PGD, PIN_MCLR,
           (unsigned long)t.low_ns, (unsigned long)t.high_ns);
    /* 1000 calls: the total in us is the time of one call in ns. */
    uint64_t start_us = time_us_64();
    for (int i = 0; i < 1000; i++) {
        hal_delay_ns(1000);
    }
    printf("clk_sys %lu MHz, hal_delay_ns(1000) takes %lu ns\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000000u), (unsigned long)(time_us_64() - start_us));
    if (pic_reset_held()) {
        /* The probe would release MCLR: the PIC would run what the failed write left. Only the
         * Pico's pin is certain: after no target, the MCLR wire may not reach the PIC. */
        printf("Bus Pirate: not checked, MCLR held low after a failed write (until 'release')\n");
    } else {
        bool present = pic_target_present_keep_jumper();
        printf("Bus Pirate: %s\n", present ? "detected (MCLR pulled up, board powered)"
                                           : "NOT detected (no power, MCLR not wired, or held in reset)");
        if (!present) print_wiring_hint();
        if (pic_jumper_held()) {
            printf("Jumper: %s\n", present ? "PGC held low, the bootloader waits for buzzloader until 'release'"
                                           : "PGC held low until 'release'");
        }
    }
    if (!oplog_print_summary()) printf("Last operation: none since power-up\n");
}

/* ------------------------------------------------------------------ operations */

/* name and source are string literals: the operation log keeps the pointers.
 * source: who started it ("console", "BOOTSEL button"). */
static void run_op(const char *name, const char *source, pic_status_t (*fn)(void)) {
    uint32_t start_ms = hal_now_ms();
    led_mode = LED_BUSY;
    oplog_begin(name, source);
    pic_status_t st = fn();
    if (st == PIC_ERR_NO_TARGET) print_wiring_hint();
    led_result(st);
    uint32_t duration_ms = hal_now_ms() - start_ms;
    printf("[%s: %s, %lu ms]\n", name, pic_status_str(st), (unsigned long)duration_ms);
    oplog_end(st, duration_ms);
}

/* The arguments of the operation run_op() starts next: its functions take none. */
static struct {
    const image_t *image; /* flash, verify, bootloader */
    uint32_t seed;        /* test */
} op_args;

static pic_status_t do_test(void) { return ops_selftest(op_args.seed); }
static pic_status_t do_flash(void) { return ops_flash(op_args.image); }
static pic_status_t do_verify(void) { return ops_verify(op_args.image); }
static pic_status_t do_bootloader(void) { return ops_bootloader(op_args.image); }

/* bootloader [mit|ds30]: bp3mitloader alone, or the ds30 bootloader of the built-in image alone. */
static void cmd_bootloader(const char *arg) {
    bool ds30 = arg && !strcmp(arg, "ds30");
    if (arg && !ds30 && strcmp(arg, "mit")) {
        printf("usage: bootloader [mit|ds30]\n");
        return;
    }
    if (!(ds30 ? ds30_image_ok : mit_image_ok)) {
        printf("ERROR: %s\n", pic_status_str(PIC_ERR_IMAGE));
        return;
    }
    op_args.image = ds30 ? &ds30_image : &mit_image;
    run_op(ds30 ? "bootloader ds30" : "bootloader", "console", do_bootloader);
}

/* flash or verify with the built-in image, or with "loaded" the image from the computer. */
static void run_image_op(const char *arg, const char *name, const char *loaded_name, pic_status_t (*fn)(void)) {
    if (arg && strcmp(arg, "loaded")) {
        printf("usage: %s [loaded]\n", name);
        return;
    }
    op_args.image = arg ? loader_image() : embedded_image_ok ? &embedded_image : NULL;
    if (!op_args.image) {
        printf("ERROR: %s\n", arg ? "no image loaded (bp3unbricker.py flash FILE.hex sends one)"
                                   : pic_status_str(PIC_ERR_IMAGE));
        return;
    }
    run_op(arg ? loaded_name : name, "console", fn);
}

/* ------------------------------------------------------------------ upload */

#define LOAD_LINE_TIMEOUT_MS 10000u

/* One line without echo; '\r' is ignored. False if nothing completes it in
 * time. A line longer than the buffer comes back as "!" (rejected later). */
static bool read_raw_line(char *buf, size_t size, uint32_t timeout_ms) {
    size_t n = 0;
    bool overflow = false;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    for (;;) {
        int64_t left_us = absolute_time_diff_us(get_absolute_time(), deadline);
        if (left_us <= 0) return false;
        int c = getchar_timeout_us((uint32_t)left_us);
        if (c == PICO_ERROR_TIMEOUT) return false;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n < size - 1) buf[n++] = (char)c;
        else overflow = true;
    }
    if (overflow) {
        buf[0] = '!';
        n = 1;
    }
    buf[n] = 0;
    return true;
}

/* 'load': receive an image from the computer, see section 8 for the protocol. */
static void cmd_load(const char *name) {
    static char line[LOADER_LINE_MAX + 1];
    char reply[100];
    loader_begin(name);
    printf("LOAD READY %u %u\n", (unsigned)PIC_ROWS, (unsigned)PIC_ROW_WORDS);
    for (;;) {
        if (!read_raw_line(line, sizeof line, LOAD_LINE_TIMEOUT_MS)) {
            loader_abort();
            printf("LOAD ERR timeout\n");
            return;
        }
        loader_state_t st = loader_line(line, reply, sizeof reply);
        printf("%s\n", reply);
        if (st != LOADER_RECEIVING) return;
    }
}

/* ------------------------------------------------------------------ commands */

static void cmd_speed(const char *arg) {
    if (arg && !strcmp(arg, "slow")) icsp_set_timing(ICSP_SLOW_PHASE_NS, ICSP_SLOW_PHASE_NS);
    else if (arg && !strcmp(arg, "normal")) icsp_set_timing(ICSP_NORMAL_PHASE_NS, ICSP_NORMAL_PHASE_NS);
    else if (arg && !strcmp(arg, "fast")) icsp_set_timing(ICSP_FAST_PHASE_NS, ICSP_FAST_PHASE_NS);
    else printf("usage: speed slow|normal|fast\n");
    icsp_timing_t t = icsp_get_timing();
    printf("PGC low/high %lu/%lu ns\n", (unsigned long)t.low_ns, (unsigned long)t.high_ns);
}

/* Bring-up only: set (all three given) and show the ICSP entry delays, in us. */
static void cmd_entry(const char *p19_us, const char *p7_us, const char *pause_us) {
    if (p19_us && p7_us && pause_us) {
        icsp_set_entry_delays((icsp_entry_delays_t){.p19_us = (uint32_t)strtoul(p19_us, NULL, 0),
                                                    .p7_us = (uint32_t)strtoul(p7_us, NULL, 0),
                                                    .first_pause_us = (uint32_t)strtoul(pause_us, NULL, 0)});
    }
    icsp_entry_delays_t delays = icsp_get_entry_delays();
    printf("entry: P19 %lu us, P7 %lu us, pause after first SIX %lu us\n", (unsigned long)delays.p19_us,
           (unsigned long)delays.p7_us, (unsigned long)delays.first_pause_us);
}

/* Bring-up only: timing of the delay and GPIO primitives. The measured loops stay
 * written out: a call through a pointer would add its own cycles. The PGC pad's
 * output is kept off while its pair is measured: the same SIO writes, so the same
 * cycles, but nothing is driven (the Bus Pirate may have no power). PGC ends
 * released, which ends a jumper. */
static void cmd_bench(void) {
    uint32_t s0 = systick_hw->cvr;
    uint64_t u0 = time_us_64();
    busy_wait_us_32(10000);
    uint32_t s1 = systick_hw->cvr;
    uint64_t u1 = time_us_64();
    printf("SysTick: %lu ticks in %lu us\n", (unsigned long)((s0 - s1) & SYSTICK_MASK), (unsigned long)(u1 - u0));
    const uint32_t delays_ns[] = {0, 100, 200, 500, 1000, 2000};
    for (unsigned k = 0; k < sizeof delays_ns / sizeof delays_ns[0]; k++) {
        uint32_t irq = save_and_disable_interrupts();
        uint32_t a = systick_hw->cvr;
        for (int i = 0; i < 100; i++) hal_delay_ns(delays_ns[k]);
        uint32_t b = systick_hw->cvr;
        restore_interrupts(irq);
        printf("hal_delay_ns(%4lu): %lu cycles per call (irqs off)\n", (unsigned long)delays_ns[k],
               (unsigned long)(((a - b) & SYSTICK_MASK) / 100u));
    }
    gpio_set_oeover(PIN_PGC, GPIO_OVERRIDE_LOW);
    uint32_t irq = save_and_disable_interrupts();
    uint32_t a = systick_hw->cvr;
    for (int i = 0; i < 100; i++) {
        hal_pgc_drive(true);
        hal_pgc_drive(false);
    }
    uint32_t b = systick_hw->cvr;
    restore_interrupts(irq);
    printf("hal_pgc_drive pair: %lu cycles\n", (unsigned long)(((a - b) & SYSTICK_MASK) / 100u));
    pic_pgc_release();
    gpio_set_oeover(PIN_PGC, GPIO_OVERRIDE_NORMAL);
}

/* Bring-up only: the header pins pulled up from outside, and the three ICSP lines (not
 * the one the Pico holds low: PGC for the jumper, MCLR after a failed write). */
static void cmd_pins(void) {
    uint32_t up = pulled_up_pins();
    printf("Pulled up from outside:%s", up ? "" : " none");
    for (uint pin = 0; pin < HEADER_GPIOS; pin++) {
        if (up & (1u << pin)) printf(" GP%u (pin %u)", pin, header_pin[pin]);
    }
    if (pic_jumper_held()) {
        printf("\nReleased lines: PGD=%d MCLR=%d (PGC held low by 'jumper')\n", hal_pgd_read(), hal_mclr_read());
    } else if (pic_reset_held()) {
        printf("\nReleased lines: PGC=%d PGD=%d (MCLR held low after a failed write)\n", hal_pgc_read(),
               hal_pgd_read());
    } else {
        printf("\nReleased lines: PGC=%d PGD=%d MCLR=%d\n", hal_pgc_read(), hal_pgd_read(), hal_mclr_read());
    }
}

/* Bring-up only: which pull PGD has while released. */
static void cmd_pgdpull(const char *arg) {
    if (arg && !strcmp(arg, "up")) gpio_pull_up(PIN_PGD);
    else if (arg && !strcmp(arg, "none")) gpio_disable_pulls(PIN_PGD);
    else gpio_pull_down(PIN_PGD);
    printf("PGD pull: %s\n", gpio_is_pulled_up(PIN_PGD) ? "up" : gpio_is_pulled_down(PIN_PGD) ? "down" : "none");
}

/* Bring-up only: ICSP entry and the VISI echo, then the PIC runs again. */
static void cmd_ping(void) {
    uint16_t echoed[2] = {0, 0};
    pic_status_t st = pic_ping(echoed);
    printf("ping: %s, VISI echo %04X %04X (expected A55A 5AA5)\n", pic_status_str(st), echoed[0], echoed[1]);
    if (st != PIC_ERR_NO_TARGET && st != PIC_ERR_LINES) {
        pic_session_end();
        pic_release_target();
    }
}

/* Bring-up only: single ICSP steps. 'enter' and 'key' check the power first; 'six',
 * 'echo' and 'regout' clock PGC only while MCLR reads high (driven by 'raw enter', or
 * pulled up by a powered, released board). */
static void cmd_raw(const char *step, const char *hex) {
    bool enters = step && (!strcmp(step, "enter") || !strcmp(step, "key"));
    bool clocks_pgc = step && ((!strcmp(step, "six") && hex) || !strcmp(step, "echo") || !strcmp(step, "regout"));
    if (enters && !pic_target_present()) {
        printf("ERROR: %s\n", pic_status_str(PIC_ERR_NO_TARGET));
    } else if (clocks_pgc && !hal_mclr_read()) {
        printf("ERROR: MCLR is low: run 'raw enter' first (or the Bus Pirate has no power)\n");
    } else if (step && !strcmp(step, "enter")) {
        printf("enter: %s\n", icsp_enter() ? "key read back OK" : "line mismatch");
    } else if (step && !strcmp(step, "key")) {
        printf("key: %s\n", icsp_enter_key_only() ? "key read back OK, 5 extra clocks sent" : "line mismatch");
    } else if (step && !strcmp(step, "six") && hex) {
        uint32_t instr = (uint32_t)strtoul(hex, NULL, 16);
        icsp_six(instr);
        printf("six %06lX\n", (unsigned long)instr);
    } else if (step && !strcmp(step, "echo")) {
        /* back-to-back: NOP GOTO NOP, MOV #A55A W0, MOV W0 VISI, NOP, REGOUT */
        const uint32_t instrs[] = {OP_NOP, OP_GOTO_200, OP_NOP, 0x2A55A0, 0x883C20, OP_NOP};
        for (unsigned i = 0; i < sizeof instrs / sizeof instrs[0]; i++) icsp_six(instrs[i]);
        uint16_t v = icsp_regout();
        icsp_six(OP_NOP);
        printf("echo %04X\n", v);
    } else if (step && !strcmp(step, "regout")) {
        printf("regout %04X\n", icsp_regout());
    } else if (step && !strcmp(step, "exit")) {
        pic_session_end();
        printf("exit\n");
    } else if (step && !strcmp(step, "release")) {
        pic_release_target();
        printf("released\n");
    } else {
        printf("usage: raw enter|key|six HEX|regout|echo|exit|release\n");
    }
}

static void run_command(char *line) {
    char *cmd = strtok(line, " \t");
    if (!cmd) return;
    char *arg = strtok(NULL, " \t");
    char *arg2 = strtok(NULL, " \t");
    char *arg3 = strtok(NULL, " \t");
    if (!strcmp(cmd, "help") || !strcmp(cmd, "h") || !strcmp(cmd, "?")) {
        cmd_help();
    } else if (!strcmp(cmd, "status") || !strcmp(cmd, "s")) {
        cmd_status();
    } else if (!strcmp(cmd, "log")) {
        oplog_replay();
    } else if (!strcmp(cmd, "flash") || !strcmp(cmd, "f") || !strcmp(cmd, "unbrick")) {
        run_image_op(arg, "flash", "flash loaded", do_flash);
    } else if (!strcmp(cmd, "verify") || !strcmp(cmd, "v")) {
        run_image_op(arg, "verify", "verify loaded", do_verify);
    } else if (!strcmp(cmd, "bootloader")) {
        cmd_bootloader(arg);
    } else if (!strcmp(cmd, "jumper")) {
        run_op("jumper", "console", ops_jumper);
    } else if (!strcmp(cmd, "load")) {
        cmd_load(arg);
    } else if (!strcmp(cmd, "id") || !strcmp(cmd, "i")) {
        run_op("id", "console", ops_identify);
    } else if (!strcmp(cmd, "blank") || !strcmp(cmd, "b")) {
        run_op("blank", "console", ops_blank_check);
    } else if (!strcmp(cmd, "erase")) {
        run_op("erase", "console", ops_erase);
    } else if (!strcmp(cmd, "e")) {
        printf("Type 'erase' in full: it leaves the Bus Pirate without firmware.\n");
    } else if (!strcmp(cmd, "dump") || !strcmp(cmd, "d") || !strcmp(cmd, "read")) {
        run_op("dump", "console", ops_dump);
    } else if (!strcmp(cmd, "test")) {
        if (embedded_image_ok) {
            op_args.seed = arg ? (uint32_t)strtoul(arg, NULL, 0) : get_rand_32();
            run_op("test", "console", do_test);
        } else {
            printf("ERROR: %s\n", pic_status_str(PIC_ERR_IMAGE));
        }
    } else if (!strcmp(cmd, "release") || !strcmp(cmd, "r") || !strcmp(cmd, "run")) {
        run_op("release", "console", ops_release);
    } else if (!strcmp(cmd, "speed")) {
        cmd_speed(arg);
    } else if (!strcmp(cmd, "entry")) {
        cmd_entry(arg, arg2, arg3);
    } else if (!strcmp(cmd, "bench")) {
        cmd_bench();
    } else if (!strcmp(cmd, "pins")) {
        cmd_pins();
    } else if (!strcmp(cmd, "pgdpull")) {
        cmd_pgdpull(arg);
    } else if (!strcmp(cmd, "ping")) {
        cmd_ping();
    } else if (!strcmp(cmd, "raw")) {
        cmd_raw(arg, arg2);
    } else if (!strcmp(cmd, "bootsel")) {
        printf("Rebooting into the USB bootloader...\n");
        stdio_flush();
        sleep_ms(100);
        reset_usb_boot(0, 0);
    } else {
        printf("Unknown command '%s' (type 'help')\n", cmd);
    }
}

/* ------------------------------------------------------------------ main */

/* The command line being typed, kept between poll_line() calls. */
static char console_line[80];
static size_t console_len;

/* "> " with an empty command line: what was partly typed before is dropped, so the
 * next command is read alone (after a reconnect or a BOOTSEL run too). */
static void new_prompt(void) {
    console_len = 0;
    printf("> ");
}

/* Non-blocking: adds what has arrived to buf, echoed, with backspace editing.
 * True once Enter ends a non-empty line; *len keeps a partial line between calls. */
static bool poll_line(char *buf, size_t size, size_t *len) {
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r' || c == '\n') {
            if (*len) {
                buf[*len] = 0;
                *len = 0;
                printf("\n");
                return true;
            }
        } else if ((c == '\b' || c == 0x7F) && *len) {
            (*len)--;
            printf("\b \b");
        } else if (c >= ' ' && c < 0x7F && *len < size - 1) {
            buf[(*len)++] = (char)c;
            putchar(c);
        }
    }
    return false;
}

/* BOOTSEL held for BOOTSEL_HOLD_MS flashes the built-in image, once per press;
 * ignored while that image is corrupt. True while the button is held and that
 * flash has not run yet: keep holding. */
static bool poll_bootsel(void) {
    static uint32_t pressed_since_ms;
    static bool armed = true;
    uint32_t now_ms = hal_now_ms();
    if (!(embedded_image_ok && bootsel_pressed())) {
        pressed_since_ms = 0;
        armed = true;
        return false;
    }
    if (!pressed_since_ms) pressed_since_ms = now_ms;
    if (armed && now_ms - pressed_since_ms >= BOOTSEL_HOLD_MS) {
        armed = false; /* once per press */
        printf("\nBOOTSEL button: flash\n");
        op_args.image = &embedded_image; /* always the built-in image, never one from the computer */
        run_op("flash", "BOOTSEL button", do_flash);
        new_prompt();
    }
    return armed;
}

int main(void) {
    hal_init(); /* release the ICSP lines before anything else */
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    stdio_init_all();
    oplog_init();
    ops_set_busy_hook(busy_hook);
    embedded_image_ok = image_ok(&embedded_image);
    ds30_image_ok = image_ok(&ds30_image);
    mit_image_ok = image_ok(&mit_image);
    if (!embedded_image_ok) led_result(PIC_ERR_IMAGE);

    bool was_connected = false;
    for (;;) {
        bool connected = stdio_usb_connected();
        if (connected && !was_connected) {
            sleep_ms(200);
            print_banner();
            new_prompt();
        }
        was_connected = connected;

        if (poll_line(console_line, sizeof console_line, &console_len)) {
            run_command(console_line);
            new_prompt();
        }

        led_update(poll_bootsel()); /* lit while BOOTSEL is held: keep holding */
        sleep_ms(10);
    }
}

#ifdef BP3_HOST_TEST /* the image section below defines the real images */
#undef embedded_image
#undef ds30_image
#undef mit_image
#endif

/* ====================================================== 11. embedded image */
/* BEGIN GENERATED IMAGE */
/* Made from BPCOMPBZ-pickit-firmware-v7.1.12-bootloader-v4.5.hex
 * (sha256 9d0768740bbc11303be2ba573db849978b50ff94447cee0484848b2bd3ea4bb4) and bp3mitloader.hex
 * (sha256 787dfeaf0ff8d5500ed3f1fda6ddb20f1d20f984932c544c1fa25e210bc1ff71). Do not edit by hand. */
static const uint8_t image_data[IMAGE_BYTES] = {
0,168,4,0,0,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,96,4,0,130,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,96,4,0,130,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,162,4,0,
162,4,0,162,4,0,162,4,0,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
47,232,33,174,126,34,14,1,136,0,0,0,0,0,32,0,0,224,2,0,50,0,1,32,
32,2,136,20,0,7,72,1,7,0,0,224,3,0,58,96,246,39,1,0,32,2,0,55,
0,0,32,1,0,32,129,15,112,1,0,50,20,0,7,0,0,32,0,0,224,2,0,50,
0,0,2,0,0,0,30,139,2,0,0,0,0,64,218,0,0,254,68,64,169,128,59,32,
0,0,224,3,0,50,0,0,32,160,1,136,68,64,168,0,0,6,138,31,120,139,31,120,
0,0,6,145,1,136,128,4,120,0,0,235,26,0,55,226,132,68,50,160,180,153,5,186,
226,132,68,50,160,180,153,6,186,226,132,68,50,160,180,0,6,235,71,111,222,253,7,178,
99,108,225,3,0,58,235,255,7,0,0,235,10,0,55,96,108,225,4,0,58,0,93,235,
139,5,233,253,255,62,4,0,55,97,104,225,1,0,50,0,134,235,11,0,7,25,199,186,
25,5,186,14,4,224,3,0,58,10,0,224,224,255,58,0,0,6,254,7,178,158,1,136,
138,4,120,245,255,55,57,93,186,139,5,233,12,0,50,41,93,186,139,5,233,8,0,50,
12,0,224,3,0,58,226,132,68,50,160,180,245,255,55,25,221,186,139,5,233,250,255,58,
137,4,232,225,132,68,50,160,180,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
128,159,190,130,31,120,50,17,128,0,242,128,225,241,128,130,112,120,0,242,128,0,0,232,
0,242,136,241,241,128,129,15,80,1,0,58,149,96,169,133,96,169,79,1,120,79,0,190,
0,64,6,128,159,190,32,242,128,0,0,232,32,242,136,49,242,128,129,15,80,2,0,58,
149,128,169,4,0,55,17,242,128,97,64,120,0,128,251,32,17,136,133,128,169,79,0,190,
0,64,6,0,64,218,0,0,254,0,0,5,20,0,32,185,4,55,244,255,47,183,4,55,
0,3,190,148,0,7,96,17,184,4,112,167,7,240,161,7,248,163,23,0,58,101,9,177,
13,0,50,13,0,53,134,2,32,6,40,225,3,0,53,3,0,40,96,0,184,2,0,5,
0,0,64,129,128,72,2,1,73,131,129,73,133,2,233,250,255,58,0,0,6,248,143,66,
5,0,52,129,0,209,0,128,211,133,2,232,252,255,58,0,0,6,96,17,184,128,0,235,
0,0,5,0,1,190,115,0,7,4,112,167,3,240,161,3,248,163,19,0,58,101,9,177,
16,0,50,9,0,53,105,40,225,2,0,53,1,0,40,0,0,5,0,0,64,129,128,72,
133,2,233,252,255,58,0,0,6,248,143,66,5,0,53,129,0,209,0,128,211,133,2,232,
252,255,58,0,0,6,128,0,235,0,0,5,79,137,222,130,1,120,147,4,55,136,159,190,
138,159,190,0,130,112,4,2,113,4,130,113,38,0,50,0,4,190,2,0,190,96,17,184,
107,9,32,149,2,32,1,130,207,10,0,57,5,65,177,0,130,207,7,0,57,5,65,177,
9,130,207,4,0,57,5,65,177,8,130,207,1,0,57,5,65,177,5,66,82,18,0,50,
10,0,59,130,129,113,97,1,100,129,0,209,0,128,211,137,132,211,8,132,211,139,5,232,
4,66,232,247,255,58,7,0,55,8,4,68,137,132,76,0,0,72,129,128,72,139,5,233,
4,66,233,249,255,58,5,0,7,79,5,190,79,4,190,0,0,6,96,17,184,207,255,55,
129,0,32,97,128,85,11,0,61,11,0,32,97,254,47,1,0,225,29,0,52,130,129,113,
97,1,100,137,4,209,8,132,211,0,0,232,250,255,58,113,0,32,97,0,100,3,0,112,
2,0,96,9,0,50,97,4,68,224,132,76,9,136,165,5,0,50,139,5,232,104,8,225,
2,0,58,137,4,209,8,132,211,242,15,32,2,88,225,5,0,61,199,93,221,249,7,178,
137,128,117,8,0,120,0,0,6,11,248,39,139,0,120,0,0,5,199,10,222,241,7,178,
245,15,178,8,0,50,225,207,66,2,0,50,1,112,160,36,0,5,129,15,112,1,0,50,
4,8,5,68,0,5,129,15,112,2,0,58,20,0,5,133,2,233,0,0,64,129,192,72,
252,255,59,1,112,160,36,0,5,96,34,184,6,2,32,0,0,208,129,128,210,4,130,210,
133,130,210,0,0,160,2,2,82,131,130,90,3,0,49,2,2,66,131,130,74,0,0,161,
6,3,233,243,255,58,0,0,6,239,255,7,4,0,190,0,0,6,7,248,39,7,131,97,
6,56,225,4,0,58,247,7,32,135,131,97,135,3,113,1,0,58,3,240,162,136,159,190,
138,159,190,140,159,190,71,10,222,4,128,161,199,26,222,5,128,161,7,0,40,7,133,96,
135,133,97,247,15,32,7,32,225,158,0,50,7,40,225,156,0,50,1,6,120,4,0,224,
180,0,50,247,7,32,135,128,96,1,112,160,5,0,224,192,0,50,247,7,32,135,129,97,
3,112,160,5,6,82,15,0,58,11,80,225,11,0,50,3,8,225,2,0,58,2,0,225,
126,0,50,2,4,80,131,132,88,4,0,49,0,1,253,129,1,253,132,2,253,138,5,253,
96,68,184,33,0,55,5,32,225,5,0,61,0,1,253,129,1,253,132,2,253,138,5,253,
5,6,82,122,96,225,5,0,53,1,112,161,71,34,221,132,128,112,138,128,112,99,0,55,
96,68,184,239,2,102,7,255,47,7,6,102,5,0,224,6,0,52,240,131,18,135,20,221,
5,17,222,7,27,221,6,1,113,133,25,222,112,96,225,5,0,53,9,4,120,130,4,120,
3,1,120,128,1,235,112,6,86,11,80,225,3,0,58,2,0,64,131,128,72,4,0,55,
96,4,20,224,132,28,2,0,88,131,128,88,1,136,163,6,0,50,129,0,209,0,128,211,
137,132,211,8,132,211,4,2,232,39,0,55,1,120,163,37,0,58,1,0,224,3,0,50,
129,134,207,105,134,86,10,0,55,140,0,32,0,0,224,4,0,58,9,0,120,136,4,120,
0,4,235,112,2,82,128,134,207,13,6,70,12,6,233,112,96,225,6,0,53,128,0,120,
9,0,120,136,4,120,0,4,235,112,6,86,112,2,82,12,0,224,12,0,52,140,8,221,
240,3,22,7,3,222,134,128,112,12,0,221,7,75,222,6,0,112,140,76,221,7,67,222,
134,132,116,12,68,221,12,2,82,97,32,225,32,0,53,97,1,96,7,0,40,135,129,100,
247,255,39,135,131,100,7,3,116,3,0,224,6,0,50,130,3,115,4,0,50,0,0,232,
224,128,72,1,136,163,14,0,58,247,15,32,7,32,225,74,0,61,1,112,161,71,34,221,
129,0,114,138,128,112,0,0,190,79,6,190,79,5,190,79,4,190,0,0,6,96,0,184,
250,255,55,129,0,209,0,128,211,4,2,232,238,255,55,4,1,120,137,1,116,161,1,7,
138,128,112,241,255,55,247,1,7,7,2,120,0,1,253,129,1,253,243,1,7,135,2,120,
133,3,114,7,120,163,4,0,50,0,1,253,129,1,253,6,2,7,228,255,55,133,3,98,
7,104,163,8,0,50,11,80,225,3,0,50,240,255,47,241,255,39,220,255,55,0,1,253,
129,1,253,217,255,55,4,104,163,215,255,50,250,255,55,248,255,39,136,131,96,11,0,58,
0,0,224,9,0,58,131,3,100,130,131,115,2,0,50,2,0,190,204,255,55,11,80,225,
206,255,58,2,0,190,200,255,55,20,0,32,129,0,100,62,255,55,248,255,39,136,131,97,
4,0,58,2,0,224,2,0,58,140,0,120,190,255,55,21,0,32,131,1,100,57,255,55,
1,248,39,0,0,32,129,0,117,183,255,55,136,159,190,138,159,190,140,159,190,27,0,32,
2,2,190,5,134,104,6,0,40,6,6,102,71,9,222,242,15,178,130,0,50,2,67,232,
128,0,50,199,41,222,243,15,178,125,0,50,3,67,232,123,0,50,241,7,178,1,112,160,
245,7,178,5,112,160,199,8,221,73,3,222,134,128,112,71,0,221,199,42,221,73,35,222,
134,130,114,71,34,221,3,5,81,4,3,80,133,131,88,3,0,49,0,0,64,129,128,72,
10,5,233,140,31,120,138,159,190,4,0,80,133,128,88,4,2,66,133,130,74,0,0,64,
129,128,72,0,5,235,0,4,190,96,51,184,4,6,190,13,72,225,86,0,49,17,0,9,
77,204,216,12,1,184,13,2,184,132,129,65,96,130,74,130,131,83,3,4,92,132,132,92,
96,5,93,10,0,224,37,0,58,128,5,120,13,64,225,73,0,49,136,0,120,7,0,120,
17,0,9,77,136,216,12,1,184,13,2,184,132,129,65,96,130,74,96,3,17,131,131,91,
4,4,92,224,132,92,9,0,224,33,0,58,79,1,190,3,0,224,43,0,50,227,1,96,
7,6,115,140,129,113,1,0,50,100,0,112,66,0,222,78,94,221,12,0,112,194,88,222,
1,224,160,131,1,7,207,1,120,129,128,113,79,6,190,79,5,190,79,4,190,0,0,6,
140,131,67,13,4,76,224,132,76,96,5,77,0,0,233,10,0,224,212,255,50,140,131,67,
13,4,76,224,132,76,96,5,77,0,0,233,206,255,55,12,3,67,141,131,75,96,4,76,
224,132,76,0,0,233,9,0,224,216,255,50,12,3,67,141,131,75,96,4,76,224,132,76,
0,0,233,210,255,55,66,0,222,78,94,221,12,0,112,194,88,222,1,224,160,207,1,120,
3,0,224,218,255,50,96,0,16,224,128,24,215,255,55,240,255,47,170,255,55,240,255,47,
185,255,55,4,4,190,47,1,7,135,6,120,0,4,253,129,4,253,43,1,7,7,131,102,
6,120,163,3,0,50,9,96,167,8,0,190,44,0,55,13,120,163,2,0,50,8,0,190,
40,0,55,7,120,163,38,0,58,1,240,161,9,240,161,6,104,163,3,0,50,241,255,39,
240,255,47,186,255,55,13,104,163,3,0,50,8,0,190,140,128,112,181,255,55,7,104,163,
7,0,58,6,72,163,3,0,50,241,255,39,240,255,47,174,255,55,13,72,163,3,0,50,
96,0,184,140,128,112,169,255,55,7,72,163,4,0,50,1,248,39,0,0,32,140,128,112,
163,255,55,118,1,7,0,4,253,129,4,253,2,5,120,114,1,7,8,2,190,138,1,120,
89,255,55,1,96,160,18,2,32,2,64,136,151,255,55,1,0,224,18,0,50,5,0,224,
16,0,50,136,159,190,138,159,190,140,159,190,0,6,235,1,248,163,3,0,50,96,0,16,
224,128,24,12,240,160,5,248,163,3,0,50,96,2,18,224,130,26,12,240,162,128,5,235,
65,255,55,96,0,184,2,0,5,3,131,104,7,0,40,7,3,99,71,10,222,4,128,161,
199,26,222,5,128,161,247,15,32,7,32,225,66,0,50,7,40,225,64,0,50,136,159,190,
138,159,190,140,31,120,241,7,178,243,7,178,4,0,224,100,0,50,1,112,160,5,0,224,
104,0,50,3,112,160,133,5,66,235,7,177,2,4,184,3,2,184,132,132,68,96,133,74,
2,10,184,132,132,68,10,133,74,3,10,184,4,5,69,4,0,51,8,4,208,137,132,210,
10,133,210,139,5,233,97,88,225,61,0,53,2,16,32,9,1,97,3,8,32,137,129,97,
247,7,32,135,131,100,8,134,115,200,76,222,200,83,221,135,132,116,72,85,222,3,0,224,
6,0,50,130,3,118,4,0,50,137,4,232,96,5,77,10,136,163,12,0,58,247,15,32,
7,88,225,68,0,61,10,112,161,199,93,221,10,133,117,138,0,115,9,0,120,79,6,120,
79,5,190,79,4,190,0,0,6,10,5,209,137,132,211,139,5,232,240,255,55,136,31,120,
6,4,120,144,0,7,7,2,120,0,1,253,129,1,253,140,0,7,135,2,120,133,3,114,
7,120,163,5,0,50,0,1,253,129,1,253,159,0,7,79,4,120,0,0,6,133,3,114,
7,72,163,3,0,50,240,255,47,241,255,39,248,255,55,0,0,32,1,248,39,136,128,112,
244,255,55,136,1,120,72,72,221,128,129,113,72,72,222,200,80,221,1,0,112,200,80,222,
6,4,120,11,1,120,22,0,7,136,128,112,210,255,55,5,0,224,5,0,50,23,0,32,
58,0,7,157,255,55,129,3,112,249,255,58,134,0,120,79,6,120,79,5,190,79,4,190,
0,0,5,131,3,113,249,255,50,39,0,32,46,0,7,148,255,55,1,248,39,134,128,112,
244,255,55,1,0,224,38,0,50,97,1,17,120,16,225,35,0,60,96,34,184,112,16,225,
4,0,53,128,2,120,1,0,120,128,0,235,112,1,81,2,0,224,9,0,52,112,3,17,
6,42,221,130,42,222,134,3,221,135,130,114,2,0,222,134,11,221,7,0,112,130,8,222,
132,129,113,244,255,39,4,130,98,132,129,113,4,0,40,132,130,98,97,2,96,5,240,166,
0,0,6,4,130,113,7,0,50,0,0,232,224,128,72,0,0,6,0,2,190,96,0,184,
239,255,55,96,0,184,0,0,6,136,159,190,0,4,235,97,56,225,22,0,58,1,0,224,
8,0,58,200,0,222,72,0,221,136,0,32,1,0,224,3,0,58,200,0,222,72,0,221,
104,4,68,129,132,207,233,132,84,6,0,52,137,8,221,240,131,20,135,3,222,135,128,112,
9,0,221,9,4,68,97,2,20,79,4,190,0,0,6,3,0,224,8,0,58,200,17,222,
72,17,221,136,0,32,3,0,224,3,0,58,200,17,222,72,17,221,104,4,68,131,132,207,
233,132,84,6,0,52,137,25,221,240,131,20,135,19,222,135,129,113,9,17,221,9,4,68,
225,2,20,233,255,55,39,0,32,1,240,167,23,0,32,71,11,222,246,15,178,4,0,50,
6,67,232,10,0,50,71,2,179,0,0,6,1,3,120,246,7,178,6,3,112,2,0,58,
7,3,179,0,0,6,135,2,179,0,0,6,1,3,120,246,7,178,6,3,112,2,0,50,
7,112,160,0,0,6,7,96,160,0,0,6,4,120,163,4,0,50,5,120,163,3,0,50,
1,104,163,1,0,58,2,0,190,1,96,160,0,0,6,129,1,112,39,0,50,128,2,235,
1,248,163,3,0,50,96,0,16,224,128,24,21,0,32,244,7,32,2,2,66,32,0,52,
247,3,32,135,3,96,6,4,32,0,3,99,2,8,32,0,1,97,71,0,222,201,9,221,
0,128,113,199,8,222,6,0,224,8,0,50,135,3,113,6,0,50,16,0,176,1,128,176,
1,136,163,2,0,50,193,8,222,4,2,232,241,7,178,243,15,32,3,32,225,3,0,61,
71,34,221,129,0,114,41,0,55,1,248,39,0,0,235,38,0,55,96,0,184,0,0,6,
104,2,18,127,32,225,2,0,52,96,0,184,31,0,55,96,17,184,112,32,225,4,0,53,
128,1,120,1,0,120,128,0,235,112,2,82,4,0,224,9,0,52,240,3,18,7,25,221,
132,25,222,7,3,221,134,129,113,4,0,222,7,11,221,6,0,112,132,8,222,247,255,39,
131,131,99,135,3,113,6,0,40,3,3,99,7,0,50,7,0,224,3,0,58,97,1,96,
2,129,115,2,0,50,16,0,176,1,128,176,5,0,166,0,0,6,1,240,160,0,0,6,
79,10,222,71,9,222,241,7,178,242,15,178,11,0,50,242,7,177,1,112,160,199,8,221,
201,1,222,131,128,112,71,0,221,4,0,166,0,0,6,96,0,16,224,128,24,0,0,6,
129,15,112,1,0,58,0,0,6,129,130,207,5,0,224,16,0,62,128,130,207,238,130,66,
112,40,225,4,0,57,7,0,62,128,0,120,0,0,235,14,0,55,112,131,18,134,0,222,
5,0,221,10,0,55,112,131,82,134,0,221,0,0,235,6,0,55,226,130,82,133,8,221,
112,131,18,6,3,222,134,128,112,5,0,221,117,7,176,5,1,234,4,0,166,0,0,6,
96,0,16,224,128,24,0,0,6,136,159,190,138,159,190,48,4,66,176,132,74,48,5,75,
144,133,75,96,0,177,8,152,190,10,144,190,4,64,225,5,200,225,6,208,225,7,216,225,
2,0,49,104,0,64,16,8,232,79,5,190,79,4,190,0,0,6,136,159,190,138,159,190,
48,4,190,32,5,190,4,24,84,5,152,92,6,24,93,7,152,93,17,0,32,4,64,225,
5,200,225,6,208,225,7,216,225,1,0,57,128,0,235,16,136,16,79,5,190,79,4,190,
0,0,6,18,0,250,136,159,190,138,159,190,140,159,190,224,183,159,241,183,159,130,191,159,
147,191,159,6,5,190,4,4,190,0,0,235,208,183,159,192,183,159,176,183,159,160,183,159,
144,183,159,0,6,120,95,177,151,207,176,151,130,1,65,63,176,151,2,0,224,4,0,61,
55,0,7,126,128,87,192,255,7,3,0,55,51,0,7,126,128,87,207,255,7,223,182,151,
13,0,224,12,0,53,111,178,151,255,178,151,15,187,151,159,187,151,16,0,32,1,0,32,
96,17,184,0,2,114,129,130,114,2,3,115,131,131,115,27,0,7,12,6,232,1,4,32,
129,15,86,224,255,58,13,0,224,4,0,61,8,2,190,10,3,190,126,128,87,163,255,7,
95,168,151,0,0,224,5,0,58,31,178,151,175,178,151,63,179,151,207,179,151,9,0,7,
111,176,151,255,176,151,15,185,151,159,185,151,79,6,190,79,5,190,79,4,190,0,128,250,
0,0,6,1,0,7,0,0,6,164,183,159,181,183,159,198,183,159,215,183,159,0,0,6,
79,9,222,130,129,113,179,183,159,79,1,222,129,128,64,130,128,112,161,183,159,143,176,151,
79,9,222,0,0,64,2,0,112,144,183,159,127,168,151,79,1,222,129,128,64,130,128,112,
129,183,159,13,0,7,79,58,222,0,0,64,4,0,112,240,175,159,8,0,7,4,2,66,
133,130,74,6,3,75,135,131,75,223,255,7,8,2,190,10,3,190,0,0,6,47,178,151,
191,178,151,79,179,151,223,179,151,0,0,6,136,31,120,24,0,32,136,31,120,137,255,7,
143,135,233,79,4,120,0,0,6,128,31,235,132,255,7,143,135,233,0,0,6,136,159,190,
138,159,190,132,31,120,39,0,7,79,0,120,17,0,51,240,255,47,131,143,108,6,0,59,
4,5,101,10,0,167,8,0,55,9,240,166,16,0,32,8,0,55,130,15,84,131,143,92,
3,0,57,16,0,32,1,0,62,0,0,32,9,240,167,0,0,234,79,5,190,79,4,190,
0,0,224,0,0,6,136,31,120,3,4,120,3,0,224,7,0,61,96,0,16,224,128,24,
96,1,25,224,129,25,2,0,59,1,240,45,3,0,55,97,251,7,8,240,167,1,240,160,
79,4,120,0,0,6,0,4,190,178,251,7,0,3,190,4,5,190,2,0,190,174,251,7,
132,79,117,0,0,6,128,1,120,2,0,55,177,89,120,2,1,233,2,0,224,252,255,58,
0,0,6,136,159,190,138,159,190,128,4,120,2,0,224,91,0,50,1,64,120,129,76,120,
130,129,68,241,249,159,226,15,81,85,0,54,145,68,152,161,68,152,225,249,159,209,249,159,
230,15,81,79,0,54,177,68,152,193,249,159,232,15,81,75,0,54,9,5,234,99,5,101,
10,132,68,10,5,81,193,255,47,1,5,101,0,128,251,128,0,235,18,16,32,19,16,32,
68,0,7,0,140,190,10,1,68,96,185,159,113,185,159,232,15,85,58,0,54,32,4,152,
49,4,152,64,4,152,81,4,152,32,185,159,49,185,159,64,185,159,81,185,159,248,15,85,
48,0,54,96,4,152,113,4,152,0,12,152,17,12,152,32,12,152,49,12,152,64,12,152,
81,12,152,32,177,159,49,177,159,64,177,159,81,177,159,96,177,159,113,177,159,0,185,159,
17,185,159,100,1,100,120,1,65,2,4,68,2,5,85,96,17,184,2,3,190,0,2,190,
4,3,190,96,34,184,0,0,114,129,128,114,2,1,115,131,129,115,16,0,55,0,156,190,
2,148,190,64,4,152,81,4,152,98,4,152,115,4,152,0,12,152,17,12,152,34,12,152,
51,12,152,64,12,152,81,12,152,98,12,152,115,12,152,10,2,177,8,2,176,255,15,85,
238,255,62,9,0,120,79,5,190,79,4,190,0,0,6,2,10,185,0,27,185,2,0,184,
129,0,66,129,0,67,0,0,6,0,82,0,69,84,0,85,82,0,78,0,0,82,85,0,
78,0,0,76,73,0,83,84,0,0,69,0,88,73,0,84,0,0,78,69,0,87,0,0,
76,69,0,84,0,0,73,70,0,0,84,0,72,69,0,78,0,0,69,76,0,83,69,0,
0,71,0,79,84,0,79,0,0,71,79,0,83,85,0,66,0,0,82,69,0,84,85,0,
82,78,0,0,82,0,69,77,0,0,80,0,82,73,0,78,84,0,0,73,0,78,80,0,
85,84,0,0,70,0,79,82,0,0,84,0,79,0,0,78,69,0,88,84,0,0,82,0,
69,65,0,68,0,0,68,65,0,84,65,0,0,83,0,84,65,0,82,84,0,82,0,0,
83,84,0,65,82,0,84,0,0,83,84,0,79,80,0,82,0,0,83,84,0,79,80,0,
0,83,0,69,78,0,68,0,0,82,69,0,67,69,0,73,86,0,69,0,0,67,76,0,
75,0,0,68,65,0,84,0,0,66,73,0,84,82,0,69,65,0,68,0,0,65,68,0,
67,0,0,65,85,0,88,80,0,73,78,0,0,80,0,83,85,0,0,80,0,85,76,0,
76,85,0,80,0,0,68,69,0,76,65,0,89,0,0,65,85,0,88,0,0,70,82,0,
69,81,0,0,68,0,85,84,0,89,0,0,77,65,0,67,82,0,79,0,0,69,78,0,
68,0,0,128,143,0,132,143,0,135,143,0,140,143,0,145,143,0,150,143,0,156,143,0,
163,143,0,167,143,0,173,143,0,179,143,0,183,143,0,186,143,0,191,143,0,196,143,0,
201,143,0,208,143,0,214,143,0,220,143,0,225,143,0,230,143,0,238,143,0,242,143,0,
246,143,0,254,143,0,2,144,0,9,144,0,13,144,0,20,144,0,26,144,0,30,144,0,
35,144,0,40,144,0,46,144,0,0,0,0,0,46,0,0,58,0,32,0,0,0,94,0,
188,226,0,97,63,0,221,131,0,194,156,0,126,32,0,163,253,0,31,65,0,157,195,0,
33,127,0,252,162,0,64,30,0,95,1,0,227,189,0,62,96,0,130,220,0,35,125,0,
159,193,0,66,28,0,254,160,0,225,191,0,93,3,0,128,222,0,60,98,0,190,224,0,
2,92,0,223,129,0,99,61,0,124,34,0,192,158,0,29,67,0,161,255,0,70,24,0,
250,164,0,39,121,0,155,197,0,132,218,0,56,102,0,229,187,0,89,7,0,219,133,0,
103,57,0,186,228,0,6,88,0,25,71,0,165,251,0,120,38,0,196,154,0,101,59,0,
217,135,0,4,90,0,184,230,0,167,249,0,27,69,0,198,152,0,122,36,0,248,166,0,
68,26,0,153,199,0,37,123,0,58,100,0,134,216,0,91,5,0,231,185,0,140,210,0,
48,110,0,237,179,0,81,15,0,78,16,0,242,172,0,47,113,0,147,205,0,17,79,0,
173,243,0,112,46,0,204,146,0,211,141,0,111,49,0,178,236,0,14,80,0,175,241,0,
19,77,0,206,144,0,114,44,0,109,51,0,209,143,0,12,82,0,176,238,0,50,108,0,
142,208,0,83,13,0,239,177,0,240,174,0,76,18,0,145,207,0,45,115,0,202,148,0,
118,40,0,171,245,0,23,73,0,8,86,0,180,234,0,105,55,0,213,139,0,87,9,0,
235,181,0,54,104,0,138,212,0,149,203,0,41,119,0,244,170,0,72,22,0,233,183,0,
85,11,0,136,214,0,52,106,0,43,117,0,151,201,0,74,20,0,246,168,0,116,42,0,
200,150,0,21,75,0,169,247,0,182,232,0,10,84,0,215,137,0,107,53,0,73,9,0,
0,79,0,9,0,0,72,9,0,0,76,0,9,0,0,62,0,0,0,27,0,91,0,0,
68,0,0,67,0,0,13,10,0,40,0,0,120,0,0,41,62,0,0,46,0,32,0,0,
66,117,0,115,32,0,80,105,0,114,97,0,116,101,0,32,118,0,51,0,0,67,111,0,
109,109,0,117,110,0,105,116,0,121,32,0,70,105,0,114,109,0,119,97,0,114,101,0,
32,118,0,55,46,0,49,32,0,45,32,0,98,117,0,122,122,0,112,105,0,114,97,0,
116,46,0,99,111,0,109,32,0,98,121,0,32,68,0,114,101,0,103,32,0,66,80,0,
67,79,0,77,80,0,32,84,0,87,69,0,76,86,0,69,32,0,0,41,0,0,46,0,
32,60,0,0,0,0,0,0,0,1,0,0,0,0,0,2,0,0,0,0,0,4,0,0,
0,0,0,8,0,0,0,0,0,16,0,0,0,0,0,32,0,0,0,0,0,64,0,0,
0,0,0,128,0,0,0,0,0,128,0,0,0,0,0,0,1,0,0,0,0,0,2,0,
0,0,0,0,4,0,0,0,0,0,8,0,0,0,0,0,16,0,0,0,0,0,32,0,
0,0,0,0,64,0,0,0,0,0,128,0,0,0,0,0,0,0,1,0,0,0,0,0,
2,0,0,0,0,0,4,0,0,0,0,0,8,0,0,0,0,0,16,0,0,0,0,0,
20,52,0,4,13,0,130,6,0,64,3,0,160,1,0,207,0,0,103,0,0,68,0,0,
34,0,0,127,0,0,0,0,0,0,0,0,44,1,0,0,0,0,88,2,0,0,0,0,
176,4,0,0,0,0,96,9,0,0,0,0,192,18,0,0,0,0,128,37,0,0,0,0,
64,56,0,0,0,0,0,75,0,0,0,0,128,112,0,0,0,0,0,150,0,0,0,0,
192,218,0,0,0,0,0,225,0,0,0,0,0,194,0,1,0,0,0,244,0,1,0,0,
0,232,0,3,0,0,48,48,0,48,44,0,0,48,0,48,0,0,8,8,0,8,8,0,
8,0,0,0,48,0,49,50,0,51,52,0,53,54,0,55,56,0,57,65,0,66,67,0,
68,69,0,70,0,0,49,65,0,76,83,0,1,66,0,80,118,0,51,0,0,33,0,0,
0,16,0,0,35,0,0,15,0,66,64,0,64,5,0,65,2,0,0,0,0,0,0,0,
0,105,0,208,93,0,109,219,0,93,107,0,203,93,0,105,208,0,93,0,0,0,0,0,
24,28,0,29,12,0,22,26,0,11,15,0,30,23,0,27,0,0,20,80,0,1,8,0,
1,3,0,0,0,0,0,32,0,41,0,0,157,37,0,13,0,0,0,53,0,63,98,0,
97,0,0,41,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,224,235,128,
0,0,232,0,128,251,161,199,33,225,64,120,128,199,179,128,207,80,16,0,50,64,198,179,
128,207,80,9,0,50,32,198,179,128,207,80,8,0,50,0,64,235,114,199,179,130,207,80,
8,0,58,48,0,32,5,0,55,16,0,32,3,0,55,32,0,32,1,0,55,0,0,235,
0,64,232,0,0,6,66,22,128,128,0,97,16,24,41,1,0,50,224,23,41,212,107,2,
0,0,0,0,0,6,82,22,128,128,0,97,112,24,41,1,0,50,64,24,41,212,107,2,
0,0,0,0,0,6,128,0,120,0,195,179,1,0,224,1,0,50,16,195,179,98,106,2,
0,0,0,0,0,6,240,235,128,209,235,128,129,15,80,4,0,58,112,192,179,98,106,2,
0,0,0,8,0,55,0,0,232,242,15,32,225,215,33,130,8,96,0,221,41,1,0,32,
204,136,2,0,0,0,0,0,6,64,220,41,1,0,32,204,136,2,0,0,0,16,229,33,
16,129,251,0,3,32,0,145,185,33,169,33,1,0,65,212,107,2,0,0,0,32,229,33,
16,0,120,100,0,96,4,0,50,128,202,41,1,0,32,204,136,2,0,0,0,160,24,41,
212,107,2,0,0,0,0,0,6,136,159,190,138,159,190,128,5,120,225,235,128,208,235,128,
128,143,80,12,0,50,164,199,33,128,65,235,242,15,32,3,114,120,0,0,233,2,0,96,
128,143,80,251,255,58,208,235,136,0,65,235,161,199,33,130,112,120,11,0,233,0,128,251,
210,235,128,0,4,235,130,15,80,18,0,50,161,199,33,225,64,120,1,4,224,7,0,58,
3,0,55,228,64,120,1,4,224,5,0,58,0,0,232,0,132,251,7,0,55,243,15,32,
164,199,33,0,0,233,3,0,96,130,15,80,244,255,58,0,4,235,188,255,7,139,15,84,
14,0,50,169,199,33,250,15,32,105,64,124,98,106,2,0,0,0,208,235,128,104,240,124,
0,0,232,10,0,96,208,235,136,8,4,232,10,4,100,136,143,85,244,255,58,208,235,128,
0,65,235,161,199,33,130,112,120,240,235,136,79,5,190,79,4,190,0,0,6,136,159,190,
138,159,190,232,235,128,8,4,233,8,132,251,208,235,128,128,15,84,23,0,50,0,5,235,
169,199,33,11,216,33,105,64,124,0,4,224,12,0,58,8,0,233,0,128,251,105,64,120,
0,4,224,7,0,50,10,5,232,27,0,120,128,15,85,3,0,54,128,13,232,8,0,120,
162,255,7,8,4,233,8,132,251,208,235,128,136,15,80,236,255,58,112,192,179,98,106,2,
0,0,0,79,5,190,79,4,190,0,0,6,136,159,190,138,159,190,233,235,128,137,4,233,
137,132,251,208,235,128,128,143,84,24,0,50,128,0,235,170,199,33,11,216,33,106,192,124,
0,4,224,13,0,58,9,0,233,0,128,251,106,64,120,0,4,224,8,0,50,129,0,232,
27,4,233,129,15,84,4,0,58,136,13,120,9,0,120,125,255,7,136,0,120,137,4,233,
137,132,251,208,235,128,137,15,80,235,255,58,0,236,128,225,15,80,4,0,50,112,192,179,
98,106,2,0,0,0,20,0,55,87,255,7,208,235,128,225,235,128,129,15,80,9,0,50,
164,199,33,128,65,235,242,15,32,3,114,120,0,0,233,2,0,96,128,143,80,251,255,58,
208,235,136,0,65,235,161,199,33,130,112,120,240,235,136,0,216,33,0,8,235,79,5,190,
79,4,190,0,0,6,17,192,179,48,216,33,1,72,120,128,0,235,1,236,136,208,235,128,
162,199,33,1,113,120,0,0,232,0,128,251,208,235,136,240,235,136,192,24,41,182,108,2,
0,0,0,0,0,6,16,229,33,16,64,120,0,4,224,5,0,58,160,98,41,1,0,32,
38,137,2,0,0,0,20,0,55,0,128,251,1,3,32,1,128,185,32,2,176,161,166,33,
97,0,120,0,0,1,192,145,41,1,0,32,38,137,2,0,0,0,192,2,169,194,2,169,
160,146,41,1,0,32,204,136,2,0,0,0,192,24,41,182,108,2,0,0,0,0,0,6,
16,229,33,16,4,224,5,0,58,160,98,41,1,0,32,38,137,2,0,0,0,52,0,55,
192,2,169,194,2,168,33,227,168,32,0,32,94,105,2,0,0,0,160,0,32,4,105,2,
0,0,0,1,27,32,129,15,80,26,0,54,144,0,32,4,105,2,0,0,0,113,43,32,
129,15,80,20,0,54,224,162,41,1,0,32,204,136,2,0,0,0,192,24,41,182,108,2,
0,0,0,16,229,33,144,128,251,0,3,32,0,136,185,0,2,176,161,166,33,97,0,120,
0,0,1,160,160,41,1,0,32,38,137,2,0,0,0,13,0,55,192,2,169,194,2,169,
0,96,41,1,0,32,38,137,2,0,0,0,160,146,41,1,0,32,204,136,2,0,0,0,
192,24,41,182,108,2,0,0,0,33,227,169,0,0,6,136,159,190,138,159,190,241,235,128,
208,235,128,128,4,235,128,143,80,15,0,50,170,199,33,11,194,179,1,4,232,106,64,124,
0,245,120,0,4,224,1,0,58,11,64,120,98,106,2,0,0,0,137,4,232,136,128,251,
208,235,128,129,15,80,243,255,58,0,0,233,0,128,251,208,235,136,208,24,41,212,107,2,
0,0,0,9,0,120,76,107,2,0,0,0,0,25,41,212,107,2,0,0,0,79,5,190,
79,4,190,0,0,6,240,235,128,225,235,128,129,15,80,4,0,58,112,192,179,98,106,2,
0,0,0,24,0,55,209,235,128,129,15,80,12,0,58,0,0,233,0,128,251,208,235,136,
0,65,235,161,199,33,130,112,120,240,235,136,64,221,41,1,0,32,204,136,2,0,0,0,
9,0,55,128,220,41,1,0,32,204,136,2,0,0,0,224,215,33,16,1,233,241,15,32,
1,8,97,187,255,7,0,0,6,241,235,128,224,235,128,128,143,80,4,0,58,112,192,179,
98,106,2,0,0,0,1,0,55,177,255,7,0,0,6,136,159,190,241,235,128,224,235,128,
128,143,80,3,0,58,112,192,179,98,106,2,0,0,0,233,215,33,232,235,128,25,4,20,
8,132,251,208,24,41,212,107,2,0,0,0,8,0,120,76,107,2,0,0,0,0,25,41,
212,107,2,0,0,0,224,235,128,128,12,120,79,4,190,0,0,6,136,159,190,241,235,128,
208,235,128,128,143,80,3,0,58,112,192,179,98,106,2,0,0,0,233,215,33,216,235,128,
25,4,84,8,132,251,208,24,41,212,107,2,0,0,0,8,0,120,76,107,2,0,0,0,
32,25,41,212,107,2,0,0,0,208,235,128,128,12,120,79,4,190,0,0,6,136,159,190,
138,159,190,140,159,190,142,31,120,128,70,120,218,235,128,138,0,232,129,128,251,224,235,128,
128,143,80,75,0,50,255,207,86,73,0,54,224,199,179,128,207,86,70,0,62,248,235,128,
136,15,85,13,0,58,13,64,120,98,106,2,0,0,0,208,235,128,161,199,33,141,112,120,
0,0,232,0,128,251,208,235,136,0,65,235,130,112,120,240,235,136,57,0,55,208,24,41,
212,107,2,0,0,0,8,0,85,251,15,32,11,0,96,76,107,2,0,0,0,32,25,41,
212,107,2,0,0,0,218,235,128,224,215,33,16,0,233,139,5,96,138,143,85,22,0,50,
171,199,33,200,220,41,9,0,32,252,15,32,238,215,33,10,128,69,235,64,125,17,64,152,
107,64,125,0,4,224,5,0,50,98,106,2,0,0,0,8,0,190,204,136,2,0,0,0,
10,5,233,12,5,101,30,0,233,12,0,96,138,15,80,239,255,58,13,64,120,98,106,2,
0,0,0,225,215,33,17,0,120,162,199,33,13,113,120,0,1,232,240,15,32,128,8,97,
209,235,128,129,0,232,0,128,96,208,235,136,3,0,55,112,192,179,98,106,2,0,0,0,
79,7,120,79,6,190,79,5,190,79,4,190,0,0,6,32,216,33,65,192,179,144,207,16,
5,0,54,134,133,2,0,0,0,32,216,33,0,72,235,16,0,55,240,235,128,225,235,128,
129,15,80,9,0,50,0,0,233,242,15,32,225,215,33,130,8,96,128,220,41,1,0,32,
204,136,2,0,0,0,3,0,55,112,192,179,98,106,2,0,0,0,0,0,6,90,106,2,
0,0,0,177,197,179,129,79,80,41,0,58,90,106,2,0,0,0,17,196,179,129,79,80,
19,0,50,7,0,62,17,195,179,129,79,80,19,0,50,65,195,179,129,79,80,29,0,58,
22,0,55,49,196,179,129,79,80,6,0,50,9,0,57,65,196,179,129,79,80,21,0,58,
204,255,7,19,0,55,175,253,7,17,0,55,26,254,7,15,0,55,61,254,7,13,0,55,
90,106,2,0,0,0,225,199,179,129,79,80,8,0,58,47,255,7,6,0,55,90,106,2,
0,0,0,225,199,179,129,79,80,1,0,58,65,255,7,0,0,6,136,159,190,160,193,41,
1,0,32,38,137,2,0,0,0,160,245,40,1,0,32,38,137,2,0,0,0,128,119,41,
1,0,32,204,136,2,0,0,0,16,229,33,144,128,251,0,3,32,0,136,185,64,2,176,
161,166,33,97,0,120,0,0,1,32,194,41,1,0,32,204,136,2,0,0,0,0,64,32,
105,253,7,0,16,32,103,253,7,0,32,32,101,253,7,0,4,32,99,253,7,0,8,32,
97,253,7,192,24,41,182,108,2,0,0,0,192,218,41,1,0,32,204,136,2,0,0,0,
33,227,168,160,0,32,4,105,2,0,0,0,98,107,2,0,0,0,104,227,41,9,0,32,
8,0,190,204,136,2,0,0,0,144,192,179,98,106,2,0,0,0,144,0,32,4,105,2,
0,0,0,98,107,2,0,0,0,8,0,190,204,136,2,0,0,0,144,192,179,98,106,2,
0,0,0,192,0,32,4,105,2,0,0,0,98,107,2,0,0,0,8,0,190,204,136,2,
0,0,0,144,192,179,98,106,2,0,0,0,176,0,32,4,105,2,0,0,0,98,107,2,
0,0,0,8,0,190,204,136,2,0,0,0,144,192,179,98,106,2,0,0,0,0,64,32,
49,253,7,0,16,32,47,253,7,0,32,32,45,253,7,0,4,32,43,253,7,0,8,32,
41,253,7,192,24,41,182,108,2,0,0,0,192,24,41,182,108,2,0,0,0,64,142,41,
1,0,32,38,137,2,0,0,0,192,177,41,1,0,32,204,136,2,0,0,0,192,24,41,
182,108,2,0,0,0,128,218,41,1,0,32,204,136,2,0,0,0,16,24,41,192,66,171,
1,0,50,224,23,41,212,107,2,0,0,0,192,24,41,182,108,2,0,0,0,80,0,32,
4,105,2,0,0,0,98,107,2,0,0,0,104,227,41,9,0,32,8,0,190,204,136,2,
0,0,0,144,192,179,98,106,2,0,0,0,64,0,32,4,105,2,0,0,0,98,107,2,
0,0,0,8,0,190,204,136,2,0,0,0,144,192,179,98,106,2,0,0,0,112,24,41,
194,66,171,1,0,50,64,24,41,212,107,2,0,0,0,192,24,41,182,108,2,0,0,0,
33,227,169,79,4,190,0,0,6,64,214,41,1,0,32,204,136,2,0,0,0,193,191,42,
0,0,235,60,105,2,0,0,0,120,106,2,0,0,0,32,205,41,1,0,32,204,136,2,
0,0,0,225,191,42,0,0,235,60,105,2,0,0,0,120,106,2,0,0,0,192,24,41,
182,108,2,0,0,0,64,175,41,1,0,32,38,137,2,0,0,0,63,255,7,194,2,171,
5,0,50,224,162,41,1,0,32,204,136,2,0,0,0,4,0,55,160,146,41,1,0,32,
204,136,2,0,0,0,192,194,179,98,106,2,0,0,0,0,194,179,98,106,2,0,0,0,
203,98,171,5,0,50,224,137,41,1,0,32,204,136,2,0,0,0,4,0,55,224,125,41,
1,0,32,204,136,2,0,0,0,192,194,179,98,106,2,0,0,0,0,194,179,98,106,2,
0,0,0,96,229,33,16,64,120,112,64,96,5,0,50,0,67,41,1,0,32,38,137,2,
0,0,0,4,0,55,192,87,41,1,0,32,38,137,2,0,0,0,96,229,33,16,64,120,
104,64,96,5,0,58,224,99,41,1,0,32,204,136,2,0,0,0,4,0,55,32,101,41,
1,0,32,204,136,2,0,0,0,192,194,179,98,106,2,0,0,0,0,194,179,98,106,2,
0,0,0,224,104,41,1,0,32,204,136,2,0,0,0,80,229,33,16,64,120,62,107,2,
0,0,0,192,24,41,182,108,2,0,0,0,96,229,33,16,64,120,99,64,96,225,79,80,
5,0,58,224,124,41,1,0,32,38,137,2,0,0,0,4,0,55,224,123,41,1,0,32,
38,137,2,0,0,0,16,229,33,144,128,251,0,3,32,0,136,185,96,2,176,161,166,33,
97,0,120,0,0,1,64,175,41,1,0,32,38,137,2,0,0,0,0,0,6,136,159,190,
138,31,120,227,235,128,3,128,251,161,199,33,97,64,120,241,204,179,129,64,64,232,207,80,
26,0,62,0,128,251,0,3,177,131,0,232,129,128,251,162,199,33,98,193,120,4,205,179,
4,66,65,17,0,32,233,79,82,118,0,62,248,15,32,167,199,33,6,205,179,106,2,185,
2,128,251,4,0,64,0,3,177,129,0,232,3,129,64,8,1,97,103,65,121,6,66,65,
233,79,82,245,255,54,103,0,55,1,195,179,129,79,80,96,0,58,3,0,232,0,128,251,
161,199,33,97,65,120,32,198,179,128,79,81,11,0,58,3,128,232,0,128,251,161,199,33,
97,65,120,4,205,179,4,66,65,0,0,235,33,0,32,225,79,82,5,0,54,82,0,55,
32,196,179,128,79,81,16,0,58,241,255,55,247,15,32,166,199,33,5,205,179,0,0,64,
2,129,251,2,0,64,0,3,177,129,0,232,3,129,64,7,1,97,102,65,121,5,66,65,
225,79,82,245,255,54,63,0,55,128,199,179,128,79,81,10,0,50,128,197,179,128,79,81,
30,0,58,9,0,55,68,0,221,0,3,177,2,129,251,2,0,64,129,0,232,10,0,55,
0,0,235,33,0,32,2,0,55,0,0,235,33,0,32,248,15,32,166,199,33,7,205,179,
249,203,179,250,201,179,3,130,64,8,2,98,102,65,122,135,66,65,233,207,82,234,255,54,
137,66,65,229,207,82,38,0,54,138,66,65,229,207,82,35,0,54,27,0,55,4,205,179,
4,66,65,0,0,235,17,0,32,233,79,82,21,0,62,2,128,251,0,3,177,248,15,32,
167,199,33,6,205,179,106,2,185,2,128,251,4,0,64,0,3,177,129,0,232,3,129,64,
8,1,97,103,65,121,6,66,65,233,79,82,245,255,54,4,0,55,96,229,33,16,116,160,
0,0,235,4,0,55,226,235,128,130,128,64,129,128,251,225,235,136,79,5,120,79,4,190,
0,0,6,2,84,160,2,115,122,68,0,221,112,5,177,2,129,251,2,0,64,190,255,55,
136,31,120,225,235,128,1,0,232,0,128,251,162,199,33,226,65,120,16,0,32,162,195,179,
130,207,81,9,0,58,129,128,232,248,15,32,136,128,96,225,235,136,88,255,7,225,235,128,
129,0,233,8,132,96,232,235,136,79,4,120,0,0,6,136,31,120,225,235,128,1,0,232,
0,128,251,162,199,33,226,65,120,0,0,235,178,195,179,130,207,81,9,0,58,129,128,232,
248,15,32,136,128,96,225,235,136,67,255,7,225,235,128,129,0,233,8,132,96,232,235,136,
79,4,120,0,0,6,224,235,128,161,199,33,97,65,120,1,194,179,129,79,81,9,0,58,
244,15,32,163,199,33,1,65,120,0,0,232,4,0,96,227,64,120,130,207,80,251,255,50,
224,235,136,0,0,6,136,159,190,138,31,120,0,68,120,224,235,128,0,0,232,0,128,251,
224,235,136,232,255,7,36,255,7,0,5,120,8,4,224,6,0,50,80,229,33,144,64,120,
10,0,120,18,105,2,0,0,0,0,5,120,10,64,120,166,106,2,0,0,0,136,219,41,
9,0,32,8,0,190,204,136,2,0,0,0,10,64,120,62,107,2,0,0,0,8,0,190,
204,136,2,0,0,0,10,64,120,160,107,2,0,0,0,192,24,41,182,108,2,0,0,0,
79,5,120,79,4,190,0,0,6,16,0,250,136,159,190,138,159,190,140,159,190,80,7,152,
97,7,152,114,7,152,67,7,152,72,221,41,9,0,32,106,102,41,11,0,32,64,25,41,
212,107,2,0,0,0,94,0,144,0,0,224,4,0,61,128,25,41,212,107,2,0,0,0,
3,0,55,46,72,144,62,107,2,0,0,0,160,25,41,212,107,2,0,0,0,128,0,235,
49,7,152,129,6,120,90,106,2,0,0,0,0,70,120,237,79,86,70,0,50,5,0,62,
232,79,86,10,0,50,234,79,86,44,0,58,64,0,55,208,194,179,128,79,86,24,0,50,
128,199,179,128,79,86,37,0,58,32,0,55,13,0,224,5,0,50,141,6,233,8,0,190,
204,136,2,0,0,0,231,255,55,62,0,144,0,0,224,6,0,50,8,0,190,204,136,2,
0,0,0,128,0,235,49,7,152,222,255,55,112,192,179,98,106,2,0,0,0,218,255,55,
13,0,224,6,0,58,208,194,179,98,106,2,0,0,0,16,0,32,48,7,152,210,255,55,
112,192,179,98,106,2,0,0,0,206,255,55,206,0,144,1,0,224,18,0,50,53,0,55,
1,205,179,1,64,70,233,79,80,13,0,62,227,143,86,5,0,52,112,192,179,98,106,2,
0,0,0,77,0,32,191,255,55,12,64,120,98,106,2,0,0,0,12,247,126,141,6,232,
185,255,55,112,192,179,98,106,2,0,0,0,181,255,55,192,24,41,182,108,2,0,0,0,
13,0,224,31,0,50,13,0,233,0,1,235,8,0,51,14,0,120,141,6,71,106,17,185,
176,128,251,1,3,177,2,129,64,141,15,80,250,255,58,110,0,144,130,15,80,9,0,60,
254,0,144,130,143,80,6,0,53,62,0,144,0,0,224,10,0,50,2,1,234,82,7,152,
8,0,55,10,0,190,38,137,2,0,0,0,130,255,55,128,128,235,81,7,152,1,0,55,
82,7,152,94,0,144,79,6,190,79,5,190,79,4,190,0,128,250,0,0,6,14,0,250,
136,159,190,138,159,190,140,159,190,224,235,128,0,4,233,8,132,251,29,0,32,136,15,80,
40,0,50,169,199,33,251,15,32,105,64,124,0,4,224,30,0,58,8,6,232,11,6,102,
105,64,126,0,4,224,25,0,50,13,64,120,62,107,2,0,0,0,208,25,41,212,107,2,
0,0,0,105,64,126,0,4,224,8,0,50,8,133,232,98,106,2,0,0,0,11,0,101,
105,64,120,10,5,232,0,4,224,249,255,58,13,128,70,12,55,120,141,6,232,231,143,86,
8,0,50,192,24,41,182,108,2,0,0,0,8,4,233,11,4,100,224,235,128,128,15,84,
218,255,58,192,24,41,182,108,2,0,0,0,96,204,41,1,0,32,38,137,2,0,0,0,
19,0,32,13,1,120,131,0,120,0,0,235,46,255,7,128,0,232,225,143,80,9,0,54,
0,0,64,238,2,120,5,128,251,161,199,33,225,64,120,0,0,235,1,4,224,6,0,58,
18,0,55,192,24,41,182,108,2,0,0,0,16,0,32,28,0,55,214,235,128,0,0,235,
243,15,32,162,199,33,6,2,64,3,2,98,1,113,122,0,0,232,133,0,64,131,128,96,
226,192,120,1,4,224,247,255,58,210,235,128,2,1,233,241,15,32,1,1,97,226,235,136,
2,129,232,0,0,65,1,0,96,208,235,136,0,0,233,129,0,96,0,65,235,160,199,33,
2,240,120,0,0,235,79,6,190,79,5,190,79,4,190,0,128,250,0,0,6,136,159,190,
138,159,190,224,235,128,0,0,232,0,128,251,224,235,136,188,254,7,248,253,7,0,0,224,
69,0,58,0,4,235,9,3,32,170,166,33,8,64,232,62,107,2,0,0,0,208,25,41,
212,107,2,0,0,0,9,192,185,128,2,176,10,0,64,182,108,2,0,0,0,8,4,232,
235,15,84,242,255,58,224,128,41,1,0,32,38,137,2,0,0,0,96,229,33,16,116,161,
19,0,32,178,0,32,131,0,120,3,0,120,214,254,7,0,4,233,0,0,232,225,15,80,
5,0,62,160,174,41,1,0,32,38,137,2,0,0,0,63,0,55,25,229,33,25,128,251,
10,3,32,10,128,185,32,2,176,171,166,33,107,0,120,0,0,1,200,104,2,0,0,0,
136,76,120,8,128,251,10,128,185,126,5,64,107,0,125,0,0,1,192,145,41,1,0,32,
38,137,2,0,0,0,8,0,224,5,0,50,194,34,168,224,8,41,1,0,32,38,137,2,
0,0,0,192,213,41,1,0,32,38,137,2,0,0,0,31,0,55,0,4,233,234,15,84,
24,0,60,25,229,33,25,128,251,10,3,32,10,128,185,32,2,176,171,166,33,107,0,120,
0,0,1,200,104,2,0,0,0,136,76,120,8,128,251,10,128,185,126,5,64,107,0,125,
0,0,1,8,0,224,1,0,50,194,34,168,192,213,41,1,0,32,38,137,2,0,0,0,
4,0,55,224,129,41,1,0,32,38,137,2,0,0,0,208,235,128,0,0,233,0,128,251,
224,235,136,79,5,190,79,4,190,0,0,6,136,31,120,224,235,128,0,0,232,0,128,251,
224,235,136,72,254,7,132,253,7,128,0,233,233,143,80,3,0,62,241,228,33,128,72,233,
13,0,55,96,229,33,16,116,161,224,224,40,1,0,32,38,137,2,0,0,0,128,1,235,
162,0,32,17,0,32,144,0,32,111,254,7,241,228,33,128,72,233,0,4,235,240,228,33,
145,192,179,144,207,16,17,0,58,46,254,7,106,253,7,0,4,120,8,0,224,12,0,58,
96,229,33,16,116,161,224,135,41,1,0,32,38,137,2,0,0,0,128,1,235,242,255,39,
131,0,120,32,2,32,88,254,7,0,4,120,160,139,41,1,0,32,38,137,2,0,0,0,
32,156,41,1,0,32,38,137,2,0,0,0,164,109,2,0,0,0,240,228,33,145,192,179,
144,207,16,3,0,58,8,0,120,174,109,2,0,0,0,136,105,2,0,0,0,8,194,179,
90,106,2,0,0,0,136,79,80,252,255,58,79,4,120,0,0,6,224,235,128,0,0,232,
0,128,251,224,235,136,255,253,7,59,253,7,128,0,233,227,143,80,3,0,62,1,229,33,
128,72,233,13,0,55,96,229,33,16,116,161,128,68,41,1,0,32,38,137,2,0,0,0,
128,1,235,66,0,32,17,0,32,1,0,120,38,254,7,1,229,33,128,72,233,192,138,41,
1,0,32,38,137,2,0,0,0,0,0,6,136,159,190,138,159,190,0,26,41,212,107,2,
0,0,0,224,194,179,98,106,2,0,0,0,224,228,33,16,64,120,98,106,2,0,0,0,
81,242,128,240,68,32,128,143,80,4,0,58,224,133,41,1,0,32,204,136,2,0,0,0,
192,24,41,182,108,2,0,0,0,224,26,41,212,107,2,0,0,0,176,197,179,98,106,2,
0,0,0,25,0,32,0,4,235,10,3,32,171,166,33,2,0,55,8,4,232,137,4,232,
8,0,224,115,0,50,0,194,179,98,106,2,0,0,0,10,192,185,128,2,176,11,0,64,
212,107,2,0,0,0,234,143,84,242,255,54,208,197,179,98,106,2,0,0,0,224,175,41,
1,0,32,204,136,2,0,0,0,161,191,42,0,0,235,60,105,2,0,0,0,0,4,120,
72,64,222,62,107,2,0,0,0,224,194,179,98,106,2,0,0,0,8,64,120,62,107,2,
0,0,0,192,24,41,182,108,2,0,0,0,192,204,41,1,0,32,204,136,2,0,0,0,
168,228,33,24,0,120,120,106,2,0,0,0,192,207,41,1,0,32,204,136,2,0,0,0,
96,242,128,120,106,2,0,0,0,128,181,41,1,0,32,204,136,2,0,0,0,240,68,32,
152,15,16,5,0,58,0,227,41,1,0,32,204,136,2,0,0,0,4,0,55,224,226,41,
1,0,32,204,136,2,0,0,0,96,242,128,33,4,35,129,15,80,17,0,50,4,0,62,
49,0,35,129,15,80,28,0,58,7,0,55,49,4,35,129,15,80,14,0,50,97,4,35,
129,15,80,21,0,58,15,0,55,96,226,41,1,0,32,204,136,2,0,0,0,19,0,55,
128,226,41,1,0,32,204,136,2,0,0,0,14,0,55,160,226,41,1,0,32,204,136,2,
0,0,0,9,0,55,192,226,41,1,0,32,204,136,2,0,0,0,4,0,55,0,220,41,
1,0,32,204,136,2,0,0,0,208,30,41,182,108,2,0,0,0,96,86,41,1,0,32,
38,137,2,0,0,0,79,5,190,79,4,190,0,0,6,10,192,185,128,2,176,11,0,64,
212,107,2,0,0,0,131,255,55,22,0,250,136,159,190,138,159,190,140,159,190,0,0,235,
224,235,136,208,235,136,240,235,136,17,229,33,128,72,120,97,229,33,17,116,161,65,216,33,
71,0,9,128,88,235,96,238,136,234,126,41,11,0,32,172,154,41,13,0,32,10,143,190,
16,229,33,16,129,251,0,3,32,0,145,185,33,169,33,1,0,65,212,107,2,0,0,0,
32,229,33,16,0,120,100,0,96,4,0,50,128,202,41,1,0,32,204,136,2,0,0,0,
160,24,41,212,107,2,0,0,0,73,216,33,250,0,55,96,238,128,0,0,224,68,0,50,
0,0,233,96,238,136,120,1,185,233,64,121,1,4,224,20,0,50,0,4,235,210,235,128,
163,199,33,129,113,121,120,0,185,0,0,68,105,64,120,98,106,2,0,0,0,8,4,232,
208,235,128,0,0,232,0,128,251,208,235,136,96,238,128,120,1,185,130,0,68,233,192,120,
1,4,224,237,255,58,0,0,235,96,238,136,39,0,55,97,229,33,145,64,120,100,192,96,
37,0,50,18,229,33,18,128,251,8,128,185,122,0,64,106,0,120,0,0,1,0,4,224,
29,0,50,19,229,33,19,128,251,8,128,185,128,2,176,10,0,64,212,107,2,0,0,0,
160,24,41,212,107,2,0,0,0,235,235,128,208,235,128,128,143,85,15,0,50,139,15,80,
13,0,50,161,199,33,97,192,125,98,106,2,0,0,0,242,15,32,130,133,101,139,5,232,
208,235,128,139,15,80,3,0,50,245,255,55,8,3,32,170,166,33,174,105,2,0,0,0,
0,4,224,211,255,50,178,109,2,0,0,0,0,4,224,3,0,50,184,109,2,0,0,0,
166,0,55,90,106,2,0,0,0,0,129,251,128,1,235,241,7,32,129,15,81,224,143,89,
157,0,62,2,96,1,147,0,55,140,0,55,131,0,55,152,0,55,125,0,55,138,0,55,
129,0,55,148,0,55,119,0,55,146,0,55,135,0,55,144,0,55,143,0,55,132,0,55,
125,0,55,140,0,55,121,0,55,138,0,55,137,0,55,136,0,55,135,0,55,134,0,55,
133,0,55,132,0,55,131,0,55,130,0,55,129,0,55,104,0,55,127,0,55,126,0,55,
125,0,55,124,0,55,123,0,55,122,0,55,121,0,55,120,0,55,119,0,55,118,0,55,
117,0,55,116,0,55,115,0,55,114,0,55,113,0,55,112,0,55,111,0,55,110,0,55,
109,0,55,108,0,55,107,0,55,106,0,55,105,0,55,104,0,55,103,0,55,102,0,55,
101,0,55,100,0,55,99,0,55,98,0,55,97,0,55,96,0,55,95,0,55,94,0,55,
93,0,55,92,0,55,91,0,55,90,0,55,89,0,55,88,0,55,87,0,55,86,0,55,
85,0,55,84,0,55,83,0,55,82,0,55,81,0,55,80,0,55,79,0,55,78,0,55,
77,0,55,76,0,55,75,0,55,74,0,55,73,0,55,72,0,55,71,0,55,70,0,55,
69,0,55,68,0,55,67,0,55,66,0,55,65,0,55,64,0,55,63,0,55,62,0,55,
61,0,55,60,0,55,59,0,55,58,0,55,57,0,55,56,0,55,55,0,55,54,0,55,
53,0,55,52,0,55,51,0,55,50,0,55,49,0,55,48,0,55,47,0,55,46,0,55,
45,0,55,44,0,55,43,0,55,42,0,55,41,0,55,40,0,55,39,0,55,38,0,55,
37,0,55,36,0,55,35,0,55,34,0,55,33,0,55,32,0,55,31,0,55,30,0,55,
29,0,55,2,0,55,79,249,7,27,0,55,110,249,7,25,0,55,32,250,7,23,0,55,
4,250,7,21,0,55,231,247,7,19,0,55,82,248,7,17,0,55,117,248,7,15,0,55,
108,249,7,13,0,55,131,249,7,11,0,55,173,248,7,9,0,55,33,216,33,17,64,232,
128,72,120,244,79,80,4,0,58,172,65,2,0,0,0,1,0,55,144,249,7,50,216,33,
18,4,224,3,255,50,211,235,128,83,7,152,225,235,128,2,0,120,0,72,235,96,229,33,
16,116,161,32,229,33,16,0,120,100,0,96,7,0,50,252,54,2,0,0,0,192,213,41,
1,0,32,38,137,2,0,0,0,34,4,55,17,15,152,0,64,235,0,79,152,162,191,41,
3,0,32,34,7,152,51,7,152,30,5,190,224,235,128,161,199,33,97,65,120,2,129,251,
128,1,235,225,7,32,129,15,81,224,143,89,235,3,62,2,96,1,235,3,55,232,3,55,
231,3,55,230,3,55,229,3,55,228,3,55,227,3,55,226,3,55,225,3,55,224,3,55,
225,3,55,222,3,55,221,3,55,222,3,55,219,3,55,218,3,55,217,3,55,216,3,55,
215,3,55,214,3,55,213,3,55,212,3,55,211,3,55,210,3,55,209,3,55,208,3,55,
207,3,55,206,3,55,205,3,55,204,3,55,203,3,55,202,3,55,203,3,55,172,3,55,
24,2,55,243,0,55,250,0,55,95,1,55,77,1,55,194,3,55,244,1,55,192,3,55,
191,3,55,190,3,55,191,3,55,95,3,55,120,3,55,67,3,55,127,2,55,126,2,55,
125,2,55,124,2,55,123,2,55,122,2,55,121,2,55,120,2,55,119,2,55,118,2,55,
175,3,55,174,3,55,103,1,55,198,0,55,171,3,55,71,0,55,12,1,55,8,1,55,
167,3,55,106,0,55,44,1,55,166,3,55,163,3,55,162,3,55,161,3,55,160,3,55,
159,3,55,158,3,55,107,0,55,156,3,55,155,3,55,154,3,55,141,0,55,152,3,55,
151,3,55,69,1,55,149,3,55,148,3,55,147,3,55,7,1,55,145,3,55,144,3,55,
143,3,55,49,2,55,35,3,55,64,2,55,86,3,55,58,3,55,137,3,55,229,0,55,
47,0,55,66,0,55,254,0,55,205,0,55,49,0,55,51,0,55,22,0,55,32,0,55,
127,3,55,126,3,55,85,0,55,34,0,55,123,3,55,36,0,55,91,0,55,120,3,55,
172,2,55,34,1,55,117,3,55,116,3,55,31,0,55,233,0,55,113,3,55,112,3,55,
111,3,55,25,2,55,138,0,55,41,2,55,139,0,55,73,252,7,0,0,224,106,3,58,
226,235,128,18,15,152,211,235,128,83,7,152,101,3,55,48,137,2,0,0,0,98,3,55,
139,253,7,16,229,33,16,4,224,94,3,50,38,250,7,92,3,55,168,252,7,90,3,55,
27,253,7,88,3,55,99,253,7,86,3,55,121,249,7,84,3,55,52,93,2,0,0,0,
81,3,55,16,229,33,16,4,224,5,0,58,160,98,41,1,0,32,38,137,2,0,0,0,
73,3,55,32,92,2,0,0,0,70,3,55,96,229,33,193,207,179,16,200,96,224,123,41,
1,0,32,38,137,2,0,0,0,62,3,55,96,229,33,193,207,179,144,192,96,1,4,160,
1,72,120,224,124,41,1,0,32,38,137,2,0,0,0,52,3,55,96,229,33,16,52,160,
32,101,41,1,0,32,204,136,2,0,0,0,192,24,41,182,108,2,0,0,0,42,3,55,
96,229,33,16,52,161,224,99,41,1,0,32,204,136,2,0,0,0,192,24,41,182,108,2,
0,0,0,32,3,55,16,229,33,16,4,224,5,0,58,160,98,41,1,0,32,38,137,2,
0,0,0,24,3,55,203,98,169,201,98,169,224,125,41,1,0,32,204,136,2,0,0,0,
192,24,41,182,108,2,0,0,0,14,3,55,16,229,33,16,4,224,5,0,58,160,98,41,
1,0,32,38,137,2,0,0,0,6,3,55,96,229,33,16,64,120,112,64,96,4,0,58,
0,70,41,1,0,32,38,137,2,0,0,0,201,98,168,203,98,169,224,137,41,1,0,32,
204,136,2,0,0,0,192,24,41,182,108,2,0,0,0,33,227,168,176,0,32,4,105,2,
0,0,0,241,4,32,129,15,80,4,0,62,0,79,41,1,0,32,38,137,2,0,0,0,
33,227,169,232,2,55,0,64,235,6,251,7,229,2,55,16,192,179,3,251,7,226,2,55,
16,229,33,16,4,224,5,0,58,17,192,179,1,64,120,170,120,2,0,0,0,218,2,55,
96,118,41,1,0,32,38,137,2,0,0,0,213,2,55,0,214,41,1,0,32,38,137,2,
0,0,0,164,109,2,0,0,0,0,0,254,205,2,55,194,137,2,0,0,0,0,4,224,
201,2,50,17,192,179,160,127,34,1,72,120,32,191,41,1,0,32,38,137,2,0,0,0,
64,6,32,94,105,2,0,0,0,200,104,2,0,0,0,164,109,2,0,0,0,0,0,254,
185,2,55,192,66,168,194,66,169,156,250,7,0,4,120,8,0,224,179,2,50,201,24,41,
12,0,190,204,136,2,0,0,0,16,22,128,66,0,222,97,0,96,99,246,7,9,0,120,
182,108,2,0,0,0,8,4,233,244,255,58,165,2,55,202,96,2,0,0,0,162,2,55,
174,96,2,0,0,0,159,2,55,132,250,7,0,4,120,8,0,224,155,2,50,201,24,41,
10,0,190,204,136,2,0,0,0,230,96,2,0,0,0,0,128,251,75,246,7,9,0,120,
182,108,2,0,0,0,8,4,233,244,255,58,141,2,55,74,247,7,139,2,55,42,247,7,
137,2,55,32,159,41,1,0,32,204,136,2,0,0,0,148,107,2,0,0,0,96,227,41,
1,0,32,204,136,2,0,0,0,192,24,41,182,108,2,0,0,0,123,2,55,194,108,2,
0,0,0,120,2,55,93,250,7,0,4,120,224,202,41,1,0,32,204,136,2,0,0,0,
8,0,120,76,107,2,0,0,0,192,225,41,1,0,32,38,137,2,0,0,0,8,0,120,
104,105,2,0,0,0,103,2,55,76,250,7,0,4,120,224,202,41,1,0,32,204,136,2,
0,0,0,8,0,120,76,107,2,0,0,0,32,226,41,1,0,32,38,137,2,0,0,0,
8,0,120,94,105,2,0,0,0,86,2,55,32,229,33,16,32,160,83,2,55,16,229,33,
16,4,224,5,0,58,160,98,41,1,0,32,38,137,2,0,0,0,75,2,55,10,97,2,
0,0,0,72,2,55,97,229,33,17,116,160,0,4,232,8,132,251,161,199,33,97,65,124,
2,4,224,23,0,50,22,192,179,17,0,32,229,195,179,128,67,235,244,15,32,163,199,33,
133,79,81,1,0,58,7,67,120,129,0,232,0,129,64,4,1,97,99,65,121,2,4,224,
247,255,58,96,229,33,71,51,221,16,65,120,2,116,161,6,72,113,250,143,80,1,0,52,
16,116,160,96,229,33,16,4,224,38,2,53,232,235,136,113,249,7,0,4,120,224,235,128,
128,128,251,162,199,33,98,193,120,209,195,179,129,79,81,33,0,58,136,0,233,226,143,80,
27,0,62,0,0,232,0,128,251,224,235,136,8,1,233,120,17,185,48,216,33,128,0,65,
1,1,120,0,0,235,128,65,235,3,105,120,0,0,232,248,15,80,252,255,58,224,235,128,
165,199,33,228,195,179,243,15,32,3,0,55,130,104,120,0,0,232,3,0,96,101,65,120,
132,79,81,250,255,58,224,235,136,254,1,55,96,229,33,16,116,160,251,1,55,8,0,224,
31,0,58,64,216,33,0,15,152,17,0,32,248,30,41,169,24,41,1,1,120,34,15,152,
106,7,152,123,7,152,74,216,33,78,80,144,62,107,2,0,0,0,8,0,120,212,107,2,
0,0,0,10,0,120,212,107,2,0,0,0,9,0,120,182,108,2,0,0,0,174,8,144,
129,0,232,33,15,152,120,5,69,228,143,80,238,255,58,110,5,144,254,5,144,218,1,55,
8,0,233,226,15,80,5,0,62,192,24,41,182,108,2,0,0,0,104,238,136,210,1,55,
96,229,33,16,116,160,207,1,55,0,0,232,248,15,32,8,0,96,224,235,136,23,249,7,
128,4,120,216,249,7,224,235,128,8,4,96,160,199,33,224,64,124,144,194,179,128,207,80,
13,0,58,16,229,33,144,128,251,0,3,32,0,136,185,124,0,64,161,166,33,225,0,120,
9,0,120,1,0,1,192,24,41,182,108,2,0,0,0,180,1,55,96,229,33,16,116,160,
177,1,55,97,229,33,17,116,160,128,0,232,129,128,251,162,199,33,226,192,120,1,4,224,
18,0,50,0,128,232,21,192,179,36,194,179,0,67,235,243,15,32,132,207,80,1,0,58,
134,66,120,131,0,96,226,192,120,0,0,232,1,4,224,248,255,58,96,229,33,199,42,221,
144,64,120,1,116,161,5,200,112,96,229,33,16,4,224,148,1,53,64,203,41,1,0,32,
204,136,2,0,0,0,32,194,179,98,106,2,0,0,0,225,235,128,129,0,232,129,128,251,
160,199,33,96,192,120,34,194,179,130,79,80,30,0,50,168,199,33,105,229,33,225,235,136,
98,106,2,0,0,0,224,235,128,104,64,120,0,128,251,25,65,120,232,64,97,4,0,50,
83,229,33,147,64,120,18,105,2,0,0,0,17,229,33,17,129,251,1,3,32,1,145,185,
232,0,65,162,166,33,226,128,120,1,0,1,225,235,128,129,0,232,129,128,251,104,192,120,
34,194,179,130,79,80,228,255,58,225,235,136,32,194,179,98,106,2,0,0,0,192,24,41,
182,108,2,0,0,0,95,1,55,16,229,33,144,128,251,0,3,32,0,136,185,162,166,33,
98,0,120,0,0,1,87,1,55,16,229,33,144,128,251,0,3,32,0,136,185,0,128,232,
161,166,33,97,0,120,0,0,1,78,1,55,16,229,33,144,128,251,0,3,32,0,136,185,
100,0,64,161,166,33,97,0,120,0,0,1,69,1,55,16,229,33,144,128,251,0,3,32,
0,136,185,102,0,64,161,166,33,97,0,120,0,0,1,60,1,55,64,203,41,1,0,32,
204,136,2,0,0,0,132,248,7,0,15,152,224,235,128,0,0,233,0,128,251,224,235,136,
23,249,7,0,0,232,96,7,152,41,249,7,0,0,224,83,0,50,81,229,33,128,72,120,
17,192,179,232,15,80,1,0,60,128,64,235,96,229,33,225,192,96,197,8,221,16,65,120,
2,84,161,1,72,113,70,0,55,14,8,144,58,109,2,0,0,0,1,194,179,99,229,33,
19,192,96,5,0,58,131,192,179,82,229,33,146,207,17,5,0,58,11,0,55,1,193,179,
80,229,33,144,207,16,7,0,50,176,195,179,98,106,2,0,0,0,82,229,33,18,64,120,
62,107,2,0,0,0,99,229,33,147,65,120,104,192,97,6,0,50,80,229,33,144,64,120,
14,8,144,18,105,2,0,0,0,0,15,152,18,229,33,146,128,251,0,3,32,0,136,185,
104,0,64,163,166,33,227,0,120,14,8,144,1,0,1,96,7,152,0,194,179,98,106,2,
0,0,0,2,196,179,97,229,33,17,64,97,24,0,50,8,0,190,204,136,2,0,0,0,
99,229,33,147,65,120,104,192,97,6,0,50,80,229,33,144,64,120,110,0,144,18,105,2,
0,0,0,96,7,152,110,0,144,58,109,2,0,0,0,0,194,179,98,106,2,0,0,0,
4,0,55,8,213,41,9,0,32,238,0,144,33,15,152,46,9,144,2,1,233,34,15,152,
178,255,58,192,24,41,182,108,2,0,0,0,205,0,55,0,213,41,1,0,32,204,136,2,
0,0,0,85,244,7,0,87,152,172,248,7,0,4,120,191,248,7,0,0,224,12,0,50,
81,229,33,128,72,120,17,192,179,232,15,80,1,0,60,128,64,235,96,229,33,225,192,96,
197,8,221,16,65,120,2,84,161,1,72,113,142,81,144,3,4,224,4,0,50,0,229,33,
144,64,120,1,79,152,3,72,233,8,0,224,42,0,50,105,229,33,18,229,33,18,128,251,
3,3,32,3,128,185,106,0,64,161,166,33,97,0,120,0,0,1,25,65,120,232,64,97,
4,0,50,83,229,33,147,64,120,18,105,2,0,0,0,58,109,2,0,0,0,1,194,179,
25,192,96,5,0,58,131,192,179,82,229,33,146,207,17,5,0,58,11,0,55,1,193,179,
80,229,33,144,207,16,7,0,50,176,195,179,98,106,2,0,0,0,82,229,33,18,64,120,
62,107,2,0,0,0,0,194,179,98,106,2,0,0,0,8,4,233,215,255,58,142,81,144,
3,4,224,3,0,50,0,229,33,142,72,144,1,72,120,192,24,41,182,108,2,0,0,0,
121,0,55,160,203,41,1,0,32,38,137,2,0,0,0,16,229,33,144,128,251,0,3,32,
0,136,185,108,0,64,161,166,33,97,0,120,0,0,1,108,0,55,0,204,41,1,0,32,
38,137,2,0,0,0,16,229,33,144,128,251,0,3,32,0,136,185,110,0,64,161,166,33,
97,0,120,0,0,1,95,0,55,192,172,41,1,0,32,38,137,2,0,0,0,16,229,33,
144,128,251,0,3,32,0,136,185,112,0,64,161,166,33,97,0,120,0,0,1,82,0,55,
96,173,41,1,0,32,38,137,2,0,0,0,16,229,33,144,128,251,0,3,32,0,136,185,
114,0,64,161,166,33,97,0,120,0,0,1,69,0,55,32,172,41,1,0,32,204,136,2,
0,0,0,16,229,33,144,128,251,0,3,32,0,136,185,116,0,64,161,166,33,97,0,120,
0,0,1,240,243,7,55,0,55,28,248,7,0,4,120,0,174,41,1,0,32,204,136,2,
0,0,0,8,0,120,58,109,2,0,0,0,8,0,224,10,0,50,25,229,33,153,128,251,
0,3,32,0,136,185,118,0,64,162,166,33,98,0,120,0,0,1,8,4,233,247,255,58,
192,24,41,182,108,2,0,0,0,30,0,55,3,248,7,0,4,120,46,0,144,190,0,144,
204,136,2,0,0,0,8,0,224,15,0,50,25,229,33,153,128,251,0,3,32,0,136,185,
120,0,64,163,166,33,99,0,120,0,0,1,0,128,251,196,243,7,0,194,179,98,106,2,
0,0,0,8,4,233,242,255,58,96,155,41,1,0,32,38,137,2,0,0,0,2,0,55,
96,229,33,16,116,160,224,235,128,0,0,232,0,128,251,224,235,136,0,64,235,97,229,33,
17,4,224,22,0,61,224,127,41,1,0,32,204,136,2,0,0,0,224,235,128,158,8,144,
128,143,80,4,0,49,1,64,80,62,107,2,0,0,0,4,0,55,46,81,144,2,64,80,
62,107,2,0,0,0,96,229,33,16,116,161,192,24,41,182,108,2,0,0,0,16,192,179,
209,235,128,226,235,128,129,15,81,2,0,50,0,4,224,230,251,50,222,1,144,227,235,136,
211,235,136,48,216,33,0,72,235,180,250,55,16,208,128,33,128,33,97,65,120,49,200,179,
129,79,81,14,0,58,0,0,232,16,208,136,33,128,33,97,65,120,1,206,179,129,79,81,
7,0,62,33,128,33,129,0,64,2,206,179,0,0,232,209,79,17,253,255,54,16,208,136,
0,0,6,34,128,33,18,66,120,0,65,235,3,206,179,131,79,82,35,0,54,50,128,33,
146,129,251,200,25,221,66,128,33,18,129,251,2,129,65,130,15,80,16,0,58,11,0,55,
130,2,232,99,195,122,72,51,221,130,130,232,227,194,122,133,130,251,133,2,67,133,15,80,
3,0,50,4,66,176,8,0,55,0,1,235,130,8,120,18,192,179,11,0,55,4,66,176,
0,1,235,35,128,33,7,206,179,4,130,251,99,2,66,4,1,65,99,66,121,135,79,82,
231,255,62,0,65,235,2,64,120,0,0,6,0,1,120,230,235,128,146,64,120,16,192,179,
1,4,224,22,0,50,160,199,33,96,64,123,128,207,80,7,0,50,3,0,55,229,193,120,
131,79,81,7,0,50,230,235,136,0,64,235,11,0,55,2,0,120,134,0,120,244,15,32,
165,199,33,129,0,232,132,128,96,225,235,136,80,65,120,2,4,224,240,255,58,16,192,179,
0,0,6,136,159,190,138,31,120,9,3,41,0,4,235,42,2,32,89,0,120,220,255,7,
0,4,224,3,0,50,0,200,179,0,64,68,4,0,55,8,4,232,138,15,84,246,255,58,
0,64,235,79,5,120,79,4,190,0,0,6,136,159,190,138,31,120,0,0,235,16,208,136,
32,128,33,16,68,120,8,4,224,47,0,50,42,3,41,128,4,120,8,4,224,4,0,53,
8,64,120,98,106,2,0,0,0,33,0,55,0,206,179,128,79,84,18,0,54,96,246,40,
182,108,2,0,0,0,16,208,128,128,128,232,0,0,232,105,65,120,72,17,221,105,192,120,
0,128,251,0,0,65,17,208,136,76,107,2,0,0,0,0,194,179,98,106,2,0,0,0,
12,0,55,0,194,179,98,106,2,0,0,0,8,132,251,8,8,177,8,4,68,106,0,124,
212,107,2,0,0,0,0,194,179,98,106,2,0,0,0,16,208,128,0,0,232,16,208,136,
105,68,120,8,4,224,211,255,58,96,246,40,182,108,2,0,0,0,16,208,128,0,0,233,
76,107,2,0,0,0,32,202,41,1,0,32,38,137,2,0,0,0,79,5,120,79,4,190,
0,0,6,136,31,120,113,201,179,129,79,80,58,0,50,10,0,62,65,201,179,129,79,80,
22,0,50,97,201,179,129,79,80,42,0,50,49,201,179,129,79,80,72,0,58,24,0,55,
161,201,179,129,79,80,54,0,50,4,0,62,129,201,179,129,79,80,64,0,58,57,0,55,
177,201,179,129,79,80,49,0,50,209,201,179,129,79,80,57,0,58,19,0,55,16,229,33,
144,128,251,0,3,32,0,136,185,106,0,64,161,166,33,97,0,120,0,0,1,48,0,55,
16,229,33,144,128,251,0,3,32,0,136,185,104,0,64,161,166,33,97,4,120,145,0,7,
8,0,1,38,0,55,230,96,2,0,0,0,0,128,251,34,0,55,16,229,33,144,128,251,
0,3,32,0,136,185,116,0,64,161,166,33,97,0,120,0,0,1,25,0,55,16,229,33,
144,128,251,0,3,32,0,136,185,120,0,64,161,166,33,97,0,120,0,0,1,0,128,251,
15,0,55,16,22,128,97,0,96,12,0,55,80,22,128,75,0,222,97,0,96,0,128,234,
7,0,55,33,227,168,192,0,32,4,105,2,0,0,0,33,227,169,1,0,55,0,0,235,
79,4,120,0,0,6,18,208,128,32,128,33,224,64,121,128,194,179,128,207,80,12,0,58,
2,1,232,18,208,136,94,0,7,17,208,128,34,128,33,226,193,120,146,194,179,130,207,81,
43,0,58,129,0,232,17,208,136,40,0,55,240,203,179,0,192,64,249,79,80,8,0,62,
129,128,251,17,4,177,129,128,64,160,160,33,96,128,120,2,1,232,18,208,136,28,0,55,
0,200,179,128,207,80,6,0,62,0,205,179,0,192,64,233,79,80,7,0,54,0,0,235,
19,0,55,2,1,232,18,208,136,1,64,120,124,255,7,14,0,55,35,128,33,131,1,65,
0,0,235,6,205,179,106,2,185,1,128,251,4,0,64,0,3,177,2,1,232,211,64,120,
6,194,64,233,79,82,247,255,54,18,208,136,0,0,6,136,159,190,138,31,120,194,255,7,
0,4,120,42,128,33,169,194,179,17,208,128,106,192,120,129,0,232,17,208,136,137,79,80,
12,0,50,4,0,62,97,194,179,129,79,80,24,0,58,17,0,55,241,194,179,129,79,80,
8,0,50,193,199,179,129,79,80,17,0,58,13,0,55,172,255,7,0,192,185,0,4,120,
234,255,55,168,255,7,128,1,120,17,0,9,3,4,216,0,4,120,228,255,55,162,255,7,
0,4,100,225,255,55,159,255,7,8,4,112,222,255,55,8,0,120,79,5,120,79,4,190,
0,0,6,136,159,190,138,31,120,209,255,7,0,4,120,42,128,33,201,195,179,17,208,128,
106,192,120,129,0,232,17,208,136,137,79,80,43,0,50,7,0,62,177,194,179,129,79,80,
14,0,50,209,194,179,129,79,80,78,0,58,7,0,55,210,195,179,130,79,80,67,0,50,
226,195,179,130,79,80,71,0,58,6,0,55,184,255,7,0,4,84,232,255,55,181,255,7,
0,4,68,229,255,55,129,0,232,234,192,120,208,195,179,128,207,80,10,0,58,173,255,7,
17,0,32,136,15,80,1,0,52,128,0,235,1,4,120,16,208,128,0,0,232,16,208,136,
214,255,55,163,255,7,17,0,32,136,15,80,1,0,53,128,0,235,1,4,120,207,255,55,
129,0,232,106,192,120,225,195,179,129,79,80,10,0,58,151,255,7,8,4,104,8,240,167,
8,4,234,8,4,234,79,68,222,16,208,128,0,0,232,16,208,136,192,255,55,209,195,179,
129,79,80,10,0,58,138,255,7,17,0,32,136,15,80,1,0,61,128,0,235,1,4,120,
16,208,128,0,0,232,16,208,136,179,255,55,128,255,7,17,0,32,136,15,80,1,0,60,
128,0,235,1,4,120,172,255,55,121,255,7,8,4,104,8,240,167,8,4,234,8,4,233,
79,68,222,165,255,55,8,0,120,79,5,120,79,4,190,0,0,6,14,0,250,136,159,190,
138,159,190,140,159,190,0,0,235,16,208,136,32,208,136,48,208,136,64,208,136,32,229,33,
16,0,160,160,160,33,25,0,9,0,24,235,128,0,235,1,6,120,129,5,120,237,200,179,
42,194,179,1,0,224,16,0,58,16,208,128,33,128,33,225,69,120,241,205,179,129,207,85,
69,3,54,139,133,251,11,14,177,0,1,232,33,128,33,97,70,121,72,102,221,0,128,232,
97,64,120,0,128,251,12,6,112,20,208,128,99,1,66,32,128,33,96,64,121,0,128,251,
128,0,235,0,8,177,1,128,177,19,2,32,131,15,80,224,143,88,8,3,62,0,96,1,
33,0,55,47,0,55,4,3,55,3,3,55,89,0,55,101,0,55,125,0,55,141,0,55,
145,0,55,212,0,55,12,1,55,251,2,55,80,1,55,153,1,55,233,1,55,250,1,55,
236,1,55,20,2,55,5,2,55,32,2,55,242,2,55,162,2,55,135,2,55,239,2,55,
238,2,55,69,2,55,54,2,55,195,2,55,208,2,55,37,2,55,82,2,55,109,2,55,
213,2,55,227,2,55,102,0,66,16,208,136,100,2,66,32,128,33,96,68,122,8,132,251,
24,4,177,63,255,7,8,4,68,161,160,33,128,48,124,198,253,7,128,0,235,0,68,235,
218,2,55,100,2,66,20,208,136,53,255,7,0,0,224,10,0,58,17,208,128,32,128,33,
96,192,120,50,200,179,130,79,80,29,0,50,2,206,179,130,79,80,15,0,54,235,2,55,
16,208,128,33,128,33,225,65,120,0,0,232,16,208,136,128,0,235,0,68,235,34,200,179,
130,207,81,193,2,58,99,0,80,16,208,136,17,0,32,189,2,55,34,128,33,2,129,64,
51,200,179,4,206,179,129,0,232,82,64,120,131,79,80,3,0,50,132,79,80,250,255,54,
209,2,55,129,128,233,17,208,136,17,0,32,0,68,235,173,2,55,100,2,66,20,208,136,
8,255,7,142,0,120,166,253,7,128,0,235,104,192,179,0,4,224,164,2,50,30,2,120,
20,208,136,0,68,235,160,2,55,100,2,66,20,208,136,48,208,128,128,0,235,120,192,179,
233,15,80,153,2,62,128,0,64,226,163,33,129,0,65,4,2,233,139,8,66,0,0,232,
48,208,136,239,254,7,142,0,120,141,253,7,128,0,235,104,192,179,0,4,224,139,2,50,
158,2,120,21,208,136,0,68,235,135,2,55,112,246,40,212,107,2,0,0,0,48,208,128,
128,0,235,136,192,179,0,0,224,127,2,50,0,0,233,48,208,136,0,0,64,225,163,33,
225,0,120,17,208,136,128,0,235,0,68,235,118,2,55,2,129,69,18,208,136,128,0,235,
0,68,235,113,2,55,100,2,66,20,208,136,41,128,33,40,229,33,39,0,55,138,79,80,
17,0,58,129,0,232,105,192,120,138,79,80,10,0,50,24,0,161,129,0,232,17,208,136,
98,106,2,0,0,0,24,0,160,17,208,128,105,192,120,138,79,80,246,255,58,129,0,232,
17,208,136,20,0,55,242,203,179,2,65,64,249,79,81,5,0,54,3,200,179,131,65,64,
242,197,179,130,207,81,6,0,62,173,254,7,24,0,161,76,107,2,0,0,0,24,0,160,
5,0,55,178,195,179,130,79,80,2,0,58,129,0,232,17,208,136,17,208,128,105,192,120,
242,205,179,130,79,80,3,0,62,50,200,179,130,79,80,209,255,58,129,0,233,32,128,33,
224,192,120,176,195,179,128,207,80,6,0,50,40,229,33,24,0,161,96,246,40,182,108,2,
0,0,0,24,0,160,25,253,7,128,0,235,0,68,235,45,2,55,32,229,33,16,0,161,
17,208,128,228,128,64,17,208,136,32,128,33,96,192,120,138,79,80,16,0,58,129,0,232,
32,128,33,96,192,120,138,79,80,9,0,50,40,128,33,129,0,232,17,208,136,98,106,2,
0,0,0,17,208,128,104,192,120,138,79,80,248,255,58,129,0,232,17,208,136,16,208,128,
33,128,33,97,65,120,56,192,179,193,194,179,129,79,81,3,0,58,0,0,232,16,208,136,
0,68,235,16,208,128,33,128,33,225,68,120,137,132,251,25,4,177,128,1,235,242,255,39,
131,0,120,3,0,120,150,28,2,0,0,0,137,132,68,161,160,33,128,176,124,16,208,128,
0,0,232,16,208,136,225,252,7,32,229,33,16,0,160,128,0,235,244,1,55,100,0,66,
33,208,128,72,192,179,227,143,80,3,0,62,129,0,232,33,208,136,0,68,235,33,128,33,
225,68,120,137,132,251,9,4,177,34,208,128,102,17,185,68,165,33,2,1,66,9,73,120,
200,73,222,19,65,152,0,0,232,16,208,136,97,65,120,209,195,179,129,79,81,8,0,58,
0,0,232,16,208,136,54,254,7,137,4,233,137,132,68,161,160,33,128,176,124,1,0,55,
56,192,179,16,208,128,33,128,33,225,64,120,0,0,232,16,208,136,176,200,179,128,207,80,
9,0,58,41,208,128,38,254,7,102,73,185,97,165,33,130,128,64,128,72,120,72,0,222,
144,64,152,1,0,55,56,192,179,16,208,128,33,128,33,97,65,120,241,205,179,129,79,81,
8,0,54,34,208,128,102,17,185,33,165,33,130,128,64,128,72,120,72,0,222,144,64,152,
1,0,55,56,192,179,153,252,7,128,0,235,174,1,55,100,0,66,33,128,33,225,66,120,
133,130,251,5,4,177,101,2,66,38,208,128,134,1,120,0,0,235,88,192,179,33,165,33,
133,3,233,135,131,67,169,160,33,7,129,68,18,7,152,85,7,152,100,7,152,1,0,55,
3,3,120,102,2,185,36,7,152,53,7,152,4,129,232,97,66,121,4,130,251,2,129,64,
18,65,144,72,17,221,2,1,114,222,2,144,133,15,81,32,0,58,105,129,123,66,7,152,
102,2,185,36,7,152,53,7,152,100,1,66,97,68,121,8,132,251,2,129,64,18,65,144,
72,17,221,2,1,116,206,2,144,130,143,82,15,0,49,30,1,144,5,9,232,102,2,185,
36,7,152,53,7,152,4,1,120,97,66,121,4,130,251,2,129,64,146,66,144,200,42,221,
133,2,114,101,7,152,0,68,235,2,0,55,134,1,233,0,68,235,0,0,232,128,143,81,
207,255,49,110,2,144,20,208,136,35,208,136,79,252,7,128,0,235,100,1,55,100,2,66,
20,208,136,0,68,235,64,208,128,0,0,224,31,0,58,80,128,33,144,207,22,19,0,50,
32,128,33,144,64,120,152,192,179,1,4,224,23,0,50,0,0,235,34,128,33,129,128,251,
209,13,177,1,0,64,227,0,64,226,192,120,141,207,80,6,0,50,226,64,120,1,4,224,
246,255,58,152,192,179,9,0,55,0,0,235,33,128,33,225,64,120,152,192,179,1,4,224,
3,0,50,100,0,64,64,208,136,0,68,235,25,208,128,69,208,128,21,208,136,33,128,33,
97,192,124,0,128,251,242,251,47,2,1,64,34,7,152,44,253,7,174,1,144,131,128,65,
162,160,33,0,177,120,16,208,128,64,208,136,137,4,232,25,208,136,36,128,33,100,65,120,
193,194,179,129,79,81,2,0,58,0,0,232,64,208,136,64,208,128,33,128,33,97,65,120,
1,206,179,129,79,81,10,0,54,227,0,64,34,128,33,226,192,120,141,207,80,3,0,50,
0,0,235,64,208,136,2,0,55,100,0,64,64,208,136,254,251,7,128,0,235,19,1,55,
2,129,69,18,208,136,128,0,235,0,68,235,14,1,55,100,2,66,20,208,136,16,229,33,
144,128,251,0,3,32,0,136,185,162,166,33,98,0,120,0,0,1,237,251,7,128,0,235,
0,68,235,1,1,55,100,2,66,20,208,136,16,229,33,144,128,251,0,3,32,0,136,185,
0,128,232,161,166,33,97,0,120,0,0,1,223,251,7,128,0,235,0,68,235,243,0,55,
100,2,66,20,208,136,16,229,33,144,128,251,0,3,32,0,136,185,100,0,64,161,166,33,
97,0,120,0,0,1,209,251,7,128,0,235,0,68,235,229,0,55,100,2,66,20,208,136,
16,229,33,144,128,251,0,3,32,0,136,185,102,0,64,161,166,33,97,0,120,0,0,1,
195,251,7,128,0,235,0,68,235,215,0,55,100,2,66,20,208,136,16,229,33,144,128,251,
0,3,32,0,136,185,104,0,64,161,166,33,97,4,120,43,253,7,8,0,1,180,251,7,
128,0,235,0,68,235,200,0,55,100,2,66,20,208,136,35,253,7,0,0,224,3,0,50,
174,96,2,0,0,0,2,0,55,202,96,2,0,0,0,166,251,7,128,0,235,0,68,235,
186,0,55,100,2,66,20,208,136,21,253,7,0,0,224,3,0,50,192,2,169,194,2,168,
2,0,55,192,2,169,194,2,169,152,251,7,128,0,235,0,68,235,172,0,55,100,2,66,
20,208,136,7,253,7,0,0,224,6,0,50,96,229,33,193,207,179,144,192,96,1,4,160,
1,72,120,3,0,55,96,229,33,193,207,179,16,200,96,134,251,7,128,0,235,0,68,235,
154,0,55,247,252,7,0,4,120,245,252,7,128,0,120,16,208,128,100,0,64,16,208,136,
8,0,224,1,0,61,0,4,235,8,0,120,5,250,32,133,15,80,1,0,52,5,0,120,
1,0,224,1,0,60,17,0,32,66,6,32,130,143,80,1,0,52,130,0,120,200,91,2,
0,0,0,106,251,7,128,0,235,0,68,235,126,0,55,219,252,7,17,208,128,228,128,64,
17,208,136,0,0,224,1,0,60,16,0,32,65,6,32,129,15,80,1,0,52,1,0,120,
24,92,2,0,0,0,89,251,7,128,0,235,0,68,235,109,0,55,100,2,66,20,208,136,
200,252,7,0,0,224,9,0,50,16,229,33,144,128,251,0,3,32,0,136,185,112,0,64,
161,166,33,97,0,120,0,0,1,8,0,55,16,229,33,144,128,251,0,3,32,0,136,185,
114,0,64,161,166,33,97,0,120,0,0,1,63,251,7,128,0,235,0,68,235,83,0,55,
100,2,66,20,208,136,174,252,7,225,15,80,13,0,50,226,15,80,20,0,50,0,0,224,
26,0,58,16,229,33,144,128,251,0,3,32,0,136,185,110,0,64,161,166,33,97,0,120,
0,0,1,17,0,55,16,229,33,144,128,251,0,3,32,0,136,185,108,0,64,161,166,33,
97,0,120,0,0,1,8,0,55,16,229,33,144,128,251,0,3,32,0,136,185,118,0,64,
161,166,33,97,0,120,0,0,1,24,251,7,128,0,235,0,68,235,44,0,55,100,2,66,
20,208,136,135,252,7,0,0,224,3,0,50,201,98,168,203,98,169,2,0,55,203,98,169,
201,98,169,10,251,7,128,0,235,0,68,235,30,0,55,100,2,66,20,208,136,121,252,7,
94,105,2,0,0,0,1,251,7,128,0,235,0,68,235,21,0,55,100,2,66,20,208,136,
112,252,7,17,229,33,17,129,251,1,3,32,1,145,185,252,0,65,162,166,33,226,128,120,
1,0,1,242,250,7,128,0,235,0,68,235,6,0,55,24,192,179,1,0,55,56,192,179,
11,1,65,18,208,136,128,0,235,8,4,224,212,252,50,32,229,33,16,0,161,225,79,84,
33,0,54,96,201,41,1,0,32,204,136,2,0,0,0,8,128,251,76,107,2,0,0,0,
192,201,41,1,0,32,204,136,2,0,0,0,12,0,120,76,107,2,0,0,0,32,190,41,
1,0,32,204,136,2,0,0,0,16,208,128,76,107,2,0,0,0,96,246,40,182,108,2,
0,0,0,8,0,55,17,208,136,128,0,235,0,68,235,221,255,55,32,229,33,16,0,161,
40,192,179,223,255,55,79,6,190,79,5,190,79,4,190,0,128,250,0,0,6,17,206,179,
32,128,33,1,72,120,0,192,235,49,128,33,128,72,120,65,128,33,128,72,120,17,202,179,
80,128,33,1,72,120,96,128,33,251,1,9,0,88,235,0,0,6,38,0,250,136,159,190,
138,159,190,231,235,128,212,235,128,132,143,83,14,0,50,7,0,120,163,199,33,246,201,179,
245,15,32,227,64,120,6,193,64,249,79,81,2,0,62,1,84,161,129,113,120,0,0,232,
5,0,96,132,15,80,246,255,58,160,199,33,224,192,123,0,205,179,0,192,64,233,79,80,
158,0,62,14,0,233,34,2,32,14,1,65,128,64,235,1,104,120,130,15,80,253,255,58,
158,26,2,0,0,0,200,128,222,17,71,152,32,71,152,65,2,32,142,128,64,153,250,7,
0,4,224,29,0,50,174,17,144,32,128,33,224,192,121,129,128,251,3,0,120,212,45,32,
1,2,82,132,143,81,13,0,61,129,128,65,225,13,177,34,128,33,130,128,64,3,0,65,
4,1,65,81,88,120,130,15,80,253,255,58,3,0,232,131,129,234,4,130,65,4,0,64,
2,32,32,0,1,81,33,128,33,1,0,64,128,0,235,146,14,2,0,0,0,36,28,2,
0,0,0,128,68,235,56,0,32,170,199,33,43,194,179,32,0,55,9,4,224,10,0,58,
36,28,2,0,0,0,182,250,7,128,128,251,5,0,50,0,119,124,112,8,32,128,143,80,
12,0,58,10,0,55,224,235,128,234,64,120,139,207,80,1,0,58,9,4,162,1,119,124,
0,0,232,0,128,251,224,235,136,1,0,55,25,192,179,8,4,232,64,2,32,128,15,84,
5,0,58,160,190,41,1,0,32,38,137,2,0,0,0,107,0,55,208,235,128,225,235,128,
128,143,80,220,255,58,99,0,84,225,15,80,100,0,54,192,205,179,0,68,68,8,79,120,
32,128,33,144,67,120,158,68,144,200,76,221,46,64,144,0,128,251,128,132,68,128,2,235,
5,0,120,34,128,33,3,206,179,7,69,120,28,0,55,131,207,83,10,0,62,80,0,55,
0,0,224,251,255,50,37,23,152,8,129,251,210,13,177,128,0,120,128,143,82,21,0,54,
31,0,55,138,64,120,129,128,251,1,14,177,0,2,232,98,67,122,72,51,221,0,130,232,
98,66,122,4,130,251,4,2,67,137,15,82,2,0,49,227,2,64,129,130,66,227,128,64,
1,0,64,226,64,120,131,207,80,228,255,54,237,255,55,0,2,232,33,128,33,1,2,66,
128,1,65,131,1,232,129,129,65,196,97,120,0,0,233,128,0,120,128,143,82,251,255,54,
133,130,64,32,128,33,0,128,66,129,0,71,132,14,2,0,0,0,34,0,55,224,246,40,
51,250,7,0,4,224,5,0,50,202,251,7,96,246,40,182,108,2,0,0,0,25,0,55,
32,247,40,42,250,7,0,4,224,2,0,50,87,250,7,19,0,55,112,247,40,36,250,7,
0,4,224,3,0,50,32,229,33,16,32,161,12,0,55,192,247,40,29,250,7,0,4,224,
2,0,50,29,255,7,6,0,55,128,171,41,1,0,32,38,137,2,0,0,0,1,0,55,
255,255,55,79,5,190,79,4,190,0,128,250,0,0,6,136,31,120,192,0,32,4,105,2,
0,0,0,0,4,120,72,64,222,98,106,2,0,0,0,8,64,120,98,106,2,0,0,0,
79,4,120,0,0,6,192,219,41,1,0,32,204,136,2,0,0,0,0,0,6,136,159,190,
128,4,120,0,64,235,150,118,2,0,0,0,0,64,235,150,118,2,0,0,0,0,64,235,
150,118,2,0,0,0,0,64,235,150,118,2,0,0,0,9,4,120,25,128,251,180,118,2,
0,0,0,88,128,251,180,118,2,0,0,0,24,64,144,0,128,251,180,118,2,0,0,0,
79,4,190,0,0,6,136,159,190,138,31,120,1,5,120,225,255,7,10,0,224,7,0,50,
0,4,235,233,45,41,9,0,120,219,255,7,8,4,232,136,15,85,251,255,62,79,5,120,
79,4,190,0,0,6,136,159,190,138,31,120,1,5,120,10,0,224,10,0,50,128,68,120,
0,4,235,137,132,251,97,128,100,150,118,2,0,0,0,137,4,209,8,4,232,136,15,85,
248,255,62,79,5,120,79,4,190,0,0,6,136,31,120,16,192,179,150,118,2,0,0,0,
0,64,235,150,118,2,0,0,0,0,64,235,150,118,2,0,0,0,0,64,235,150,118,2,
0,0,0,0,0,235,180,118,2,0,0,0,74,119,2,0,0,0,0,4,120,74,119,2,
0,0,0,98,106,2,0,0,0,8,64,120,98,106,2,0,0,0,224,45,41,167,255,7,
224,45,41,165,255,7,79,4,120,0,0,6,136,159,190,138,159,190,140,31,120,129,69,120,
2,70,120,0,133,251,70,85,222,0,4,235,14,0,55,227,15,84,12,0,58,10,4,224,
10,0,50,16,192,179,130,118,2,0,0,0,10,128,251,94,105,2,0,0,0,0,64,235,
130,118,2,0,0,0,8,0,55,128,132,251,97,128,100,150,118,2,0,0,0,9,0,209,
8,4,232,228,15,84,234,255,58,11,128,251,180,118,2,0,0,0,12,128,251,180,118,2,
0,0,0,79,6,120,79,5,190,79,4,190,0,0,6,2,0,250,136,159,190,128,0,235,
209,191,159,128,64,120,0,64,235,170,120,2,0,0,0,0,68,120,8,4,224,3,0,50,
192,34,169,194,34,168,2,0,55,192,34,169,194,34,169,8,64,120,98,106,2,0,0,0,
128,132,235,174,105,2,0,0,0,0,4,224,14,0,50,90,106,2,0,0,0,225,79,64,
4,0,50,8,64,64,98,106,2,0,0,0,6,0,55,16,192,179,98,106,2,0,0,0,
79,4,190,0,128,250,0,0,6,8,4,224,236,255,58,95,184,151,0,0,224,10,0,58,
217,191,159,192,34,169,194,2,213,0,128,234,97,0,96,0,0,64,17,22,128,1,16,161,
129,0,112,17,22,136,223,184,151,129,0,233,209,191,159,219,255,55,128,128,251,68,9,222,
97,1,97,74,17,221,67,22,128,3,160,161,131,1,113,67,22,136,70,9,221,2,32,178,
67,22,128,3,144,161,131,1,113,67,22,136,70,9,221,2,16,178,67,22,128,3,128,161,
131,1,113,67,22,136,198,8,221,1,8,178,66,22,128,2,112,161,2,129,112,66,22,136,
97,0,96,70,0,221,65,22,128,1,96,161,129,0,112,65,22,136,80,0,32,104,105,2,
0,0,0,128,65,235,203,66,171,1,0,50,3,193,179,0,65,235,203,34,171,1,0,50,
130,192,179,128,64,235,203,2,171,1,0,50,65,192,179,0,64,235,202,226,171,1,0,50,
32,192,179,4,4,32,85,22,128,5,2,98,4,2,234,79,34,222,132,193,113,2,193,113,
129,64,113,0,192,112,0,0,6,128,128,251,1,104,163,3,0,50,192,2,169,194,2,168,
2,0,55,192,2,169,194,2,169,1,88,163,3,0,50,201,98,168,203,98,169,2,0,55,
203,98,169,201,98,169,68,9,222,97,1,97,74,17,221,83,22,128,3,160,161,131,1,113,
83,22,136,70,9,221,2,32,178,83,22,128,3,144,161,131,1,113,83,22,136,70,9,221,
2,16,178,83,22,128,3,128,161,131,1,113,83,22,136,198,8,221,1,8,178,82,22,128,
2,112,161,2,129,112,82,22,136,97,0,96,70,0,221,81,22,128,1,96,161,129,0,112,
81,22,136,80,0,32,104,105,2,0,0,0,128,65,235,203,66,171,1,0,50,3,193,179,
0,65,235,203,34,171,1,0,50,130,192,179,128,64,235,203,2,171,1,0,50,65,192,179,
0,64,235,202,226,171,1,0,50,32,192,179,4,4,32,85,22,128,5,2,98,4,2,234,
79,34,222,132,193,113,2,193,113,129,64,113,0,192,112,0,0,6,0,192,235,118,255,7,
0,64,235,177,255,7,0,0,6,0,128,251,0,56,163,3,0,50,192,2,169,194,2,168,
2,0,55,192,2,169,194,2,169,0,40,163,3,0,50,201,98,168,203,98,169,2,0,55,
203,98,169,201,98,169,201,66,169,128,0,209,225,128,96,202,8,221,82,22,128,2,160,161,
2,129,112,82,22,136,0,8,163,9,0,50,96,229,33,16,64,120,112,64,96,2,0,50,
200,194,168,5,0,55,204,194,168,200,194,169,2,0,55,204,194,169,200,194,169,0,0,6,
232,135,71,136,159,190,138,159,190,140,159,190,142,31,120,96,229,33,144,64,120,1,68,160,
1,52,161,1,72,120,24,192,179,64,229,33,8,72,120,129,192,179,80,229,33,1,72,120,
0,232,33,144,64,120,1,4,161,102,200,112,49,0,32,32,192,179,72,117,2,0,0,0,
201,34,169,201,2,169,200,226,168,8,64,120,120,118,2,0,0,0,64,224,41,1,0,32,
204,136,2,0,0,0,108,229,33,14,232,33,74,224,41,11,0,32,90,106,2,0,0,0,
0,68,120,200,239,159,226,79,68,4,0,58,116,128,87,64,135,2,0,0,0,246,255,55,
8,128,251,68,0,222,0,128,251,128,0,235,0,96,1,15,0,55,133,0,55,178,0,55,
185,0,55,31,2,55,26,2,55,35,2,55,89,2,55,51,2,55,87,2,55,200,0,55,
85,2,55,84,2,55,83,2,55,82,2,55,77,2,55,8,132,251,128,4,235,237,15,84,
224,143,92,110,0,62,8,96,1,78,2,55,12,0,55,15,0,55,21,0,55,26,0,55,
32,0,55,38,0,55,56,0,55,60,0,55,64,0,55,70,0,55,76,0,55,82,0,55,
88,0,55,10,0,190,204,136,2,0,0,0,201,255,55,0,0,235,234,117,2,0,0,0,
16,192,179,98,106,2,0,0,0,194,255,55,202,117,2,0,0,0,16,192,179,98,106,2,
0,0,0,188,255,55,0,64,235,120,118,2,0,0,0,16,192,179,98,106,2,0,0,0,
181,255,55,16,192,179,120,118,2,0,0,0,16,192,179,98,106,2,0,0,0,174,255,55,
158,64,120,97,192,96,3,0,58,74,119,2,0,0,0,3,0,55,240,15,32,142,119,2,
0,0,0,28,65,120,232,64,97,4,0,50,81,229,33,145,64,120,18,105,2,0,0,0,
98,106,2,0,0,0,155,255,55,36,119,2,0,0,0,98,106,2,0,0,0,150,255,55,
30,119,2,0,0,0,98,106,2,0,0,0,145,255,55,16,0,32,156,117,2,0,0,0,
16,192,179,98,106,2,0,0,0,138,255,55,0,64,235,130,118,2,0,0,0,16,192,179,
98,106,2,0,0,0,131,255,55,16,192,179,130,118,2,0,0,0,16,192,179,98,106,2,
0,0,0,124,255,55,0,64,235,140,118,2,0,0,0,16,192,179,98,106,2,0,0,0,
117,255,55,16,192,179,140,118,2,0,0,0,16,192,179,98,106,2,0,0,0,110,255,55,
0,64,235,98,106,2,0,0,0,106,255,55,111,4,100,136,6,232,16,192,179,98,106,2,
0,0,0,13,0,224,99,255,50,0,4,235,249,15,32,90,106,2,0,0,0,0,128,251,
28,65,120,232,64,97,4,0,50,82,229,33,146,64,120,18,105,2,0,0,0,30,65,120,
225,64,97,7,0,58,9,0,96,180,118,2,0,0,0,16,192,179,98,106,2,0,0,0,
13,0,55,9,0,96,142,119,2,0,0,0,28,65,120,232,64,97,4,0,50,82,229,33,
146,64,120,18,105,2,0,0,0,9,0,96,180,118,2,0,0,0,8,4,232,136,143,86,
220,255,62,60,255,55,111,0,100,0,0,232,156,117,2,0,0,0,16,192,179,98,106,2,
0,0,0,52,255,55,239,6,100,141,6,232,16,192,179,98,106,2,0,0,0,90,106,2,
0,0,0,13,0,224,9,0,50,128,4,235,8,132,251,71,68,222,8,64,120,150,118,2,
0,0,0,137,4,232,137,143,86,250,255,62,16,192,179,98,106,2,0,0,0,30,255,55,
64,202,179,128,79,84,30,0,50,4,0,62,0,202,179,128,79,84,66,1,58,7,0,55,
80,202,179,128,79,84,91,0,50,112,202,179,128,79,84,59,1,58,152,0,55,90,106,2,
0,0,0,128,64,233,226,207,80,4,0,54,0,64,235,98,106,2,0,0,0,6,255,55,
99,64,96,0,64,64,145,207,179,158,192,96,0,207,112,16,192,179,98,106,2,0,0,0,
253,254,55,158,64,120,102,192,96,226,79,80,6,0,50,228,79,80,4,0,50,0,64,235,
98,106,2,0,0,0,243,254,55,90,106,2,0,0,0,128,128,251,30,64,120,102,65,96,
64,0,32,226,79,81,1,0,58,48,0,32,1,128,185,128,4,120,9,0,224,9,0,50,
0,4,235,129,228,33,145,6,68,90,106,2,0,0,0,128,78,120,8,4,232,136,143,84,
248,255,62,30,65,120,102,64,97,226,79,80,4,0,50,0,4,235,9,0,224,18,0,58,
25,0,55,9,0,224,11,0,50,0,4,235,129,228,33,17,0,120,136,0,64,33,65,144,
145,64,144,96,64,124,145,253,7,99,4,68,136,143,84,246,255,62,16,192,179,98,106,2,
0,0,0,196,254,55,130,228,33,18,0,68,176,64,144,129,128,251,67,253,7,100,4,68,
136,143,84,248,255,62,16,192,179,98,106,2,0,0,0,184,254,55,158,64,120,102,192,96,
226,79,80,6,0,50,228,79,80,4,0,50,0,64,235,98,106,2,0,0,0,174,254,55,
90,106,2,0,0,0,0,68,120,30,65,120,102,64,97,226,79,80,7,0,50,8,132,251,
136,191,159,128,6,235,8,0,120,0,0,224,20,0,58,160,254,55,90,106,2,0,0,0,
128,70,120,136,132,251,155,254,50,0,4,235,65,0,32,13,64,120,43,253,7,74,119,2,
0,0,0,74,119,2,0,0,0,98,106,2,0,0,0,8,4,232,136,143,84,244,255,62,
141,254,55,24,46,41,73,46,41,33,0,32,8,0,120,12,253,7,45,253,7,33,0,32,
9,0,120,8,253,7,33,0,32,112,46,41,5,253,7,38,253,7,33,0,32,160,46,41,
1,253,7,34,253,7,141,6,232,143,184,151,129,143,86,237,255,57,119,254,55,90,106,2,
0,0,0,0,68,120,90,106,2,0,0,0,128,247,159,30,0,209,99,0,96,226,15,80,
5,0,50,227,15,80,10,0,50,225,15,80,15,0,58,7,0,55,8,64,120,82,192,179,
4,64,188,143,240,151,1,128,251,128,6,65,11,0,55,8,132,251,8,4,68,15,241,151,
2,128,251,128,6,68,141,134,70,4,0,55,0,64,235,98,106,2,0,0,0,86,254,55,
13,0,224,9,0,50,0,4,235,128,228,33,144,4,68,90,106,2,0,0,0,128,76,120,
8,4,232,141,15,84,248,255,57,143,240,151,1,4,224,3,0,50,16,192,179,98,106,2,
0,0,0,13,0,224,103,0,50,0,4,235,130,228,33,18,0,120,224,64,124,225,207,80,
3,0,50,226,207,80,92,0,58,49,0,55,158,0,209,227,128,96,226,143,80,33,0,50,
227,143,80,3,0,50,225,143,80,37,0,58,21,0,55,8,0,64,176,64,144,241,239,159,
32,65,144,226,239,159,144,68,144,97,0,32,9,64,120,188,252,7,9,4,224,8,0,53,
239,232,151,1,128,251,180,118,2,0,0,0,127,233,151,2,128,251,180,118,2,0,0,0,
100,4,68,62,0,55,8,0,64,48,65,144,160,64,144,16,64,144,221,252,7,100,4,68,
55,0,55,136,0,64,193,64,144,239,128,96,8,1,232,2,0,64,147,252,7,101,4,68,
47,0,55,0,64,235,98,106,2,0,0,0,9,254,55,158,0,209,227,128,96,226,143,80,
31,0,50,227,143,80,3,0,50,225,143,80,30,0,58,14,0,55,8,0,64,16,64,144,
97,0,32,144,252,7,74,119,2,0,0,0,98,106,2,0,0,0,74,119,2,0,0,0,
98,106,2,0,0,0,8,132,232,20,0,55,8,0,64,16,64,144,65,0,32,130,252,7,
74,119,2,0,0,0,74,119,2,0,0,0,98,106,2,0,0,0,8,132,232,8,0,55,
139,252,7,8,4,232,5,0,55,0,64,235,98,106,2,0,0,0,223,253,55,8,4,232,
141,15,84,154,255,57,15,240,151,0,4,224,217,253,58,16,192,179,98,106,2,0,0,0,
213,253,55,0,64,235,98,106,2,0,0,0,209,253,55,0,64,235,98,106,2,0,0,0,
205,253,55,8,64,120,128,253,7,16,192,179,98,106,2,0,0,0,199,253,55,99,68,100,
65,229,33,136,72,120,136,128,251,30,64,120,97,65,96,48,192,179,1,0,58,32,192,179,
72,117,2,0,0,0,16,192,179,120,118,2,0,0,0,16,192,179,98,106,2,0,0,0,
181,253,55,8,132,251,67,64,222,97,64,96,68,0,221,156,64,120,1,68,161,128,192,112,
66,64,222,97,64,96,30,65,120,2,4,161,0,65,113,2,79,120,8,0,209,97,64,96,
67,0,221,1,52,161,0,206,112,64,229,33,144,128,251,48,192,179,2,8,163,1,0,58,
32,192,179,72,117,2,0,0,0,16,192,179,120,118,2,0,0,0,16,192,179,98,106,2,
0,0,0,148,253,55,0,64,235,98,106,2,0,0,0,144,253,55,0,64,235,98,106,2,
0,0,0,140,253,55,79,7,120,79,6,190,79,5,190,79,4,190,143,0,177,0,0,6,
16,33,239,156,33,239,0,254,47,202,38,182,16,192,179,98,106,2,0,0,0,0,0,6,
33,227,168,211,251,7,33,227,169,0,0,6,33,227,168,207,251,7,174,105,2,0,0,0,
0,4,224,251,255,50,90,106,2,0,0,0,33,227,169,0,0,6,136,159,190,90,96,2,
0,0,0,0,4,190,72,8,222,98,106,2,0,0,0,64,72,222,1,0,32,98,106,2,
0,0,0,72,73,221,72,64,222,0,0,113,200,72,222,98,106,2,0,0,0,8,64,120,
98,106,2,0,0,0,79,4,190,0,0,6,136,31,120,16,33,239,30,33,239,156,33,239,
0,254,47,202,6,182,96,1,179,80,54,136,90,106,2,0,0,0,99,0,96,68,0,221,
241,252,47,130,8,128,130,128,96,129,0,112,129,8,136,90,106,2,0,0,0,72,4,221,
90,106,2,0,0,0,0,128,251,8,4,112,216,12,136,200,12,136,96,0,32,224,12,136,
90,106,2,0,0,0,72,4,221,90,106,2,0,0,0,0,128,251,8,0,112,96,8,136,
17,225,168,16,192,179,98,106,2,0,0,0,79,4,120,0,0,6,2,0,250,136,159,190,
192,34,169,194,34,168,219,252,7,142,251,7,8,14,32,9,4,32,90,106,2,0,0,0,
0,4,224,121,0,53,160,255,159,234,79,80,4,0,58,102,128,87,64,135,2,0,0,0,
245,255,55,0,129,251,128,1,235,248,15,81,224,143,89,98,0,62,2,96,1,24,0,55,
25,0,55,30,0,55,35,0,55,40,0,55,45,0,55,49,0,55,54,0,55,88,0,55,
87,0,55,86,0,55,85,0,55,84,0,55,83,0,55,82,0,55,52,0,55,60,0,55,
62,0,55,64,0,55,65,0,55,66,0,55,67,0,55,68,0,55,73,0,55,68,0,55,
97,251,7,212,255,55,171,252,7,172,73,2,0,0,0,168,252,7,91,251,7,206,255,55,
165,252,7,14,89,2,0,0,0,162,252,7,85,251,7,200,255,55,159,252,7,18,81,2,
0,0,0,156,252,7,79,251,7,194,255,55,153,252,7,102,103,2,0,0,0,150,252,7,
73,251,7,188,255,55,147,252,7,188,252,7,145,252,7,68,251,7,183,255,55,142,252,7,
62,130,2,0,0,0,139,252,7,62,251,7,177,255,55,136,252,7,28,113,2,0,0,0,
133,252,7,56,251,7,171,255,55,16,192,179,98,106,2,0,0,0,192,34,169,194,34,169,
164,109,2,0,0,0,0,0,254,162,255,55,0,64,235,183,251,7,159,255,55,16,192,179,
180,251,7,156,255,55,105,255,7,154,255,55,59,255,7,152,255,55,65,255,7,150,255,55,
67,255,7,148,255,55,75,255,7,146,255,55,0,64,235,98,106,2,0,0,0,142,255,55,
128,0,100,137,143,80,4,0,58,218,251,7,98,106,2,0,0,0,135,255,55,0,64,235,
98,106,2,0,0,0,131,255,55,15,252,7,98,106,2,0,0,0,127,255,55,200,194,168,
201,2,168,200,226,168,201,34,168,0,254,47,82,53,128,130,0,96,97,0,179,81,53,136,
115,53,128,131,0,96,97,0,179,113,53,136,66,53,128,130,0,96,145,0,179,65,53,136,
241,15,46,67,53,128,3,129,96,2,176,160,66,53,136,172,6,182,112,0,179,96,53,136,
96,53,128,128,128,96,1,176,160,97,53,136,209,243,128,71,8,221,0,16,178,225,128,96,
198,8,221,0,128,112,65,229,33,145,128,251,242,46,41,98,193,120,127,1,97,0,0,113,
16,18,136,68,34,239,64,34,239,16,19,136,100,34,239,96,34,239,72,34,239,104,34,239,
67,98,168,99,98,168,67,34,169,99,34,169,64,194,169,96,194,169,68,2,168,100,2,168,
0,0,6,65,226,169,97,226,169,67,98,169,99,98,169,240,1,32,170,38,183,174,38,183,
168,38,183,2,240,33,2,0,120,168,38,183,240,1,32,172,38,183,2,0,120,172,38,183,
0,0,6,97,229,33,97,64,96,70,0,221,17,65,120,2,100,161,128,72,113,160,231,33,
16,64,120,104,64,96,128,128,251,129,0,233,207,8,222,198,8,221,82,22,128,2,96,161,
2,129,112,82,22,136,0,4,224,3,0,50,240,194,179,98,106,2,0,0,0,160,198,41,
1,0,32,38,137,2,0,0,0,0,0,6,16,192,179,227,255,7,0,0,6,0,64,235,
224,255,7,0,0,6,160,231,33,144,0,120,195,8,221,1,4,178,82,22,128,2,96,161,
2,129,112,82,22,136,16,64,120,104,64,96,3,0,50,240,194,179,98,106,2,0,0,0,
32,198,41,1,0,32,38,137,2,0,0,0,0,0,6,136,31,120,64,93,41,1,0,32,
204,136,2,0,0,0,64,229,33,16,64,232,62,107,2,0,0,0,0,194,179,98,106,2,
0,0,0,168,231,33,152,64,120,97,192,96,62,107,2,0,0,0,0,194,179,98,106,2,
0,0,0,24,0,209,97,64,96,62,107,2,0,0,0,0,194,179,98,106,2,0,0,0,
24,0,120,66,0,222,97,64,96,62,107,2,0,0,0,0,194,179,98,106,2,0,0,0,
24,0,120,67,0,222,97,64,96,62,107,2,0,0,0,0,194,179,98,106,2,0,0,0,
96,229,33,16,128,251,68,0,222,97,64,96,62,107,2,0,0,0,64,227,41,1,0,32,
38,137,2,0,0,0,79,4,120,0,0,6,136,159,190,138,159,190,140,31,120,36,28,2,
0,0,0,158,26,2,0,0,0,0,4,120,36,28,2,0,0,0,158,26,2,0,0,0,
128,4,120,36,28,2,0,0,0,158,26,2,0,0,0,0,5,120,36,28,2,0,0,0,
158,26,2,0,0,0,128,5,120,36,28,2,0,0,0,158,26,2,0,0,0,0,6,120,
36,28,2,0,0,0,158,26,2,0,0,0,136,0,233,227,143,80,15,0,62,137,0,233,
225,143,80,12,0,62,138,0,233,225,143,80,9,0,62,139,0,233,225,143,80,6,0,62,
140,0,233,225,143,80,3,0,62,128,0,233,225,143,80,94,0,54,105,229,33,25,116,161,
0,212,40,1,0,32,38,137,2,0,0,0,128,1,235,194,0,32,17,0,32,1,0,120,
150,28,2,0,0,0,65,229,33,128,72,233,160,25,41,1,0,32,38,137,2,0,0,0,
128,1,235,34,0,32,17,0,32,1,0,120,150,28,2,0,0,0,168,231,33,0,64,233,
97,64,96,152,64,120,1,4,161,0,204,112,160,248,40,1,0,32,38,137,2,0,0,0,
128,1,235,34,0,32,17,0,32,2,0,120,150,28,2,0,0,0,0,64,233,97,64,96,
0,64,64,152,64,120,1,20,161,0,204,112,224,27,41,1,0,32,38,137,2,0,0,0,
128,1,235,34,0,32,17,0,32,1,0,120,150,28,2,0,0,0,0,64,233,97,64,96,
66,0,221,152,64,120,1,36,161,0,204,112,96,112,41,1,0,32,38,137,2,0,0,0,
128,1,235,34,0,32,17,0,32,2,0,120,150,28,2,0,0,0,0,64,233,97,64,96,
67,0,221,152,64,120,1,52,161,0,204,112,0,239,40,1,0,32,38,137,2,0,0,0,
128,1,235,34,0,32,17,0,32,1,0,120,150,28,2,0,0,0,97,64,96,68,0,221,
153,64,120,1,68,161,128,204,112,31,0,55,65,229,33,136,72,233,161,231,33,137,68,233,
225,196,100,17,65,120,2,4,161,137,68,113,10,69,233,97,69,101,10,65,69,9,69,120,
10,20,161,2,69,117,139,69,233,225,197,101,194,93,221,10,36,161,11,69,117,12,70,233,
97,70,102,67,102,221,10,52,161,140,72,117,97,229,33,97,64,96,68,0,221,17,65,120,
2,68,161,128,72,113,26,255,7,96,229,33,16,100,161,79,6,120,79,5,190,79,4,190,
0,0,6,224,165,41,1,0,32,38,137,2,0,0,0,0,0,6,65,226,169,97,229,33,
145,64,120,240,192,96,4,0,50,207,34,168,207,2,168,206,194,168,3,0,55,207,34,169,
207,2,169,206,194,169,1,254,47,67,53,128,3,129,96,114,0,179,66,53,136,243,15,46,
66,54,128,130,129,97,2,112,32,3,1,113,66,54,136,67,54,128,131,128,96,1,48,160,
65,54,136,202,194,168,200,194,169,201,2,169,200,226,168,201,34,169,209,243,128,225,129,96,
198,25,221,71,9,221,2,16,178,3,1,113,2,80,160,199,8,221,1,32,178,130,128,112,
127,0,96,1,0,112,16,18,136,68,34,239,0,0,40,0,18,136,0,0,6,64,229,33,
16,128,251,241,46,41,97,64,120,202,255,7,208,243,128,67,0,221,0,4,178,81,22,128,
1,96,161,129,0,112,81,22,136,0,0,6,65,226,169,240,1,32,168,38,183,240,15,46,
200,38,182,0,254,47,200,38,182,207,34,169,207,2,169,206,194,169,0,0,6,228,135,71,
136,159,190,138,159,190,140,159,190,142,31,120,128,247,159,129,70,120,44,229,33,40,165,41,
9,0,32,180,105,2,0,0,0,232,255,7,58,254,7,15,240,151,0,4,224,2,0,58,
66,226,168,98,226,168,65,226,168,97,226,168,27,192,179,10,2,32,8,0,55,11,4,224,
6,0,58,202,194,171,4,0,50,208,197,179,22,106,2,0,0,0,27,192,179,1,18,128,
1,0,101,35,0,58,1,19,128,1,0,101,32,0,58,78,18,128,11,4,224,4,0,50,
176,197,179,22,106,2,0,0,0,128,69,235,13,4,224,14,0,50,14,64,120,58,106,2,
0,0,0,78,19,128,128,194,179,22,106,2,0,0,0,14,64,120,58,106,2,0,0,0,
144,194,179,22,106,2,0,0,0,9,0,55,192,197,179,22,106,2,0,0,0,14,64,120,
22,106,2,0,0,0,64,19,128,22,106,2,0,0,0,64,194,171,6,0,58,96,194,171,
4,0,58,156,0,120,98,128,96,16,0,50,5,0,55,156,0,120,98,128,96,2,0,58,
228,105,2,0,0,0,64,34,239,96,34,239,13,4,224,4,0,50,8,0,190,38,137,2,
0,0,0,175,255,55,194,34,169,13,0,55,194,105,2,0,0,0,174,105,2,0,0,0,
0,4,224,181,255,50,90,106,2,0,0,0,13,4,224,3,0,50,224,46,41,182,108,2,
0,0,0,23,254,7,64,229,33,16,128,251,241,46,41,97,64,120,71,255,7,79,7,120,
79,6,190,79,5,190,79,4,190,79,0,177,0,0,6,235,15,80,49,0,50,8,0,62,
225,15,80,19,0,50,13,0,57,226,15,80,28,0,50,234,15,80,76,0,58,37,0,55,
237,15,80,44,0,50,40,0,57,238,15,80,44,0,50,239,15,80,68,0,58,44,0,55,
224,187,40,1,0,32,38,137,2,0,0,0,66,0,55,96,213,41,1,0,32,38,137,2,
0,0,0,224,159,41,1,0,32,38,137,2,0,0,0,17,192,179,0,64,235,103,255,7,
54,0,55,96,213,41,1,0,32,38,137,2,0,0,0,224,159,41,1,0,32,38,137,2,
0,0,0,17,192,179,1,64,120,91,255,7,42,0,55,160,231,33,16,4,161,14,0,55,
160,231,33,16,4,160,11,0,55,160,231,33,16,20,161,8,0,55,160,231,33,16,20,160,
5,0,55,160,231,33,16,36,161,2,0,55,160,231,33,16,36,160,208,243,128,225,0,96,
198,8,221,18,18,128,2,96,161,2,129,112,18,18,136,199,0,221,1,16,178,18,18,128,
2,128,161,2,129,112,18,18,136,71,0,221,0,32,178,17,18,128,1,144,161,129,0,112,
17,18,136,251,253,7,4,0,55,224,63,41,1,0,32,38,137,2,0,0,0,0,0,6,
35,255,7,0,0,6,0,128,251,64,18,136,1,64,32,34,4,128,2,128,96,253,255,50,
64,18,128,133,64,169,0,0,6,136,159,190,138,159,190,128,4,120,9,0,224,16,0,50,
0,4,235,139,228,33,27,5,68,90,106,2,0,0,0,0,77,120,8,4,232,136,143,84,
249,255,62,0,4,235,64,242,128,96,64,124,229,255,7,8,4,232,136,143,84,250,255,62,
79,5,190,79,4,190,0,0,6,136,159,190,138,159,190,140,31,120,128,4,120,9,0,224,
22,0,50,0,4,235,139,228,33,0,198,235,27,5,68,12,64,120,211,255,7,0,77,120,
8,4,232,136,143,84,249,255,62,16,192,179,98,106,2,0,0,0,0,4,235,64,242,128,
96,64,124,98,106,2,0,0,0,8,4,232,136,143,84,249,255,62,3,0,55,16,192,179,
98,106,2,0,0,0,79,6,120,79,5,190,79,4,190,0,0,6,136,159,190,138,31,120,
0,69,120,222,109,2,0,0,0,0,4,120,222,109,2,0,0,0,128,4,120,0,0,33,
128,15,84,4,0,54,0,64,235,98,106,2,0,0,0,36,0,55,0,0,33,128,143,84,
4,0,54,0,64,235,98,106,2,0,0,0,29,0,55,8,0,224,6,0,58,9,0,224,
4,0,58,16,192,179,98,106,2,0,0,0,21,0,55,10,4,224,1,0,50,202,194,169,
8,0,224,5,0,50,8,0,120,159,255,7,16,0,32,104,105,2,0,0,0,9,0,224,
3,0,50,9,0,120,176,255,7,3,0,55,16,192,179,98,106,2,0,0,0,10,4,224,
1,0,50,202,194,168,79,5,120,79,4,190,0,0,6,136,159,190,138,159,190,140,159,190,
142,31,120,16,192,179,98,106,2,0,0,0,90,106,2,0,0,0,225,79,80,8,0,50,
3,0,57,226,79,80,78,0,58,14,0,55,16,192,179,98,106,2,0,0,0,76,0,55,
16,192,179,98,106,2,0,0,0,0,64,235,98,106,2,0,0,0,16,192,179,98,106,2,
0,0,0,66,0,55,240,109,2,0,0,0,0,5,190,240,109,2,0,0,0,0,4,190,
225,15,69,224,143,93,8,0,62,225,15,64,224,143,88,5,0,62,0,0,69,129,128,77,
225,15,64,224,143,88,4,0,54,0,64,235,98,106,2,0,0,0,45,0,55,16,192,179,
98,106,2,0,0,0,224,15,84,224,143,92,39,0,50,12,194,179,12,64,120,74,255,7,
72,89,221,72,80,222,0,0,113,200,88,222,128,70,120,68,255,7,10,64,120,66,255,7,
0,64,235,64,255,7,98,106,2,0,0,0,16,0,32,1,0,32,128,15,84,129,143,92,
19,0,50,128,194,179,55,255,7,13,64,120,53,255,7,10,64,120,51,255,7,0,64,235,
49,255,7,98,106,2,0,0,0,98,4,84,224,132,92,6,0,50,97,5,69,224,133,77,
221,255,55,0,64,235,98,106,2,0,0,0,79,7,120,79,6,190,79,5,190,79,4,190,
0,0,6,32,255,7,0,65,120,97,229,33,0,196,179,145,64,96,0,0,235,1,0,50,
2,128,251,0,0,6,0,192,235,22,255,7,0,128,251,0,0,6,228,135,71,136,159,190,
138,159,190,140,159,190,142,31,120,17,192,179,64,229,33,1,72,120,160,231,33,144,64,120,
1,4,161,1,20,160,1,36,161,1,72,120,96,229,33,16,68,160,128,193,179,230,253,7,
128,224,41,1,0,32,204,136,2,0,0,0,78,229,33,172,231,33,109,229,33,136,224,41,
9,0,32,90,106,2,0,0,0,0,69,120,138,247,159,10,128,251,68,0,222,226,79,69,
4,0,58,112,128,87,64,135,2,0,0,0,244,255,55,228,79,80,92,0,50,5,0,62,
0,4,224,8,0,50,225,79,80,141,0,58,71,0,55,230,79,80,91,0,50,232,79,80,
136,0,58,104,0,55,229,79,85,10,0,62,228,79,85,50,0,49,225,79,85,20,0,50,
12,0,57,226,79,85,21,0,50,227,79,85,51,0,58,23,0,55,237,79,85,26,0,50,
238,79,85,31,0,50,230,79,85,44,0,58,41,0,55,238,253,7,79,7,120,79,6,190,
79,5,190,79,4,190,79,0,177,0,0,6,8,0,190,204,136,2,0,0,0,201,255,55,
204,194,169,16,192,179,98,106,2,0,0,0,196,255,55,204,194,168,16,192,179,98,106,2,
0,0,0,191,255,55,16,192,179,98,106,2,0,0,0,128,64,235,16,192,179,223,253,7,
184,255,55,16,192,179,98,106,2,0,0,0,128,64,235,1,64,120,216,253,7,177,255,55,
10,36,162,10,128,251,0,0,233,79,0,222,233,254,7,171,255,55,30,255,7,169,255,55,
0,64,235,98,106,2,0,0,0,165,255,55,16,192,179,98,106,2,0,0,0,111,5,101,
138,5,232,0,5,235,90,106,2,0,0,0,149,254,7,98,106,2,0,0,0,10,5,232,
139,15,85,248,255,57,150,255,55,10,64,120,118,59,2,0,0,0,16,192,179,98,106,2,
0,0,0,143,255,55,111,69,101,236,79,85,4,0,54,0,64,235,98,106,2,0,0,0,
136,255,55,10,79,120,10,133,251,241,46,41,97,64,125,96,253,7,16,192,179,98,106,2,
0,0,0,127,255,55,10,128,251,194,0,222,225,192,96,156,65,120,3,4,161,129,193,113,
128,0,209,225,192,96,1,193,64,131,64,120,1,20,161,130,192,112,97,69,101,66,85,221,
1,36,161,10,206,112,104,0,96,0,0,233,79,0,222,68,0,221,157,64,120,1,68,161,
128,206,112,30,128,251,241,46,41,97,64,120,65,253,7,16,192,179,98,106,2,0,0,0,
96,255,55,0,64,235,98,106,2,0,0,0,92,255,55,0,3,190,176,18,32,128,15,83,
224,143,91,7,0,54,132,38,41,33,0,32,84,1,190,134,15,81,135,143,89,19,0,54,
3,0,55,194,18,32,3,0,32,17,0,32,129,0,233,194,8,221,64,38,41,1,0,64,
16,0,190,0,2,81,129,130,89,133,2,209,4,130,211,4,2,64,133,130,72,132,15,83,
133,143,91,6,0,54,6,0,55,129,0,232,239,143,80,230,255,58,96,17,184,1,0,55,
0,1,190,2,0,190,0,0,6,2,0,250,136,159,190,138,159,190,140,159,190,0,69,120,
202,226,169,200,226,168,10,4,224,4,0,58,64,149,41,1,0,32,38,137,2,0,0,0,
202,226,171,9,0,50,34,2,171,7,0,58,1,8,32,0,0,0,82,22,128,2,128,96,
2,0,50,34,2,171,250,255,50,19,0,32,96,102,184,5,8,32,0,2,235,34,2,171,
19,0,50,48,17,128,144,191,159,22,33,239,128,0,32,240,8,136,20,33,239,8,33,239,
128,8,136,6,33,239,96,68,184,10,4,224,87,0,58,96,188,41,1,0,32,38,137,2,
0,0,0,96,68,184,81,0,55,131,1,232,82,22,128,130,128,98,253,255,50,31,225,168,
17,225,168,80,22,128,128,128,98,253,255,58,17,225,169,31,225,169,240,1,32,134,38,183,
244,8,136,132,8,136,72,8,128,54,8,128,128,4,235,192,68,221,8,0,32,128,3,235,
6,3,116,135,131,116,225,143,81,231,255,50,224,15,86,224,143,94,5,0,50,134,15,86,
135,143,94,3,0,54,6,6,190,1,0,55,6,6,190,248,143,81,220,255,54,240,1,32,
134,38,183,6,33,239,8,33,239,12,1,190,0,64,34,65,15,32,6,6,2,0,0,0,
0,4,190,10,4,224,33,0,58,0,159,36,33,0,32,128,15,84,129,143,92,5,0,54,
192,18,41,1,0,32,38,137,2,0,0,0,23,0,55,96,167,41,1,0,32,204,136,2,
0,0,0,8,0,190,88,107,2,0,0,0,202,224,41,11,0,32,10,0,190,38,137,2,
0,0,0,160,166,41,1,0,32,204,136,2,0,0,0,8,0,190,94,255,7,88,107,2,
0,0,0,10,0,190,38,137,2,0,0,0,8,0,190,79,6,190,79,5,190,79,4,190,
0,128,250,0,0,6,136,31,120,58,141,2,0,0,0,0,4,224,23,0,50,50,98,171,
4,0,50,128,225,41,1,0,32,204,136,2,0,0,0,50,66,171,4,0,50,0,225,41,
1,0,32,204,136,2,0,0,0,50,141,2,0,0,0,0,132,251,50,34,171,11,0,50,
224,168,41,1,0,32,204,136,2,0,0,0,50,34,169,5,0,55,32,168,41,1,0,32,
204,136,2,0,0,0,0,4,235,8,0,120,79,4,120,0,0,6,36,141,2,0,0,0,
0,0,235,0,0,6,136,31,120,160,94,41,1,0,32,204,136,2,0,0,0,72,229,33,
24,64,120,62,107,2,0,0,0,0,194,179,98,106,2,0,0,0,24,64,120,233,79,80,
5,0,50,0,128,251,0,0,64,1,37,41,97,0,120,1,0,55,192,17,128,76,107,2,
0,0,0,0,194,179,98,106,2,0,0,0,200,231,33,152,64,120,99,192,96,62,107,2,
0,0,0,0,194,179,98,106,2,0,0,0,24,0,120,66,0,222,97,64,96,62,107,2,
0,0,0,0,194,179,98,106,2,0,0,0,24,0,120,67,0,222,97,64,96,62,107,2,
0,0,0,0,194,179,98,106,2,0,0,0,96,229,33,16,128,251,68,0,222,97,64,96,
62,107,2,0,0,0,64,227,41,1,0,32,38,137,2,0,0,0,79,4,120,0,0,6,
136,159,190,138,159,190,140,31,120,36,28,2,0,0,0,158,26,2,0,0,0,0,4,120,
234,15,84,25,0,58,36,28,2,0,0,0,158,26,2,0,0,0,0,6,120,36,28,2,
0,0,0,158,26,2,0,0,0,128,4,120,36,28,2,0,0,0,158,26,2,0,0,0,
0,5,120,36,28,2,0,0,0,158,26,2,0,0,0,128,5,120,36,28,2,0,0,0,
158,26,2,0,0,0,23,0,55,36,28,2,0,0,0,158,26,2,0,0,0,128,4,120,
36,28,2,0,0,0,158,26,2,0,0,0,0,5,120,36,28,2,0,0,0,158,26,2,
0,0,0,128,5,120,36,28,2,0,0,0,158,26,2,0,0,0,136,0,233,233,143,80,
4,0,62,0,6,235,65,229,33,136,72,233,2,0,55,0,6,235,12,4,120,137,0,233,
227,143,80,7,0,62,193,231,33,137,68,233,227,196,100,194,207,179,17,65,97,137,72,113,
1,0,55,0,4,235,138,0,233,225,143,80,8,0,62,193,231,33,10,69,233,97,69,101,
66,85,221,17,65,120,2,36,161,138,72,113,1,0,55,0,4,235,139,0,233,225,143,80,
8,0,62,193,231,33,139,69,233,225,197,101,195,93,221,17,65,120,2,52,161,139,72,113,
1,0,55,0,4,235,128,0,233,225,143,80,8,0,62,97,229,33,97,64,96,68,0,221,
17,65,120,2,68,161,128,72,113,8,0,224,93,0,58,96,229,33,16,116,161,224,224,40,
1,0,32,38,137,2,0,0,0,128,1,235,162,0,32,17,0,32,1,0,120,150,28,2,
0,0,0,0,64,233,65,229,33,128,72,120,233,79,80,11,0,58,224,114,41,1,0,32,
38,137,2,0,0,0,128,1,235,242,255,39,17,0,32,32,2,32,150,28,2,0,0,0,
192,17,136,96,235,40,1,0,32,204,136,2,0,0,0,128,1,235,66,0,32,17,0,32,
1,0,120,150,28,2,0,0,0,200,231,33,0,64,233,99,64,96,193,207,179,152,192,96,
0,204,112,0,82,41,1,0,32,204,136,2,0,0,0,128,1,235,34,0,32,17,0,32,
1,0,120,150,28,2,0,0,0,0,64,233,97,64,96,66,0,221,152,64,120,1,36,161,
0,204,112,0,30,41,1,0,32,204,136,2,0,0,0,128,1,235,34,0,32,17,0,32,
1,0,120,150,28,2,0,0,0,0,64,233,97,64,96,67,0,221,152,64,120,1,52,161,
0,204,112,0,239,40,1,0,32,38,137,2,0,0,0,128,1,235,34,0,32,17,0,32,
1,0,120,150,28,2,0,0,0,97,229,33,97,64,96,68,0,221,17,65,120,2,68,161,
128,72,113,13,0,55,64,229,33,145,192,179,144,207,16,8,0,58,97,97,185,0,144,32,
209,3,32,6,6,2,0,0,0,97,0,80,224,128,88,192,17,136,235,254,7,79,6,120,
79,5,190,79,4,190,0,0,6,226,243,128,77,18,221,79,34,222,227,65,97,76,17,221,
79,17,222,96,229,33,144,128,251,203,8,221,207,8,222,64,229,33,16,64,120,233,79,80,
5,0,50,0,128,251,0,0,64,5,37,41,101,0,120,1,0,55,192,17,128,204,140,2,
0,0,0,192,231,33,16,64,120,99,64,96,227,79,80,3,0,58,145,192,179,80,229,33,
1,72,120,8,141,2,0,0,0,192,17,128,40,2,227,4,0,54,128,83,41,1,0,32,
38,137,2,0,0,0,0,0,6,18,141,2,0,0,0,0,0,6,2,0,250,136,159,190,
0,4,120,226,15,84,76,0,50,5,0,62,8,0,224,8,0,50,225,15,84,111,0,58,
14,0,55,227,15,84,8,0,50,228,15,84,106,0,58,91,0,55,224,230,40,1,0,32,
204,136,2,0,0,0,104,0,55,192,98,169,192,130,168,200,194,168,201,2,169,32,199,41,
1,0,32,38,137,2,0,0,0,192,187,41,1,0,32,38,137,2,0,0,0,194,137,2,
0,0,0,0,4,224,88,0,50,50,34,169,9,32,32,50,2,171,5,0,50,17,17,128,
1,128,100,2,0,58,176,17,128,32,17,136,174,105,2,0,0,0,0,4,224,7,0,50,
145,17,128,1,128,100,4,0,58,90,106,2,0,0,0,0,128,251,160,17,136,50,34,171,
2,0,58,34,34,171,3,0,50,50,34,169,34,34,169,194,34,169,227,15,84,229,255,58,
16,22,128,68,0,221,0,16,178,81,22,128,1,128,161,129,0,112,81,22,136,80,22,128,
67,0,222,104,0,96,17,22,128,1,48,161,129,0,112,17,22,136,214,255,55,0,189,41,
1,0,32,38,137,2,0,0,0,224,159,41,1,0,32,38,137,2,0,0,0,50,34,169,
8,32,32,50,2,171,5,0,50,17,17,128,1,0,100,2,0,58,176,17,128,32,17,136,
174,105,2,0,0,0,0,4,224,245,255,50,90,106,2,0,0,0,0,128,251,208,191,159,
18,0,55,18,141,2,0,0,0,0,64,235,158,253,7,8,141,2,0,0,0,192,17,128,
40,2,227,9,0,54,128,83,41,1,0,32,38,137,2,0,0,0,4,0,55,224,63,41,
1,0,32,38,137,2,0,0,0,79,4,190,0,128,250,0,0,6,50,34,169,192,231,33,
16,68,160,96,229,33,16,36,160,160,113,41,1,0,32,38,137,2,0,0,0,0,0,6,
192,231,33,16,68,161,96,229,33,16,36,161,96,148,41,1,0,32,38,137,2,0,0,0,
0,0,6,230,135,71,136,159,190,138,159,190,140,159,190,142,31,120,0,71,235,8,213,41,
9,0,32,234,168,41,11,0,32,12,225,41,13,0,32,128,225,41,1,0,32,240,183,159,
129,191,159,39,0,55,193,231,33,145,64,120,112,192,96,3,0,58,50,141,2,0,0,0,
31,0,55,224,36,41,182,108,2,0,0,0,8,0,190,204,136,2,0,0,0,50,98,171,
4,0,50,127,176,151,143,184,151,204,136,2,0,0,0,50,66,171,3,0,50,12,0,190,
204,136,2,0,0,0,50,141,2,0,0,0,0,128,251,58,109,2,0,0,0,50,34,171,
4,0,50,50,34,169,10,0,190,204,136,2,0,0,0,224,36,41,182,108,2,0,0,0,
30,192,179,58,141,2,0,0,0,0,4,224,213,255,58,14,64,120,79,7,120,79,6,190,
79,5,190,79,4,190,111,0,177,0,0,6,160,199,41,1,0,32,38,137,2,0,0,0,
0,0,6,228,135,71,136,159,190,138,159,190,140,159,190,142,31,120,192,231,33,2,207,179,
16,65,97,97,229,33,17,68,160,130,64,120,1,68,161,1,72,120,0,66,235,132,65,120,
4,65,120,17,192,179,64,65,35,204,140,2,0,0,0,8,141,2,0,0,0,64,225,41,
1,0,32,204,136,2,0,0,0,78,65,35,202,231,33,109,229,33,72,225,41,9,0,32,
58,141,2,0,0,0,0,4,224,10,0,50,154,64,120,112,192,96,5,0,50,50,141,2,
0,0,0,98,106,2,0,0,0,2,0,55,50,141,2,0,0,0,50,34,169,174,105,2,
0,0,0,0,4,224,237,255,50,90,106,2,0,0,0,128,247,159,226,79,64,4,0,58,
112,128,87,64,135,2,0,0,0,228,255,55,0,129,251,3,15,32,131,0,97,3,4,32,
131,143,80,139,0,50,5,0,60,1,0,224,13,0,50,240,143,80,217,0,58,109,0,55,
3,8,32,131,143,80,167,0,50,3,9,32,131,143,80,164,0,50,2,6,32,130,143,80,
207,0,58,129,0,55,226,79,80,25,0,50,5,0,62,0,4,224,10,0,50,225,79,80,
88,0,58,15,0,55,231,79,80,28,0,50,239,79,80,59,0,50,227,79,80,81,0,58,
18,0,55,18,141,2,0,0,0,79,7,120,79,6,190,79,5,190,79,4,190,79,0,177,
0,0,6,8,0,190,204,136,2,0,0,0,179,255,55,16,192,179,98,106,2,0,0,0,
50,34,169,26,68,160,173,255,55,26,68,161,16,192,179,98,106,2,0,0,0,168,255,55,
16,192,179,98,106,2,0,0,0,18,141,2,0,0,0,90,106,2,0,0,0,200,5,221,
16,192,179,98,106,2,0,0,0,90,106,2,0,0,0,0,135,251,11,7,119,154,1,120,
77,26,221,76,25,221,79,17,222,157,128,251,203,8,221,207,8,222,79,34,222,227,193,97,
14,0,120,204,140,2,0,0,0,8,141,2,0,0,0,16,192,179,98,106,2,0,0,0,
135,255,55,16,192,179,98,106,2,0,0,0,50,34,169,8,32,32,50,2,171,5,0,50,
17,17,128,1,0,100,2,0,58,179,17,128,35,17,136,174,105,2,0,0,0,0,4,224,
245,255,50,145,17,128,1,0,100,242,255,58,90,106,2,0,0,0,0,128,251,160,17,136,
237,255,55,0,64,235,98,106,2,0,0,0,107,255,55,111,64,96,0,64,232,128,247,159,
16,192,179,98,106,2,0,0,0,143,241,151,3,4,224,98,255,50,128,5,235,28,192,179,
90,106,2,0,0,0,36,141,2,0,0,0,12,64,120,98,106,2,0,0,0,139,5,232,
143,240,151,1,128,251,139,15,80,244,255,62,83,255,55,118,59,2,0,0,0,16,192,179,
98,106,2,0,0,0,77,255,55,111,64,96,128,247,159,234,79,80,2,0,54,160,192,179,
128,247,159,143,241,151,3,128,251,0,0,64,1,37,41,97,7,120,18,141,2,0,0,0,
154,1,120,77,26,221,76,25,221,79,17,222,157,128,251,203,8,221,207,8,222,79,34,222,
227,193,97,14,0,120,204,140,2,0,0,0,8,141,2,0,0,0,16,192,179,98,106,2,
0,0,0,46,255,55,0,207,179,26,77,96,29,68,161,2,56,163,4,0,50,192,207,179,
26,64,96,0,20,160,0,77,120,2,40,163,6,0,50,154,65,120,99,192,97,0,4,160,
193,207,179,129,192,97,0,205,112,2,24,163,1,0,50,26,36,160,2,8,163,1,0,50,
26,52,160,2,72,163,1,0,58,29,68,160,18,141,2,0,0,0,154,1,120,77,26,221,
76,25,221,79,17,222,157,128,251,203,8,221,207,8,222,79,34,222,227,193,97,14,0,120,
204,140,2,0,0,0,8,141,2,0,0,0,16,192,179,98,106,2,0,0,0,0,255,55,
0,64,235,98,106,2,0,0,0,252,254,55,6,2,168,6,2,171,254,255,58,0,0,6,
6,66,168,6,66,171,254,255,58,0,0,6,64,16,128,79,0,222,0,0,6,97,0,96,
69,0,221,49,16,128,1,80,161,129,0,112,49,16,136,6,130,168,6,130,171,254,255,58,
0,0,6,0,128,251,16,16,136,1,0,36,66,16,128,2,128,96,253,255,58,0,0,6,
6,98,168,6,98,171,254,255,58,0,16,128,0,0,6,33,230,33,145,64,120,225,192,96,
3,0,58,150,118,2,0,0,0,1,0,55,226,255,7,32,230,33,16,20,161,0,0,6,
136,159,190,138,159,190,140,159,190,222,109,2,0,0,0,128,4,120,222,109,2,0,0,0,
0,6,120,0,64,235,1,0,33,129,143,84,75,0,62,0,4,235,139,228,33,9,0,224,
3,0,58,12,0,224,73,0,58,68,0,55,27,5,68,90,106,2,0,0,0,0,77,120,
8,4,232,136,143,84,249,255,62,64,242,128,16,69,120,0,0,235,234,117,2,0,0,0,
0,4,235,64,242,128,96,64,124,0,128,251,180,118,2,0,0,0,36,119,2,0,0,0,
0,4,224,45,0,58,8,4,232,136,143,84,244,255,62,12,0,224,29,0,50,225,143,84,
49,0,54,16,0,32,234,117,2,0,0,0,10,4,160,10,128,251,180,118,2,0,0,0,
36,119,2,0,0,0,128,64,120,0,64,235,1,4,224,36,0,50,25,0,55,155,4,68,
74,119,2,0,0,0,128,76,120,13,64,120,138,15,84,1,0,49,0,64,235,150,118,2,
0,0,0,8,4,232,136,15,86,243,255,62,202,117,2,0,0,0,16,192,179,98,106,2,
0,0,0,140,0,120,64,242,128,28,109,2,0,0,0,16,192,179,1,0,55,0,64,235,
79,6,190,79,5,190,79,4,190,0,0,6,64,242,128,16,69,120,0,0,235,234,117,2,
0,0,0,205,255,55,12,5,233,0,4,235,139,228,33,29,192,179,216,255,55,230,135,71,
136,159,190,138,159,190,140,159,190,142,31,120,0,71,120,180,105,2,0,0,0,201,34,168,
201,2,168,203,2,169,203,34,169,98,160,168,98,192,168,134,96,169,91,22,128,198,93,221,
207,93,222,88,22,128,71,68,221,79,68,222,0,64,235,240,239,159,128,247,159,0,70,120,
29,192,179,237,239,159,11,0,55,9,68,120,138,69,120,8,0,55,9,68,120,138,69,120,
5,0,55,138,69,120,13,68,120,2,0,55,138,69,120,13,68,120,134,96,171,13,0,58,
194,105,2,0,0,0,174,105,2,0,0,0,0,4,224,248,255,50,90,106,2,0,0,0,
98,160,169,98,192,169,14,4,224,80,0,50,76,0,55,134,96,169,90,22,128,70,85,221,
79,85,222,89,22,128,199,76,221,207,76,222,12,4,224,39,0,50,8,4,224,39,0,58,
9,4,224,35,0,50,15,241,151,231,79,81,9,0,62,255,232,151,1,192,64,10,64,112,
240,239,159,2,65,232,130,247,159,138,69,120,13,68,120,216,255,55,14,4,224,4,0,50,
127,232,151,58,106,2,0,0,0,6,0,55,192,197,179,22,106,2,0,0,0,127,232,151,
22,106,2,0,0,0,176,194,179,10,4,224,1,0,50,208,194,179,22,106,2,0,0,0,
9,68,120,138,69,120,0,65,235,130,247,159,193,255,55,8,4,224,180,255,50,9,4,224,
181,255,50,11,4,224,11,0,50,10,4,224,180,255,58,176,197,179,22,106,2,0,0,0,
0,64,235,128,247,159,13,70,120,111,236,151,128,69,120,176,255,55,10,4,224,172,255,50,
208,197,179,22,106,2,0,0,0,128,64,235,129,247,159,1,70,120,13,68,120,239,237,151,
165,255,55,64,48,41,182,108,2,0,0,0,79,7,120,79,6,190,79,5,190,79,4,190,
111,0,177,0,0,6,10,34,239,12,34,239,64,229,33,16,128,251,129,48,41,97,64,120,
0,128,251,32,16,136,240,175,46,6,34,182,201,34,169,128,12,32,104,105,2,0,0,0,
205,34,169,128,12,32,104,105,2,0,0,0,7,226,168,0,0,6,32,230,33,16,64,120,
98,64,96,12,0,50,0,64,235,233,254,7,0,194,179,98,106,2,0,0,0,64,219,41,
1,0,32,204,136,2,0,0,0,0,194,179,98,106,2,0,0,0,32,230,33,16,64,120,
97,64,96,3,0,58,74,119,2,0,0,0,1,0,55,210,254,7,33,230,33,17,20,160,
0,128,251,0,0,6,136,31,120,0,4,120,32,230,33,16,64,120,98,64,96,12,0,50,
0,194,179,98,106,2,0,0,0,64,219,41,1,0,32,204,136,2,0,0,0,0,194,179,
98,106,2,0,0,0,0,64,235,193,254,7,32,230,33,16,64,120,97,64,96,7,0,58,
8,0,120,180,118,2,0,0,0,36,119,2,0,0,0,0,132,251,4,0,55,8,64,120,
168,254,7,154,254,7,0,132,251,0,194,179,98,106,2,0,0,0,8,0,224,6,0,58,
64,219,41,1,0,32,204,136,2,0,0,0,0,48,32,5,0,55,128,222,41,1,0,32,
204,136,2,0,0,0,0,16,32,79,4,120,0,0,6,32,230,33,16,64,120,98,64,96,
9,0,50,128,222,41,1,0,32,204,136,2,0,0,0,64,48,41,182,108,2,0,0,0,
16,192,179,147,254,7,32,230,33,16,64,120,97,64,96,17,0,58,0,0,235,234,117,2,
0,0,0,0,4,224,13,0,50,32,200,41,1,0,32,204,136,2,0,0,0,224,136,41,
1,0,32,204,136,2,0,0,0,64,48,41,182,108,2,0,0,0,1,0,55,92,254,7,
32,182,41,1,0,32,38,137,2,0,0,0,0,0,6,32,230,33,16,64,120,98,64,96,
9,0,50,128,222,41,1,0,32,204,136,2,0,0,0,64,48,41,182,108,2,0,0,0,
16,192,179,107,254,7,32,230,33,16,64,120,97,64,96,3,0,58,202,117,2,0,0,0,
1,0,55,70,254,7,192,182,41,1,0,32,38,137,2,0,0,0,0,0,6,224,153,41,
1,0,32,204,136,2,0,0,0,16,243,128,97,64,96,62,107,2,0,0,0,0,194,179,
98,106,2,0,0,0,64,229,33,16,64,120,62,107,2,0,0,0,80,48,41,182,108,2,
0,0,0,0,0,6,136,31,120,36,28,2,0,0,0,158,26,2,0,0,0,0,4,120,
36,28,2,0,0,0,158,26,2,0,0,0,136,0,233,225,143,80,13,0,62,33,230,33,
8,68,233,97,68,100,17,65,120,2,4,161,136,72,113,128,0,233,227,143,80,4,0,62,
65,229,33,128,72,233,0,0,224,55,0,58,96,229,33,16,116,161,224,50,41,1,0,32,
38,137,2,0,0,0,128,1,235,34,0,32,17,0,32,1,0,120,150,28,2,0,0,0,
33,230,33,0,64,233,97,64,96,17,65,120,2,4,161,0,64,113,128,72,120,0,8,163,
13,0,58,128,11,41,1,0,32,38,137,2,0,0,0,128,1,235,66,0,32,17,0,32,
1,0,120,150,28,2,0,0,0,65,229,33,128,72,233,32,0,55,97,242,128,48,0,35,
128,143,80,4,0,62,32,21,41,1,0,32,204,136,2,0,0,0,32,40,41,1,0,32,
204,136,2,0,0,0,128,1,235,50,0,32,17,0,32,1,0,120,150,28,2,0,0,0,
65,229,33,128,72,233,11,0,55,97,242,128,48,0,35,128,143,80,4,0,62,32,21,41,
1,0,32,204,136,2,0,0,0,147,255,7,32,230,33,16,20,161,96,229,33,16,68,160,
79,4,120,0,0,6,32,230,33,16,64,120,97,64,96,10,0,58,201,34,168,201,2,168,
203,2,169,203,34,169,64,229,33,144,128,251,32,192,179,72,117,2,0,0,0,1,0,55,
217,254,7,0,0,6,33,230,33,17,64,120,0,20,161,128,72,120,0,8,163,1,0,50,
7,226,169,0,0,6,136,159,190,138,159,190,140,159,190,142,31,120,225,15,80,9,0,50,
3,0,57,226,15,80,118,0,58,100,0,55,96,23,41,1,0,32,204,136,2,0,0,0,
116,0,55,128,0,235,0,48,32,98,117,2,0,0,0,32,32,41,1,0,32,38,137,2,
0,0,0,203,2,171,3,0,50,0,6,235,203,34,171,12,0,58,32,200,41,1,0,32,
204,136,2,0,0,0,224,136,41,1,0,32,204,136,2,0,0,0,64,48,41,182,108,2,
0,0,0,91,0,55,45,230,33,200,221,41,9,0,32,10,222,41,11,0,32,14,16,32,
157,64,120,97,192,96,10,0,58,0,0,235,234,117,2,0,0,0,12,0,120,180,118,2,
0,0,0,36,119,2,0,0,0,0,128,251,5,0,55,118,253,7,12,64,120,137,253,7,
123,253,7,0,128,251,0,0,224,30,0,58,12,0,120,58,109,2,0,0,0,128,194,179,
98,106,2,0,0,0,12,128,209,58,109,2,0,0,0,12,8,163,4,0,58,10,0,190,
204,136,2,0,0,0,15,0,55,157,64,120,97,192,96,6,0,58,74,119,2,0,0,0,
16,192,179,150,118,2,0,0,0,3,0,55,115,253,7,16,192,179,96,253,7,8,0,190,
204,136,2,0,0,0,157,64,120,97,192,96,3,0,58,202,117,2,0,0,0,1,0,55,
79,253,7,12,6,232,142,15,86,196,255,58,64,48,41,182,108,2,0,0,0,21,0,55,
137,255,7,96,213,41,1,0,32,38,137,2,0,0,0,224,159,41,1,0,32,38,137,2,
0,0,0,16,192,179,204,253,7,32,230,33,16,64,120,97,64,96,6,0,50,82,254,7,
4,0,55,224,63,41,1,0,32,38,137,2,0,0,0,79,7,120,79,6,190,79,5,190,
79,4,190,0,0,6,32,196,41,1,0,32,38,137,2,0,0,0,0,0,6,136,159,190,
138,159,190,140,159,190,201,34,168,201,2,168,203,2,169,203,34,169,200,194,169,96,229,33,
144,64,120,1,68,160,1,52,161,1,72,120,49,0,32,32,192,179,72,117,2,0,0,0,
128,221,41,1,0,32,204,136,2,0,0,0,11,230,33,252,229,33,234,229,33,109,229,33,
136,221,41,9,0,32,90,106,2,0,0,0,128,77,120,128,128,251,196,8,222,1,78,120,
226,79,64,4,0,58,11,0,120,64,135,2,0,0,0,244,255,55,225,207,80,135,0,50,
5,0,57,228,207,80,164,0,50,230,207,80,168,0,58,151,0,55,0,128,251,128,0,235,
239,15,80,224,143,88,120,0,62,0,96,1,164,0,55,14,0,55,17,0,55,23,0,55,
28,0,55,113,0,55,31,0,55,37,0,55,43,0,55,49,0,55,108,0,55,107,0,55,
106,0,55,105,0,55,104,0,55,97,0,55,8,0,190,204,136,2,0,0,0,210,255,55,
0,0,235,234,117,2,0,0,0,16,192,179,98,106,2,0,0,0,203,255,55,202,117,2,
0,0,0,16,192,179,98,106,2,0,0,0,197,255,55,74,119,2,0,0,0,98,106,2,
0,0,0,192,255,55,0,64,235,150,118,2,0,0,0,16,192,179,98,106,2,0,0,0,
185,255,55,16,192,179,150,118,2,0,0,0,16,192,179,98,106,2,0,0,0,178,255,55,
231,252,7,0,4,224,175,255,58,0,64,235,98,106,2,0,0,0,171,255,55,16,192,179,
98,106,2,0,0,0,34,2,171,254,255,50,49,17,128,1,64,120,129,77,120,226,207,80,
22,0,50,5,0,62,1,4,224,11,0,50,225,207,80,35,0,58,12,0,55,240,207,80,
22,0,50,1,194,179,129,79,80,23,0,50,227,79,80,27,0,58,12,0,55,202,96,2,
0,0,0,16,0,32,23,0,55,174,96,2,0,0,0,16,0,32,19,0,55,150,96,2,
0,0,0,16,0,32,15,0,55,230,96,2,0,0,0,0,128,251,11,0,55,192,207,179,
157,78,96,16,0,32,7,0,55,192,207,179,29,64,96,0,4,160,128,78,120,16,0,32,
1,0,55,16,0,32,98,106,2,0,0,0,117,255,55,0,64,235,16,253,7,16,192,179,
98,106,2,0,0,0,111,255,55,0,64,235,98,106,2,0,0,0,107,255,55,111,64,96,
128,77,232,16,192,179,98,106,2,0,0,0,0,77,235,27,4,224,99,255,50,90,106,2,
0,0,0,0,128,251,180,118,2,0,0,0,36,119,2,0,0,0,98,106,2,0,0,0,
26,64,232,0,77,120,155,79,16,243,255,62,85,255,55,99,64,96,128,77,120,128,128,251,
32,192,179,72,117,2,0,0,0,16,192,179,98,106,2,0,0,0,75,255,55,118,59,2,
0,0,0,16,192,179,98,106,2,0,0,0,69,255,55,0,64,235,98,106,2,0,0,0,
65,255,55,79,6,190,79,5,190,79,4,190,0,0,6,227,15,80,4,0,62,16,161,168,
16,129,168,224,3,32,16,0,55,254,15,80,4,0,62,16,161,168,16,129,169,160,15,32,
10,0,55,65,15,32,129,15,80,4,0,62,16,161,169,16,129,168,0,125,32,3,0,55,
16,161,169,16,129,169,0,232,35,0,0,6,0,128,235,112,8,136,96,8,136,8,33,239,
6,33,239,22,33,239,20,33,239,30,97,168,64,15,32,224,8,136,0,64,34,208,8,136,
135,128,169,31,225,168,17,225,168,1,0,33,50,4,128,2,128,96,253,255,50,17,225,169,
31,225,169,50,8,128,64,8,128,128,0,235,192,0,221,0,0,32,0,0,65,224,128,72,
0,0,6,136,159,190,138,159,190,0,1,120,132,160,169,132,32,169,241,15,46,112,52,128,
128,128,96,0,160,32,1,0,112,112,52,136,0,254,47,142,6,182,160,0,179,112,52,136,
8,33,239,6,33,239,128,0,40,128,8,136,48,0,32,48,10,136,48,8,32,16,10,136,
66,97,171,3,0,50,0,10,128,66,97,171,253,255,58,70,97,171,3,0,50,32,10,128,
70,97,171,253,255,58,66,97,171,254,255,50,10,10,128,40,10,128,96,0,184,2,0,224,
26,0,50,128,5,235,128,4,235,128,1,235,96,0,184,2,0,55,4,4,190,6,5,190,
66,97,171,254,255,50,6,10,128,128,3,235,36,10,128,128,2,235,0,0,67,129,128,75,
10,0,80,139,128,88,8,4,82,137,132,90,192,68,221,8,0,32,0,0,68,129,128,76,
131,1,232,131,15,81,235,255,62,66,33,239,70,33,239,17,225,169,128,1,235,6,6,2,
0,0,0,79,5,190,79,4,190,0,0,6,136,159,190,0,4,190,40,243,136,57,243,136,
30,33,239,16,33,239,156,33,239,8,0,224,5,0,58,0,254,47,202,38,182,128,230,33,
0,72,235,24,0,55,8,0,120,113,255,7,17,0,9,8,128,216,0,4,233,104,8,136,
9,196,185,66,68,222,176,71,33,0,68,184,9,0,209,1,254,47,82,54,128,130,128,96,
97,1,179,81,54,136,208,12,136,192,12,136,96,0,32,224,12,136,17,225,168,33,192,179,
128,230,33,1,72,120,79,4,190,0,0,6,128,0,120,32,243,128,213,255,7,0,0,6,
136,159,190,138,159,190,140,31,120,30,33,239,16,33,239,156,33,239,128,230,33,33,192,179,
144,207,16,15,0,58,0,254,47,202,38,182,64,170,41,1,0,32,38,137,2,0,0,0,
128,230,33,0,72,235,224,235,128,0,0,232,0,128,251,161,199,33,97,64,120,0,4,224,
109,0,50,224,235,128,0,0,232,0,128,251,224,235,136,36,28,2,0,0,0,158,26,2,
0,0,0,0,5,120,36,28,2,0,0,0,158,26,2,0,0,0,10,1,233,225,249,32,
129,15,81,55,0,62,207,128,222,40,5,2,0,0,0,0,4,190,96,17,184,172,4,2,
0,0,0,0,0,224,46,0,52,2,0,32,131,44,36,8,0,190,168,4,2,0,0,0,
0,0,224,39,0,61,10,0,120,25,255,7,17,0,9,10,128,216,0,6,233,97,96,184,
140,5,2,0,0,0,0,5,190,2,0,32,131,44,36,8,0,190,8,8,2,0,0,0,
0,1,190,10,0,190,198,9,2,0,0,0,242,4,2,0,0,0,2,254,47,83,54,128,
3,1,97,98,1,179,82,54,136,208,12,136,192,12,136,96,0,32,224,12,136,108,8,136,
17,225,168,160,189,41,1,0,32,38,137,2,0,0,0,33,192,179,128,230,33,1,72,120,
37,0,55,96,229,33,16,116,161,224,150,41,1,0,32,38,137,2,0,0,0,160,151,41,
1,0,32,204,136,2,0,0,0,128,1,235,2,250,32,17,0,32,32,3,32,150,28,2,
0,0,0,0,4,120,8,0,120,225,254,7,17,0,9,8,128,216,0,6,233,96,152,41,
1,0,32,204,136,2,0,0,0,128,1,235,50,6,32,131,0,120,32,3,32,150,28,2,
0,0,0,207,128,222,40,5,2,0,0,0,0,4,190,185,255,55,79,6,120,79,5,190,
79,4,190,0,0,6,136,159,190,138,159,190,128,230,33,33,192,179,144,207,16,5,0,58,
0,85,41,1,0,32,38,137,2,0,0,0,133,1,55,32,153,41,1,0,32,204,136,2,
0,0,0,30,33,239,16,33,239,201,66,168,0,254,47,134,6,182,160,0,179,48,52,136,
160,3,32,128,8,136,201,254,7,0,4,190,96,85,184,242,255,35,130,15,80,224,143,88,
11,0,54,200,93,221,72,80,222,139,5,112,72,85,221,72,72,222,10,5,112,200,76,221,
72,64,222,137,4,112,72,68,221,9,0,55,160,194,41,1,0,32,204,136,2,0,0,0,
16,129,169,16,161,169,177,254,7,0,4,190,96,85,184,240,249,32,1,0,32,96,17,184,
0,64,225,1,200,225,2,208,225,3,216,225,8,0,54,8,0,190,236,107,2,0,0,0,
192,223,41,1,0,32,38,137,2,0,0,0,73,1,55,224,15,84,224,143,92,224,15,93,
224,143,93,64,1,50,160,194,41,1,0,32,204,136,2,0,0,0,8,0,120,177,254,7,
96,17,184,4,168,33,101,0,32,96,51,184,4,0,225,5,136,225,6,144,225,7,152,225,
78,0,54,46,5,2,0,0,0,0,1,190,112,59,36,161,59,37,8,8,2,0,0,0,
176,4,2,0,0,0,2,5,190,0,4,190,4,106,40,21,0,32,96,51,184,8,14,2,
0,0,0,236,107,2,0,0,0,224,194,179,98,106,2,0,0,0,4,106,40,21,0,32,
96,51,184,8,0,190,10,1,190,22,14,2,0,0,0,2,5,190,0,4,190,240,112,34,
1,0,32,96,17,184,0,64,225,1,200,225,2,208,225,3,216,225,3,0,62,0,195,179,
98,106,2,0,0,0,112,62,32,1,0,32,96,17,184,0,64,225,1,200,225,2,208,225,
3,216,225,3,0,62,0,195,179,98,106,2,0,0,0,48,6,32,1,0,32,96,17,184,
0,64,225,1,200,225,2,208,225,3,216,225,3,0,62,0,195,179,98,106,2,0,0,0,
144,0,32,1,0,32,96,17,184,0,64,225,1,200,225,2,208,225,3,216,225,3,0,62,
0,195,179,98,106,2,0,0,0,8,0,190,88,107,2,0,0,0,222,0,55,180,225,46,
21,0,32,96,51,184,4,0,225,5,136,225,6,144,225,7,152,225,67,0,54,46,5,2,
0,0,0,0,1,190,144,47,32,81,33,37,8,8,2,0,0,0,176,4,2,0,0,0,
2,5,190,0,4,190,4,113,34,5,0,32,96,51,184,8,14,2,0,0,0,236,107,2,
0,0,0,224,194,179,98,106,2,0,0,0,4,113,34,5,0,32,96,51,184,8,0,190,
10,1,190,22,14,2,0,0,0,2,5,190,0,4,190,112,62,32,1,0,32,96,17,184,
0,64,225,1,200,225,2,208,225,3,216,225,3,0,62,0,195,179,98,106,2,0,0,0,
48,6,32,1,0,32,96,17,184,0,64,225,1,200,225,2,208,225,3,216,225,3,0,62,
0,195,179,98,106,2,0,0,0,144,0,32,1,0,32,96,17,184,0,64,225,1,200,225,
2,208,225,3,216,225,3,0,62,0,195,179,98,106,2,0,0,0,8,0,120,76,107,2,
0,0,0,147,0,55,4,196,41,5,0,32,96,51,184,4,0,225,5,136,225,6,144,225,
7,152,225,56,0,54,46,5,2,0,0,0,0,1,190,128,178,38,225,6,37,8,8,2,
0,0,0,176,4,2,0,0,0,2,5,190,0,4,190,132,62,32,5,0,32,96,51,184,
8,14,2,0,0,0,236,107,2,0,0,0,224,194,179,98,106,2,0,0,0,132,62,32,
5,0,32,96,51,184,8,0,190,10,1,190,22,14,2,0,0,0,2,5,190,0,4,190,
48,6,32,1,0,32,96,17,184,0,64,225,1,200,225,2,208,225,3,216,225,3,0,62,
0,195,179,98,106,2,0,0,0,144,0,32,1,0,32,96,17,184,0,64,225,1,200,225,
2,208,225,3,216,225,3,0,62,0,195,179,98,106,2,0,0,0,8,0,120,76,107,2,
0,0,0,83,0,55,148,22,35,5,0,32,96,51,184,4,0,225,5,136,225,6,144,225,
7,152,225,45,0,54,46,5,2,0,0,0,0,1,190,0,194,43,225,235,36,8,8,2,
0,0,0,176,4,2,0,0,0,2,5,190,0,4,190,68,6,32,5,0,32,96,51,184,
8,14,2,0,0,0,236,107,2,0,0,0,224,194,179,98,106,2,0,0,0,68,6,32,
5,0,32,96,51,184,8,0,190,10,1,190,22,14,2,0,0,0,2,5,190,0,4,190,
144,0,32,1,0,32,96,17,184,0,64,225,1,200,225,2,208,225,3,216,225,3,0,62,
0,195,179,98,106,2,0,0,0,8,64,120,62,107,2,0,0,0,30,0,55,46,5,2,
0,0,0,0,1,190,0,104,41,129,209,36,8,8,2,0,0,0,176,4,2,0,0,0,
2,5,190,0,4,190,164,0,32,5,0,32,96,51,184,8,14,2,0,0,0,236,107,2,
0,0,0,224,194,179,98,106,2,0,0,0,164,0,32,5,0,32,96,51,184,8,0,190,
10,1,190,22,14,2,0,0,0,62,107,2,0,0,0,192,223,41,1,0,32,38,137,2,
0,0,0,4,0,55,64,62,41,1,0,32,38,137,2,0,0,0,240,1,32,134,38,183,
30,33,239,16,33,239,79,5,190,79,4,190,0,0,6,30,33,239,16,33,239,201,66,168,
0,254,47,134,6,182,160,0,179,48,52,136,160,3,32,128,8,136,69,253,7,0,1,190,
240,15,32,128,15,81,224,143,89,5,0,54,72,26,221,200,18,222,133,2,114,72,18,221,
4,0,55,16,129,169,16,161,169,56,253,7,0,2,190,240,1,32,134,38,183,30,33,239,
16,33,239,4,0,190,0,0,6,96,229,33,16,64,120,99,64,96,2,0,58,201,66,168,
1,0,55,200,194,168,224,170,41,1,0,32,204,136,2,0,0,0,0,0,6,96,229,33,
16,64,120,99,64,96,3,0,58,201,66,169,203,66,168,2,0,55,200,194,169,202,194,168,
160,200,41,1,0,32,38,137,2,0,0,0,0,0,6,96,229,33,16,64,120,99,64,96,
3,0,58,201,66,169,203,66,169,2,0,55,200,194,169,202,194,169,0,201,41,1,0,32,
38,137,2,0,0,0,0,0,6,96,229,33,16,64,120,99,64,96,7,0,58,201,66,168,
0,0,0,0,0,0,80,22,128,69,0,221,79,0,222,6,0,55,200,194,168,0,0,0,
0,0,0,80,22,128,73,0,221,79,0,222,0,0,6,228,135,71,136,159,190,138,159,190,
140,159,190,142,31,120,30,33,239,16,33,239,156,33,239,128,230,33,33,192,179,144,207,16,
16,0,58,224,235,128,0,0,232,0,128,251,161,199,33,97,64,120,0,4,224,9,0,58,
0,254,47,202,38,182,64,170,41,1,0,32,38,137,2,0,0,0,128,230,33,0,72,235,
100,0,55,224,235,128,0,0,232,0,128,251,224,235,136,36,28,2,0,0,0,158,26,2,
0,0,0,97,229,33,17,4,224,10,0,53,207,128,222,40,5,2,0,0,0,0,4,190,
2,0,32,67,51,36,172,4,2,0,0,0,0,0,224,19,0,52,96,229,33,16,116,161,
32,143,41,1,0,32,38,137,2,0,0,0,128,1,235,66,11,32,131,0,120,160,5,32,
150,28,2,0,0,0,207,128,222,40,5,2,0,0,0,0,4,190,19,192,179,131,247,159,
2,0,55,0,64,235,128,247,159,12,254,47,109,0,32,46,78,32,106,178,41,11,0,32,
16,161,168,16,129,168,2,0,44,163,85,36,8,0,190,8,8,2,0,0,0,2,0,36,
195,73,36,198,9,2,0,0,0,2,0,32,131,39,36,62,6,2,0,0,0,242,4,2,
0,0,0,83,54,128,3,1,102,98,1,179,82,54,136,208,12,136,192,12,136,237,12,136,
110,8,136,17,225,168,10,0,190,38,137,2,0,0,0,32,192,179,129,230,33,128,72,120,
143,241,151,3,4,224,18,0,50,19,0,32,66,11,32,128,0,235,0,128,235,150,28,2,
0,0,0,207,128,222,40,5,2,0,0,0,0,4,190,96,17,184,168,4,2,0,0,0,
0,0,224,206,255,61,192,49,41,182,108,2,0,0,0,79,7,120,79,6,190,79,5,190,
79,4,190,79,0,177,0,0,6,225,220,33,17,64,104,0,128,251,226,7,41,98,64,120,
128,72,120,0,0,6,136,31,120,201,34,168,64,229,33,17,192,179,144,207,16,3,0,58,
16,0,32,104,105,2,0,0,0,203,34,169,201,34,169,64,229,33,16,4,224,4,0,58,
64,31,32,104,105,2,0,0,0,3,0,55,96,4,32,104,105,2,0,0,0,201,34,168,
64,229,33,16,4,224,4,0,58,16,4,32,104,105,2,0,0,0,3,0,55,96,0,32,
104,105,2,0,0,0,40,0,32,203,34,171,1,0,58,0,4,235,64,229,33,16,4,224,
4,0,58,64,31,32,104,105,2,0,0,0,3,0,55,0,2,32,104,105,2,0,0,0,
203,34,171,1,0,58,24,0,32,8,0,120,79,4,120,0,0,6,136,31,120,0,68,120,
201,34,168,203,34,169,201,34,169,64,229,33,16,4,224,4,0,58,64,0,32,104,105,2,
0,0,0,3,0,55,16,0,32,104,105,2,0,0,0,8,4,224,1,0,50,201,34,168,
64,229,33,16,4,224,3,0,58,128,0,32,104,105,2,0,0,0,8,4,224,14,0,50,
88,22,128,70,68,221,79,68,222,64,229,33,16,4,224,4,0,58,0,2,32,104,105,2,
0,0,0,25,0,55,96,0,32,104,105,2,0,0,0,21,0,55,64,229,33,16,4,224,
4,0,58,144,1,32,104,105,2,0,0,0,3,0,55,64,0,32,104,105,2,0,0,0,
201,34,168,64,229,33,16,4,224,4,0,58,112,0,32,104,105,2,0,0,0,3,0,55,
32,0,32,104,105,2,0,0,0,64,229,33,16,4,224,3,0,58,80,0,32,104,105,2,
0,0,0,8,64,120,79,4,120,0,0,6,136,159,190,0,68,120,137,192,179,8,132,251,
97,0,100,180,255,7,8,4,209,0,4,224,1,0,50,8,116,160,137,68,233,247,255,58,
64,229,33,16,4,224,3,0,58,128,0,32,104,105,2,0,0,0,8,64,120,79,4,190,
0,0,6,228,135,71,136,159,190,138,159,190,140,159,190,142,31,120,224,220,33,0,72,235,
176,226,33,16,64,120,98,64,96,100,0,58,101,255,7,0,0,224,9,0,50,0,64,235,
241,220,33,128,72,120,1,221,33,128,72,120,160,226,33,16,144,161,0,64,235,96,0,55,
160,226,33,16,64,120,209,255,7,24,192,179,128,68,235,137,247,159,8,69,120,13,221,33,
236,220,33,254,220,33,16,192,179,130,255,7,128,69,120,16,192,179,127,255,7,11,4,224,
3,0,50,0,4,224,24,0,50,54,0,55,0,4,224,21,0,58,30,64,120,128,79,85,
8,0,49,9,128,251,0,0,70,176,69,144,139,69,100,139,133,251,139,5,234,207,93,222,
4,0,55,128,69,109,139,133,251,139,5,233,207,93,222,11,4,224,4,0,58,232,79,85,
61,0,62,138,78,120,59,0,55,225,207,85,7,0,58,9,128,251,128,128,70,0,0,70,
48,64,144,8,64,112,144,64,152,7,0,55,9,128,251,128,128,70,0,0,70,8,193,234,
48,64,144,2,64,96,144,64,152,139,133,251,11,0,234,79,0,222,79,255,7,10,69,232,
8,68,68,6,0,58,9,128,251,0,0,70,48,64,144,13,255,7,137,68,232,24,192,179,
231,207,84,192,255,54,0,196,179,128,79,85,11,0,54,224,220,33,16,4,224,8,0,58,
240,220,33,143,240,151,1,72,120,1,4,224,19,0,58,160,226,33,16,144,160,16,0,55,
0,64,235,241,220,33,128,72,120,1,221,33,128,72,120,160,226,33,16,144,161,0,64,235,
79,7,120,79,6,190,79,5,190,79,4,190,79,0,177,0,0,6,138,247,159,204,255,55,
16,221,33,16,4,224,237,255,50,16,192,179,243,255,55,0,64,235,241,220,33,128,72,120,
1,221,33,128,72,120,160,226,33,16,144,161,116,255,7,0,0,6,114,255,7,0,0,6,
33,194,179,129,79,80,23,0,50,5,0,62,228,79,80,25,0,50,240,79,80,33,0,58,
7,0,55,129,194,179,129,79,80,9,0,50,209,194,179,129,79,80,26,0,58,20,0,55,
96,97,41,1,0,32,204,136,2,0,0,0,24,0,55,32,116,41,1,0,32,204,136,2,
0,0,0,19,0,55,224,122,41,1,0,32,204,136,2,0,0,0,14,0,55,64,117,41,
1,0,32,204,136,2,0,0,0,9,0,55,32,150,41,1,0,32,204,136,2,0,0,0,
4,0,55,160,169,41,1,0,32,204,136,2,0,0,0,128,7,41,182,108,2,0,0,0,
0,0,6,136,159,190,138,159,190,0,4,120,129,5,120,0,194,179,98,106,2,0,0,0,
8,64,120,62,107,2,0,0,0,144,7,41,212,107,2,0,0,0,139,4,120,0,4,235,
10,194,179,57,128,251,58,109,2,0,0,0,10,64,120,98,106,2,0,0,0,8,4,232,
232,15,84,247,255,58,32,208,41,1,0,32,204,136,2,0,0,0,27,64,120,176,255,7,
79,5,190,79,4,190,0,0,6,0,192,235,7,255,7,0,128,251,0,0,6,4,255,7,
0,16,32,0,0,6,16,192,179,186,254,7,0,0,6,80,241,128,71,0,221,79,0,222,
181,254,7,0,0,6,80,241,128,72,0,222,97,0,96,0,0,6,160,226,33,16,128,161,
0,59,41,1,0,32,38,137,2,0,0,0,0,0,6,160,226,33,16,128,160,0,59,41,
1,0,32,38,137,2,0,0,0,0,0,6,96,229,33,16,68,160,36,28,2,0,0,0,
158,26,2,0,0,0,128,0,233,225,143,80,3,0,62,65,229,33,128,72,233,14,0,55,
96,229,33,16,116,161,64,6,41,1,0,32,38,137,2,0,0,0,128,1,235,34,0,32,
17,0,32,1,0,120,150,28,2,0,0,0,65,229,33,128,72,233,144,221,33,0,72,235,
0,0,6,201,34,168,203,34,169,0,0,6,160,195,41,1,0,32,38,137,2,0,0,0,
0,0,6,136,31,120,71,254,7,0,4,120,32,195,41,1,0,32,204,136,2,0,0,0,
8,0,224,5,0,58,128,217,41,1,0,32,38,137,2,0,0,0,18,0,55,32,200,41,
1,0,32,204,136,2,0,0,0,225,15,84,5,0,58,224,136,41,1,0,32,204,136,2,
0,0,0,4,0,55,224,144,41,1,0,32,204,136,2,0,0,0,128,7,41,182,108,2,
0,0,0,79,4,120,0,0,6,8,0,250,136,159,190,138,159,190,0,4,120,136,0,233,
0,3,32,128,143,80,40,0,62,136,4,233,144,221,33,16,128,251,128,143,84,5,0,57,
32,49,41,1,0,32,38,137,2,0,0,0,178,0,55,0,179,41,1,0,32,204,136,2,
0,0,0,8,64,232,62,107,2,0,0,0,176,7,41,212,107,2,0,0,0,106,76,185,
144,221,33,0,4,68,128,4,235,10,194,179,88,128,251,58,109,2,0,0,0,10,64,120,
98,106,2,0,0,0,24,64,120,120,254,7,137,4,232,232,143,84,245,255,58,128,7,41,
182,108,2,0,0,0,148,0,55,80,5,32,128,15,84,125,0,50,6,0,62,8,0,224,
14,0,50,48,3,32,128,15,84,135,0,58,90,0,55,192,14,32,128,15,84,38,0,50,
0,15,32,128,15,84,35,0,50,192,12,32,128,15,84,125,0,58,116,0,55,160,179,41,
1,0,32,38,137,2,0,0,0,224,132,41,1,0,32,38,137,2,0,0,0,144,221,33,
16,4,224,5,0,58,32,49,41,1,0,32,38,137,2,0,0,0,10,0,55,169,221,33,
0,4,235,154,221,33,8,4,232,8,0,190,28,255,7,234,132,68,26,128,251,136,15,80,
249,255,62,0,179,40,1,0,32,38,137,2,0,0,0,97,0,55,160,226,33,8,72,120,
192,14,32,128,15,84,5,0,58,0,144,41,1,0,32,38,137,2,0,0,0,4,0,55,
64,180,41,1,0,32,38,137,2,0,0,0,224,132,41,1,0,32,38,137,2,0,0,0,
196,254,7,145,221,33,128,72,235,0,4,224,21,0,50,0,4,235,129,4,120,234,220,33,
8,4,232,17,221,33,8,0,120,245,254,7,8,0,233,233,15,80,8,0,62,25,128,251,
106,0,185,108,0,64,128,0,69,18,221,33,7,0,9,178,88,120,153,76,232,182,254,7,
0,4,224,238,255,58,0,42,41,1,0,32,38,137,2,0,0,0,48,0,55,73,255,7,
160,157,41,1,0,32,204,136,2,0,0,0,48,195,179,6,254,7,113,132,87,233,132,87,
0,197,235,11,194,179,10,64,120,0,254,7,0,108,120,0,128,251,58,109,2,0,0,0,
11,64,120,98,106,2,0,0,0,137,15,84,245,255,58,128,7,41,182,108,2,0,0,0,
15,240,151,150,254,7,20,0,55,45,255,7,224,156,41,1,0,32,38,137,2,0,0,0,
80,197,179,234,253,7,12,0,55,37,255,7,96,158,41,1,0,32,38,137,2,0,0,0,
192,204,179,226,253,7,4,0,55,224,63,41,1,0,32,38,137,2,0,0,0,79,5,190,
79,4,190,0,128,250,0,0,6,228,135,71,136,159,190,138,159,190,140,159,190,142,31,120,
201,34,168,203,34,169,200,194,169,96,229,33,144,64,120,1,68,160,1,52,161,1,72,120,
0,219,41,1,0,32,204,136,2,0,0,0,0,7,235,28,221,33,10,219,41,11,0,32,
77,229,33,90,106,2,0,0,0,128,247,159,128,128,251,196,8,222,226,79,64,4,0,58,
112,128,87,64,135,2,0,0,0,245,255,55,225,207,80,68,0,50,5,0,57,226,207,80,
88,0,50,228,207,80,97,0,58,90,0,55,226,79,80,17,0,50,5,0,62,0,4,224,
95,0,50,225,79,80,51,0,58,7,0,55,228,79,80,14,0,50,47,0,57,104,64,80,
225,79,80,44,0,62,14,0,55,10,0,190,204,136,2,0,0,0,218,255,55,34,253,7,
16,192,179,98,106,2,0,0,0,213,255,55,0,192,235,150,253,7,98,106,2,0,0,0,
208,255,55,16,192,179,98,106,2,0,0,0,0,207,179,143,240,151,233,207,80,1,0,58,
192,206,179,161,226,33,128,72,120,33,254,7,0,4,224,8,0,50,136,0,32,136,0,120,
12,0,120,28,109,2,0,0,0,34,254,7,0,4,224,249,255,58,136,0,32,128,196,235,
9,64,120,98,106,2,0,0,0,8,4,233,251,255,58,179,255,55,0,64,235,98,106,2,
0,0,0,175,255,55,111,64,96,0,64,232,128,247,159,16,192,179,98,106,2,0,0,0,
15,240,151,0,4,224,166,255,50,14,4,120,25,192,179,90,106,2,0,0,0,100,253,7,
9,64,120,98,106,2,0,0,0,8,4,232,143,240,151,1,128,251,136,15,80,245,255,62,
152,255,55,225,78,96,16,192,179,98,106,2,0,0,0,147,255,55,118,59,2,0,0,0,
16,192,179,98,106,2,0,0,0,141,255,55,0,64,235,98,106,2,0,0,0,137,255,55,
79,7,120,79,6,190,79,5,190,79,4,190,79,0,177,0,0,6,96,229,33,16,65,120,
2,68,161,65,229,33,128,72,235,1,206,179,1,72,97,0,0,6,224,15,80,224,143,88,
18,0,50,0,0,64,129,128,72,97,3,80,224,131,88,0,8,128,227,255,39,2,8,128,
128,0,81,97,10,184,134,15,82,135,143,90,6,0,49,131,143,80,248,255,54,4,3,83,
133,131,91,2,0,120,244,255,55,0,0,6,201,34,168,201,2,168,200,226,168,200,194,168,
201,66,168,192,66,168,192,34,169,194,34,169,0,254,47,202,38,182,16,229,33,0,72,235,
213,255,7,203,98,169,201,98,169,192,2,169,194,2,169,45,131,169,45,67,169,45,35,169,
45,99,169,44,163,169,44,131,169,0,14,32,32,35,183,48,35,239,32,240,33,32,25,136,
34,35,239,0,0,6,64,25,136,32,35,168,32,3,169,32,3,171,254,255,50,0,24,128,
0,0,6,4,1,32,19,0,32,0,1,235,2,1,65,128,130,97,1,0,50,2,0,160,
131,129,65,4,2,233,249,255,58,1,128,251,112,0,16,0,16,222,0,0,6,1,193,179,
240,255,7,0,0,6,0,128,251,129,192,179,236,255,7,0,0,6,34,3,32,18,65,120,
50,224,183,17,128,186,145,0,186,2,66,120,35,3,32,132,73,120,72,0,221,0,128,112,
0,0,6,0,1,32,32,8,136,0,33,239,2,161,239,5,225,168,0,0,6,129,62,32,
1,128,185,128,0,235,155,255,7,0,0,6,225,255,35,129,15,80,10,0,62,0,0,224,
10,0,50,0,0,64,0,1,233,1,8,128,1,0,120,0,1,181,128,15,81,252,255,62,
2,0,55,128,0,235,139,255,7,0,0,6,240,228,33,16,64,120,233,79,80,5,0,50,
0,128,251,0,0,64,1,37,41,225,0,120,65,17,136,128,0,40,0,17,136,0,64,32,
16,17,136,133,96,169,0,0,6,16,17,128,71,0,221,79,0,222,0,0,6,16,17,128,
97,0,96,0,0,6,0,0,235,96,243,136,16,0,32,80,243,136,32,229,33,16,16,161,
0,0,6,35,34,171,14,0,58,96,243,128,0,0,232,1,0,33,129,15,80,1,0,58,
0,0,235,81,243,128,129,15,80,5,0,50,96,243,136,65,242,128,97,64,120,0,128,251,
32,17,136,0,0,6,136,159,190,85,243,128,72,242,128,103,243,128,4,0,33,128,1,235,
6,32,32,7,1,232,3,0,120,132,15,81,1,0,50,2,0,120,133,15,80,8,0,50,
25,17,128,137,0,99,247,255,58,232,64,120,129,128,251,33,17,136,128,3,120,241,255,55,
103,243,136,79,4,190,0,0,6,81,243,128,98,243,128,130,143,80,4,0,58,194,34,169,
32,229,33,16,16,160,9,0,55,66,242,128,0,241,120,129,0,232,81,243,136,0,0,33,
128,143,80,2,0,58,0,0,235,80,243,136,0,0,6,136,159,190,0,68,120,0,195,179,
234,255,7,128,199,179,232,255,7,8,132,251,68,64,222,57,43,41,105,64,120,227,255,7,
111,4,100,105,64,124,224,255,7,79,4,190,0,0,6,34,2,171,254,255,50,48,17,128,
0,0,6,33,229,33,145,0,120,225,128,96,6,0,58,2,32,32,19,17,128,131,0,97,
253,255,58,0,128,251,32,17,136,0,0,6,136,159,190,0,4,120,32,227,41,1,0,32,
204,136,2,0,0,0,76,64,222,57,43,41,105,64,120,235,255,7,72,64,222,111,0,96,
105,64,120,231,255,7,68,64,222,111,0,96,105,64,120,227,255,7,111,4,100,105,64,124,
224,255,7,79,4,190,0,0,6,136,159,190,0,68,120,32,227,41,1,0,32,204,136,2,
0,0,0,8,132,251,68,64,222,57,43,41,105,64,120,211,255,7,111,4,100,105,64,124,
208,255,7,79,4,190,0,0,6,228,135,71,136,159,190,138,159,190,140,159,190,142,31,120,
132,247,159,224,15,80,224,143,88,3,0,50,4,4,224,4,0,58,42,0,55,0,195,179,
192,255,7,39,0,55,2,4,190,0,6,190,0,64,235,144,247,159,0,71,120,8,1,190,
12,0,190,6,6,2,0,0,0,0,5,190,159,240,151,1,4,224,3,0,58,224,15,85,
224,143,93,12,0,50,4,195,179,4,64,69,172,255,7,8,216,185,10,201,185,0,0,65,
8,85,184,139,5,64,10,6,86,139,134,94,16,192,179,144,247,159,14,71,232,143,240,151,
142,207,80,7,0,54,162,0,32,3,0,32,8,0,190,6,6,2,0,0,0,0,4,190,
222,255,55,79,7,120,79,6,190,79,5,190,79,4,190,79,0,177,0,0,6,0,128,251,
128,0,235,52,192,179,66,6,32,3,0,32,190,255,7,0,0,6,128,0,235,84,192,179,
2,113,34,3,0,32,184,255,7,0,0,6,132,192,179,2,104,41,131,9,32,179,255,7,
0,0,6,136,159,190,0,1,120,125,17,185,49,216,34,1,16,184,195,12,222,197,8,222,
176,71,33,0,8,184,1,4,209,8,64,120,226,255,7,224,194,179,114,255,7,64,6,32,
0,192,185,0,196,84,233,79,84,2,0,62,0,195,179,107,255,7,8,64,120,215,255,7,
79,4,190,0,0,6,33,227,168,192,0,32,181,254,7,227,255,7,33,227,169,0,0,6,
136,159,190,138,159,190,140,31,120,0,69,120,64,226,41,1,0,32,204,136,2,0,0,0,
136,0,32,9,200,179,11,195,179,28,195,179,138,192,100,11,64,120,1,0,50,12,64,120,
80,255,7,8,4,233,3,0,50,137,132,251,137,4,209,246,255,55,79,6,120,79,5,190,
79,4,190,0,0,6,136,31,120,0,4,120,24,64,120,0,4,224,5,0,50,8,4,232,
64,255,7,56,64,120,0,4,224,252,255,58,79,4,120,0,0,6,136,159,190,138,159,190,
140,31,120,0,5,190,240,35,36,241,0,32,128,15,85,129,143,93,23,0,54,2,36,36,
243,0,32,10,0,190,6,6,2,0,0,0,161,255,7,192,194,179,42,255,7,2,36,36,
243,0,32,10,0,190,38,6,2,0,0,0,0,5,190,28,0,32,112,62,32,128,15,85,
224,143,93,10,0,62,64,42,41,214,255,7,12,0,120,43,0,55,0,0,235,113,62,32,
129,15,85,224,143,93,38,0,54,0,6,235,130,62,32,3,0,32,10,0,190,6,6,2,
0,0,0,0,4,190,48,6,32,128,15,84,224,143,92,3,0,54,8,0,120,126,255,7,
15,0,55,12,0,224,11,0,50,233,15,84,224,143,92,3,0,54,0,195,179,1,255,7,
2,0,55,144,42,41,183,255,7,8,0,120,113,255,7,2,0,55,8,0,120,110,255,7,
192,194,179,247,254,7,130,62,32,3,0,32,10,0,190,38,6,2,0,0,0,0,5,190,
16,0,32,49,6,32,129,15,85,224,143,93,3,0,54,10,0,120,95,255,7,15,0,55,
0,0,224,11,0,50,233,15,85,224,143,93,3,0,54,0,195,179,226,254,7,2,0,55,
144,42,41,152,255,7,10,0,120,82,255,7,2,0,55,10,0,120,79,255,7,79,6,120,
79,5,190,79,4,190,0,0,6,142,255,7,208,192,179,211,254,7,160,192,179,209,254,7,
0,0,6,136,159,190,138,159,190,224,180,41,1,0,32,38,137,2,0,0,0,224,159,41,
1,0,32,38,137,2,0,0,0,32,159,41,1,0,32,204,136,2,0,0,0,0,0,235,
64,255,7,96,227,41,1,0,32,204,136,2,0,0,0,201,42,41,106,227,41,11,0,32,
12,0,55,33,227,168,192,0,32,6,254,7,0,4,120,33,227,169,9,0,120,106,255,7,
8,0,120,47,255,7,10,0,190,204,136,2,0,0,0,81,254,7,0,4,224,241,255,50,
164,254,7,32,43,41,208,255,7,79,5,190,79,4,190,0,0,6,136,159,190,138,31,120,
1,5,120,10,0,224,7,0,50,128,4,120,0,4,235,57,64,120,154,254,7,8,4,232,
136,15,85,251,255,62,79,5,120,79,4,190,0,0,6,136,31,120,0,4,120,0,229,33,
16,64,120,225,79,80,16,0,50,5,0,57,226,79,80,23,0,50,227,79,80,40,0,58,
31,0,55,96,229,33,1,194,179,16,192,96,3,0,50,8,0,120,141,254,7,32,0,55,
8,64,120,161,254,7,29,0,55,96,229,33,1,194,179,16,192,96,3,0,50,8,0,120,
237,254,7,22,0,55,8,64,120,227,254,7,19,0,55,96,229,33,1,194,179,16,192,96,
4,0,50,72,64,222,13,255,7,0,194,179,108,254,7,8,64,120,9,255,7,8,0,55,
96,229,33,1,194,179,16,192,96,2,0,50,72,64,222,99,254,7,8,64,120,97,254,7,
79,4,120,0,0,6,1,16,32,18,17,128,2,128,96,253,255,50,0,0,6,64,17,136,
0,0,6,34,2,213,97,0,96,0,0,6,34,34,169,0,0,6,149,128,171,14,0,58,
34,242,128,48,242,128,130,15,80,10,0,50,1,32,32,19,17,128,3,128,96,253,255,58,
133,128,169,149,128,168,16,242,128,96,64,121,0,128,251,32,17,136,0,0,6,136,31,120,
60,254,7,0,68,120,58,254,7,72,68,221,0,128,251,8,0,112,79,4,120,0,0,6,
136,31,120,245,255,7,0,4,120,243,255,7,0,1,120,97,64,184,192,0,221,0,0,32,
128,1,235,2,0,112,131,128,112,79,4,120,0,0,6,145,1,32,0,16,32,98,117,2,
0,0,0,145,1,32,0,32,32,130,117,2,0,0,0,33,3,32,0,16,32,130,117,2,
0,0,0,0,0,6,136,159,190,138,159,190,140,159,190,128,6,120,1,134,251,29,0,50,
128,4,235,24,192,179,10,16,32,11,32,32,145,1,32,10,0,120,98,117,2,0,0,0,
8,128,251,141,0,96,1,0,104,0,0,233,79,0,222,146,1,32,139,0,120,62,118,2,
0,0,0,145,1,32,10,0,120,130,117,2,0,0,0,145,1,32,11,0,120,130,117,2,
0,0,0,8,68,68,137,4,232,140,143,84,231,255,57,79,6,190,79,5,190,79,4,190,
0,0,6,136,159,190,138,159,190,140,159,190,0,134,251,128,4,235,26,0,50,0,128,251,
0,0,233,29,0,32,128,110,221,9,4,120,136,4,120,42,3,32,11,16,32,138,0,120,
11,0,120,98,117,2,0,0,0,137,4,209,0,32,32,10,119,2,0,0,0,0,128,251,
13,0,221,128,132,116,138,0,120,11,0,120,130,117,2,0,0,0,8,4,232,140,15,84,
238,255,57,33,3,32,0,16,32,98,117,2,0,0,0,33,3,32,0,16,32,130,117,2,
0,0,0,65,1,32,0,32,32,130,117,2,0,0,0,9,0,120,79,6,190,79,5,190,
79,4,190,0,0,6,136,31,120,36,28,2,0,0,0,158,26,2,0,0,0,0,4,120,
36,28,2,0,0,0,158,26,2,0,0,0,8,4,233,225,15,84,2,0,62,227,15,80,
34,0,54,96,229,33,16,116,161,128,80,41,1,0,32,38,137,2,0,0,0,128,1,235,
34,0,32,17,0,32,1,0,120,150,28,2,0,0,0,40,231,33,0,64,233,97,64,96,
152,64,120,1,4,161,0,204,112,224,208,41,1,0,32,38,137,2,0,0,0,128,1,235,
50,0,32,131,0,120,16,0,32,150,28,2,0,0,0,99,64,96,0,64,64,145,207,179,
152,192,96,0,204,112,23,0,55,64,185,41,1,0,32,204,136,2,0,0,0,32,231,33,
16,64,120,232,64,96,64,192,179,1,0,58,96,192,179,62,107,2,0,0,0,0,194,179,
98,106,2,0,0,0,144,243,128,0,0,209,99,64,96,62,107,2,0,0,0,32,49,41,
182,108,2,0,0,0,96,229,33,1,195,179,16,200,112,65,6,32,0,48,32,130,117,2,
0,0,0,79,4,120,0,0,6,96,229,33,16,84,160,0,0,6,96,229,33,16,84,161,
0,0,6,136,31,120,40,231,33,24,52,160,0,223,41,1,0,32,204,136,2,0,0,0,
24,68,120,232,64,100,64,195,179,1,0,58,96,195,179,98,106,2,0,0,0,96,229,33,
16,84,161,64,49,41,182,108,2,0,0,0,79,4,120,0,0,6,32,231,33,16,52,161,
96,229,33,16,84,160,64,223,41,1,0,32,38,137,2,0,0,0,0,0,6,32,231,33,
16,64,120,104,64,96,6,0,50,160,209,41,1,0,32,38,137,2,0,0,0,0,0,235,
31,0,55,144,243,128,97,0,96,3,0,50,225,15,80,21,0,58,14,0,55,0,32,32,
10,119,2,0,0,0,33,3,32,0,16,32,98,117,2,0,0,0,33,3,32,0,16,32,
130,117,2,0,0,0,224,192,179,61,255,7,11,0,55,0,32,32,10,119,2,0,0,0,
0,193,179,55,255,7,5,0,55,224,185,41,1,0,32,38,137,2,0,0,0,0,0,235,
0,0,6,136,31,120,0,4,120,32,231,33,16,64,120,104,64,96,27,0,50,144,243,128,
97,0,96,3,0,50,225,15,80,9,0,58,4,0,55,97,192,179,8,0,120,250,254,7,
10,0,55,65,192,179,8,0,120,246,254,7,6,0,55,224,185,41,1,0,32,38,137,2,
0,0,0,0,0,235,30,0,55,144,243,128,0,0,209,99,0,96,94,105,2,0,0,0,
0,16,32,23,0,55,144,243,128,97,0,96,3,0,50,225,15,80,13,0,58,7,0,55,
212,254,7,225,192,179,8,0,120,222,254,7,208,254,7,0,16,32,10,0,55,1,193,179,
8,0,120,216,254,7,0,16,32,5,0,55,224,185,41,1,0,32,38,137,2,0,0,0,
0,0,235,79,4,120,0,0,6,136,159,190,0,0,224,3,0,50,225,15,80,59,0,58,
5,0,55,160,184,41,1,0,32,38,137,2,0,0,0,57,0,55,144,243,128,0,8,163,
41,0,58,32,229,33,16,0,160,109,255,7,0,0,235,179,255,7,127,255,7,0,0,235,
176,255,7,103,255,7,0,4,235,105,0,32,9,0,120,171,255,7,8,4,232,230,15,84,
251,255,58,64,0,32,166,255,7,114,255,7,122,255,7,0,4,120,32,229,33,16,0,161,
64,209,41,1,0,32,204,136,2,0,0,0,69,64,222,120,106,2,0,0,0,0,210,41,
1,0,32,204,136,2,0,0,0,127,64,100,166,106,2,0,0,0,64,49,41,182,108,2,
0,0,0,4,0,55,224,134,41,1,0,32,38,137,2,0,0,0,224,91,41,1,0,32,
38,137,2,0,0,0,4,0,55,224,63,41,1,0,32,38,137,2,0,0,0,79,4,190,
0,0,6,160,196,41,1,0,32,38,137,2,0,0,0,0,0,6,136,159,190,138,159,190,
128,223,41,1,0,32,204,136,2,0,0,0,96,229,33,16,68,160,65,6,32,0,48,32,
130,117,2,0,0,0,32,231,33,1,207,179,144,192,96,1,36,160,1,72,120,0,5,120,
136,223,41,9,0,32,90,106,2,0,0,0,128,133,251,139,0,120,1,12,178,2,4,32,
130,143,80,102,0,50,3,0,60,1,0,224,8,0,50,244,255,55,2,8,32,130,143,80,
103,0,50,2,12,32,130,143,80,238,255,58,115,0,55,139,0,120,1,15,178,4,0,50,
0,64,235,240,143,80,82,0,58,31,0,55,226,79,80,16,0,50,5,0,62,0,4,224,
119,0,50,225,79,80,73,0,58,5,0,55,227,79,80,11,0,50,231,79,80,68,0,62,
11,0,55,8,0,190,204,136,2,0,0,0,16,192,179,63,0,55,26,4,161,16,192,179,
60,0,55,26,4,160,16,192,179,57,0,55,99,64,96,0,64,64,145,207,179,154,192,96,
0,205,112,16,192,179,50,0,55,11,56,163,19,0,50,66,88,222,97,0,96,82,0,32,
1,64,32,62,118,2,0,0,0,11,0,209,97,0,96,82,0,32,1,8,32,62,118,2,
0,0,0,97,128,101,82,0,32,1,4,32,62,118,2,0,0,0,16,192,179,29,0,55,
11,40,163,5,0,50,33,3,32,64,6,32,200,91,2,0,0,0,4,0,55,128,0,235,
1,0,120,200,91,2,0,0,0,11,24,163,3,0,50,192,2,169,194,2,168,2,0,55,
192,2,169,194,2,169,11,8,163,4,0,50,201,98,168,203,98,169,16,192,179,5,0,55,
203,98,169,201,98,169,16,192,179,1,0,55,0,64,235,98,106,2,0,0,0,146,255,55,
26,52,160,240,3,178,246,254,7,26,52,161,16,192,179,98,106,2,0,0,0,138,255,55,
26,52,160,240,3,178,238,254,7,26,52,161,90,106,2,0,0,0,200,5,221,90,106,2,
0,0,0,0,128,251,11,0,112,229,254,7,16,192,179,98,106,2,0,0,0,122,255,55,
26,52,160,240,3,178,222,254,7,26,52,161,16,192,179,98,106,2,0,0,0,175,254,7,
128,5,120,72,88,222,98,106,2,0,0,0,11,64,120,98,106,2,0,0,0,106,255,55,
79,5,190,79,4,190,0,0,6,2,0,250,128,128,235,241,191,159,17,192,179,127,185,151,
2,0,224,21,0,50,82,22,128,72,17,222,1,65,97,128,64,235,128,79,81,6,0,58,
14,0,55,81,22,128,200,8,222,225,192,96,128,207,80,8,0,50,255,184,151,129,0,233,
241,191,159,127,185,151,2,0,224,245,255,58,17,192,179,1,0,55,128,64,235,1,64,120,
0,128,250,0,0,6,136,31,120,0,64,235,222,255,7,128,64,120,32,0,32,1,4,224,
10,0,58,88,22,128,73,68,222,97,68,100,16,192,179,213,255,7,128,64,120,32,0,32,
1,4,224,1,0,58,8,128,251,79,4,120,0,0,6,136,159,190,138,159,190,0,5,120,
16,0,32,203,34,171,42,0,58,16,192,179,198,255,7,128,64,120,64,0,32,1,4,224,
36,0,58,137,192,179,128,69,235,11,68,120,8,132,251,8,4,209,219,255,7,225,15,80,
3,0,50,226,15,80,3,0,58,24,0,55,8,116,160,11,4,162,137,68,233,244,255,58,
209,255,7,128,0,120,64,0,32,226,143,80,16,0,50,32,0,32,129,207,85,13,0,50,
201,255,7,128,0,120,64,0,32,226,143,80,8,0,50,201,2,169,48,0,32,225,143,80,
4,0,58,8,77,120,0,0,235,1,0,55,64,0,32,79,5,190,79,4,190,0,0,6,
136,31,120,0,68,120,16,192,179,151,255,7,128,64,120,16,192,179,1,4,224,8,0,58,
97,4,100,73,68,221,64,22,128,0,144,161,0,0,116,64,22,136,0,64,235,139,255,7,
79,4,120,0,0,6,136,159,190,138,159,190,140,31,120,0,6,120,201,34,168,201,2,168,
0,196,235,249,15,32,10,16,32,91,0,32,81,22,128,1,0,101,7,0,58,12,0,120,
172,255,7,128,4,120,9,0,224,7,0,50,228,143,84,5,0,50,11,0,120,104,105,2,
0,0,0,8,68,233,241,255,58,201,2,169,9,0,120,79,6,120,79,5,190,79,4,190,
0,0,6,136,159,190,138,159,190,0,69,120,201,2,169,203,2,169,192,3,32,104,105,2,
0,0,0,203,34,169,201,34,169,201,2,168,16,0,32,104,105,2,0,0,0,0,64,235,
90,255,7,128,64,120,32,0,32,1,4,224,56,0,58,0,4,235,128,69,235,25,0,32,
10,133,251,8,72,221,10,0,96,225,15,80,6,0,58,16,192,179,177,255,7,0,4,224,
41,0,58,11,4,162,4,0,55,0,64,235,171,255,7,0,4,224,37,0,58,8,4,232,
232,15,84,239,255,58,11,4,162,97,128,101,163,255,7,128,64,120,32,0,32,1,4,224,
28,0,58,201,34,168,203,34,169,16,192,179,54,255,7,128,64,120,32,0,32,1,4,224,
20,0,58,80,255,7,128,0,120,32,0,32,128,143,80,15,0,50,75,255,7,128,0,120,
32,0,32,128,143,80,10,0,50,201,2,169,203,2,169,1,240,167,129,0,234,1,0,234,
79,0,222,3,0,55,32,0,32,1,0,55,32,0,32,79,5,190,79,4,190,0,0,6,
226,15,80,18,0,50,5,0,62,0,0,224,39,0,50,225,15,80,33,0,58,7,0,55,
228,15,80,20,0,50,14,0,57,241,15,32,129,15,80,26,0,58,20,0,55,96,161,41,
1,0,32,38,137,2,0,0,0,24,0,55,96,183,41,1,0,32,38,137,2,0,0,0,
19,0,55,32,162,41,1,0,32,38,137,2,0,0,0,14,0,55,128,208,41,1,0,32,
204,136,2,0,0,0,9,0,55,64,222,41,1,0,32,38,137,2,0,0,0,4,0,55,
0,184,41,1,0,32,38,137,2,0,0,0,0,0,6,96,229,33,16,68,160,0,0,6,
201,34,168,201,2,169,203,2,169,203,34,169,0,0,6,2,0,250,15,128,233,90,255,7,
199,255,7,239,248,151,1,128,251,0,128,250,0,0,6,115,255,7,0,0,224,3,0,50,
225,15,80,11,0,58,5,0,55,64,219,41,1,0,32,204,136,2,0,0,0,9,0,55,
128,222,41,1,0,32,204,136,2,0,0,0,4,0,55,128,208,41,1,0,32,204,136,2,
0,0,0,0,16,32,0,0,6,2,0,250,136,31,120,0,0,224,3,0,50,225,15,80,
36,0,58,5,0,55,160,60,41,1,0,32,38,137,2,0,0,0,34,0,55,32,111,41,
1,0,32,38,137,2,0,0,0,224,159,41,1,0,32,38,137,2,0,0,0,8,194,179,
100,128,87,40,255,7,0,0,224,7,0,58,207,248,151,1,128,251,58,109,2,0,0,0,
8,64,120,98,106,2,0,0,0,174,105,2,0,0,0,0,4,224,241,255,50,90,106,2,
0,0,0,96,49,41,182,108,2,0,0,0,4,0,55,224,63,41,1,0,32,38,137,2,
0,0,0,79,4,120,0,128,250,0,0,6,2,32,32,227,79,80,1,0,58,2,8,32,
114,243,136,227,143,80,1,0,54,49,0,32,129,128,64,192,47,41,128,128,64,129,243,136,
0,0,6,98,229,33,18,65,120,112,65,97,5,0,58,204,34,183,0,128,234,200,2,182,
64,22,136,1,0,55,200,34,183,1,0,224,3,0,50,1,0,120,104,105,2,0,0,0,
0,0,6,0,129,234,99,22,128,2,128,97,96,22,136,64,22,128,2,1,96,66,22,136,
1,0,224,3,0,50,1,0,120,104,105,2,0,0,0,0,0,6,136,159,190,138,31,120,
0,5,120,10,0,224,15,0,50,0,4,235,9,16,32,128,243,128,144,64,144,129,128,251,
9,0,120,215,255,7,128,243,128,144,64,144,129,128,251,9,0,120,226,255,7,8,4,232,
136,15,85,243,255,62,79,5,120,79,4,190,0,0,6,128,243,128,144,64,144,129,128,251,
0,48,32,215,255,7,128,243,128,144,64,144,129,128,251,0,16,32,194,255,7,128,243,128,
144,64,144,129,128,251,0,32,32,189,255,7,0,0,6,136,31,120,0,4,120,128,243,128,
144,64,144,129,128,251,0,48,32,181,255,7,225,15,84,10,0,58,128,243,128,144,64,144,
129,128,251,0,32,32,174,255,7,128,243,128,144,64,144,129,128,251,0,16,32,169,255,7,
16,192,179,203,2,171,18,0,50,203,34,171,16,0,50,128,243,128,144,64,144,129,128,251,
0,32,32,175,255,7,128,243,128,144,64,144,129,128,251,0,16,32,170,255,7,128,243,128,
144,64,144,129,128,251,0,32,32,149,255,7,0,64,235,79,4,120,0,0,6,0,4,224,
8,0,58,129,128,234,99,22,128,1,128,97,96,22,136,64,22,128,129,0,96,65,22,136,
13,0,55,96,229,33,16,64,120,112,64,96,7,0,58,1,0,120,204,34,183,129,128,234,
67,22,128,129,128,97,65,22,136,2,0,55,1,0,120,200,34,183,2,0,224,3,0,50,
2,0,120,104,105,2,0,0,0,0,0,6,129,243,128,17,129,251,1,4,32,223,255,7,
0,0,6,129,243,128,17,129,251,1,16,32,218,255,7,0,0,6,129,243,128,17,129,251,
1,32,32,213,255,7,0,0,6,129,243,128,17,129,251,1,32,32,208,255,7,128,243,128,
144,64,144,129,128,251,0,16,32,93,255,7,128,243,128,144,64,144,129,128,251,0,16,32,
104,255,7,0,0,6,136,159,190,138,159,190,140,31,120,81,229,33,145,64,120,1,129,251,
2,1,233,26,0,32,2,85,221,1,4,224,28,0,50,0,4,120,128,4,235,11,32,32,
92,229,33,128,243,128,16,129,251,10,0,100,0,240,167,0,0,234,0,0,234,79,0,222,
139,0,120,173,255,7,128,243,128,144,64,144,129,128,251,0,16,32,58,255,7,128,243,128,
144,64,144,129,128,251,0,16,32,69,255,7,8,4,68,137,4,232,28,128,251,137,15,80,
232,255,62,79,6,120,79,5,190,79,4,190,0,0,6,200,34,183,0,0,0,0,0,0,
0,0,0,202,2,182,0,240,167,0,0,234,0,0,234,79,0,222,0,0,6,112,243,128,
244,255,7,0,0,6,136,31,120,112,243,128,240,255,7,128,243,128,144,64,144,129,128,251,
0,16,32,23,255,7,112,243,128,233,255,7,0,68,120,128,243,128,144,64,144,129,128,251,
0,16,32,31,255,7,8,64,120,79,4,120,0,0,6,136,159,190,138,159,190,0,32,32,
220,255,7,0,4,235,80,229,33,16,4,224,22,0,50,136,4,120,10,16,32,128,5,120,
128,243,128,144,64,144,129,128,251,10,0,120,252,254,7,8,4,68,0,32,32,205,255,7,
0,128,251,0,4,116,128,243,128,144,64,144,129,128,251,10,0,120,2,255,7,137,4,232,
27,128,251,137,15,80,237,255,62,8,0,120,79,5,190,79,4,190,0,0,6,136,159,190,
138,159,190,140,159,190,81,229,33,145,64,120,1,129,251,2,1,233,27,0,32,130,93,221,
128,4,235,1,4,224,33,0,50,0,4,120,9,5,120,12,32,32,93,229,33,128,243,128,
16,129,251,11,0,100,0,240,167,0,0,234,0,0,234,79,0,222,140,0,120,63,255,7,
128,243,128,144,64,144,129,128,251,0,16,32,204,254,7,137,132,68,0,8,32,157,255,7,
0,128,251,128,132,116,8,4,68,128,243,128,144,64,144,129,128,251,0,16,32,209,254,7,
10,5,232,29,128,251,138,15,80,227,255,62,9,0,120,79,6,190,79,5,190,79,4,190,
0,0,6,129,79,80,5,0,58,128,217,41,1,0,32,38,137,2,0,0,0,6,0,55,
192,217,41,1,0,32,38,137,2,0,0,0,96,231,33,16,72,232,0,0,6,136,31,120,
0,68,120,128,216,41,1,0,32,204,136,2,0,0,0,80,22,128,70,0,221,79,0,222,
136,64,120,231,255,7,192,216,41,1,0,32,204,136,2,0,0,0,80,22,128,71,0,221,
79,0,222,136,64,120,222,255,7,0,217,41,1,0,32,204,136,2,0,0,0,80,22,128,
72,0,221,79,0,222,136,64,120,213,255,7,0,226,41,1,0,32,204,136,2,0,0,0,
80,22,128,73,0,221,79,0,222,136,64,120,204,255,7,64,217,41,1,0,32,204,136,2,
0,0,0,80,22,128,73,0,221,79,0,222,136,64,120,195,255,7,79,4,120,0,0,6,
136,159,190,138,31,120,0,4,190,2,5,120,128,194,179,98,106,2,0,0,0,8,0,120,
4,105,2,0,0,0,0,4,120,8,0,120,98,107,2,0,0,0,144,194,179,98,106,2,
0,0,0,0,0,235,137,15,84,4,0,54,16,0,32,138,15,84,1,0,57,0,0,235,
17,192,179,167,255,7,79,5,120,79,4,190,0,0,6,136,159,190,138,31,120,0,69,120,
1,68,120,96,231,33,0,72,235,10,4,224,3,0,58,32,229,33,16,0,160,12,0,55,
8,4,224,10,0,50,128,251,40,1,0,32,38,137,2,0,0,0,32,156,41,1,0,32,
38,137,2,0,0,0,90,106,2,0,0,0,128,214,41,1,0,32,38,137,2,0,0,0,
203,66,168,201,66,169,192,214,41,1,0,32,204,136,2,0,0,0,80,22,128,69,0,221,
79,0,222,17,192,179,126,255,7,203,66,169,201,66,168,194,34,168,192,34,169,128,205,41,
1,0,32,204,136,2,0,0,0,194,2,213,97,0,96,17,192,179,114,255,7,194,34,169,
201,98,168,203,98,169,224,205,41,1,0,32,204,136,2,0,0,0,80,22,128,68,0,221,
79,0,222,17,192,179,102,255,7,203,98,169,201,98,169,64,206,41,1,0,32,204,136,2,
0,0,0,80,22,128,68,0,221,79,0,222,128,64,235,91,255,7,192,2,169,194,2,168,
32,0,32,94,105,2,0,0,0,0,215,41,1,0,32,204,136,2,0,0,0,16,22,128,
97,0,96,17,192,179,78,255,7,32,177,41,1,0,32,38,137,2,0,0,0,33,227,168,
224,225,41,1,0,32,204,136,2,0,0,0,114,53,32,113,43,32,144,0,32,128,255,7,
64,215,41,1,0,32,204,136,2,0,0,0,114,53,32,113,43,32,144,0,32,120,255,7,
128,215,41,1,0,32,204,136,2,0,0,0,114,53,32,113,43,32,144,0,32,112,255,7,
8,4,224,25,0,50,192,215,41,1,0,32,204,136,2,0,0,0,114,53,32,113,43,32,
176,0,32,102,255,7,0,216,41,1,0,32,204,136,2,0,0,0,2,37,32,1,27,32,
160,0,32,94,255,7,64,216,41,1,0,32,204,136,2,0,0,0,2,37,32,1,27,32,
192,0,32,86,255,7,8,0,55,0,216,41,1,0,32,204,136,2,0,0,0,2,37,32,
1,27,32,160,0,32,77,255,7,33,227,169,160,206,41,1,0,32,38,137,2,0,0,0,
249,195,47,128,44,32,16,136,100,0,60,32,204,34,183,64,6,32,94,105,2,0,0,0,
16,192,179,13,255,7,32,192,41,1,0,32,38,137,2,0,0,0,192,44,32,16,136,100,
8,4,224,21,0,50,201,98,168,203,98,169,64,6,32,94,105,2,0,0,0,0,64,235,
254,254,7,160,192,41,1,0,32,38,137,2,0,0,0,0,60,32,200,34,183,192,66,168,
194,66,169,64,6,32,94,105,2,0,0,0,16,192,179,241,254,7,5,0,55,64,6,32,
94,105,2,0,0,0,0,64,235,235,254,7,10,4,224,20,0,50,8,4,224,18,0,50,
192,2,169,194,2,168,192,34,169,194,34,168,32,89,41,1,0,32,38,137,2,0,0,0,
224,159,41,1,0,32,38,137,2,0,0,0,90,106,2,0,0,0,192,34,169,194,34,169,
192,2,169,194,2,169,200,104,2,0,0,0,0,207,41,1,0,32,204,136,2,0,0,0,
104,231,33,24,64,120,62,107,2,0,0,0,96,207,41,1,0,32,38,137,2,0,0,0,
32,229,33,16,0,161,24,64,120,79,5,120,79,4,190,0,0,6,64,254,40,1,0,32,
38,137,2,0,0,0,0,64,235,120,118,2,0,0,0,240,15,32,104,105,2,0,0,0,
16,192,179,120,118,2,0,0,0,16,0,32,156,117,2,0,0,0,0,64,235,120,118,2,
0,0,0,0,0,6,4,0,250,136,159,190,138,159,190,140,31,120,32,38,41,1,0,32,
204,136,2,0,0,0,111,132,87,107,133,87,107,229,33,12,194,179,74,119,2,0,0,0,
0,65,120,8,4,232,136,4,120,0,76,120,27,64,120,232,64,96,4,0,50,2,64,120,
52,105,2,0,0,0,0,76,120,25,64,120,166,106,2,0,0,0,12,64,120,98,106,2,
0,0,0,138,15,84,235,255,58,64,31,41,182,108,2,0,0,0,32,197,41,1,0,32,
204,136,2,0,0,0,175,240,151,1,128,251,68,0,222,233,15,80,15,0,50,233,79,80,
3,0,62,232,15,80,26,0,58,5,0,55,234,15,80,13,0,50,239,15,80,21,0,58,
15,0,55,32,211,41,1,0,32,204,136,2,0,0,0,19,0,55,192,210,41,1,0,32,
204,136,2,0,0,0,14,0,55,96,210,41,1,0,32,204,136,2,0,0,0,9,0,55,
0,224,41,1,0,32,204,136,2,0,0,0,4,0,55,128,211,41,1,0,32,204,136,2,
0,0,0,64,31,41,182,108,2,0,0,0,160,197,41,1,0,32,204,136,2,0,0,0,
63,240,151,0,4,224,5,0,53,224,211,41,1,0,32,204,136,2,0,0,0,4,0,55,
160,163,41,1,0,32,204,136,2,0,0,0,64,31,41,182,108,2,0,0,0,128,186,41,
1,0,32,204,136,2,0,0,0,63,240,151,67,0,222,111,0,96,0,4,224,3,0,50,
239,79,80,11,0,58,5,0,55,32,187,41,1,0,32,204,136,2,0,0,0,12,0,55,
0,224,41,1,0,32,204,136,2,0,0,0,7,0,55,0,128,251,66,0,221,97,31,41,
0,128,64,16,0,190,88,107,2,0,0,0,192,121,41,1,0,32,204,136,2,0,0,0,
63,240,151,103,0,96,66,0,221,97,33,41,97,64,120,62,107,2,0,0,0,64,31,41,
182,108,2,0,0,0,79,6,120,79,5,190,79,4,190,0,128,250,0,0,6,74,119,2,
0,0,0,0,0,6,180,118,2,0,0,0,0,16,32,0,0,6,0,0,235,234,117,2,
0,0,0,64,212,41,1,0,32,204,136,2,0,0,0,32,182,41,1,0,32,38,137,2,
0,0,0,0,0,6,202,117,2,0,0,0,160,212,41,1,0,32,204,136,2,0,0,0,
192,182,41,1,0,32,38,137,2,0,0,0,0,0,6,96,164,41,1,0,32,204,136,2,
0,0,0,64,229,33,16,64,120,62,107,2,0,0,0,0,194,179,98,106,2,0,0,0,
96,229,33,16,128,251,68,0,222,97,64,96,62,107,2,0,0,0,0,194,179,98,106,2,
0,0,0,64,227,41,1,0,32,38,137,2,0,0,0,0,0,6,136,31,120,36,28,2,
0,0,0,158,26,2,0,0,0,0,4,120,36,28,2,0,0,0,158,26,2,0,0,0,
136,0,233,227,143,80,3,0,62,128,0,233,225,143,80,29,0,54,128,11,41,1,0,32,
38,137,2,0,0,0,128,1,235,66,0,32,17,0,32,1,0,120,150,28,2,0,0,0,
65,229,33,128,72,233,0,239,40,1,0,32,38,137,2,0,0,0,128,1,235,34,0,32,
17,0,32,1,0,120,150,28,2,0,0,0,97,229,33,97,64,96,68,0,221,243,198,179,
17,193,97,128,72,113,13,0,55,65,229,33,136,72,233,0,0,162,0,240,167,0,0,234,
0,0,233,97,229,33,79,0,222,68,0,221,17,65,120,2,68,161,128,72,113,173,255,7,
79,4,120,0,0,6,203,2,169,203,34,169,201,34,168,201,2,169,64,229,33,144,128,251,
32,192,179,72,117,2,0,0,0,0,0,6,225,15,80,9,0,50,3,0,57,226,15,80,
9,0,58,6,0,55,96,16,41,1,0,32,204,136,2,0,0,0,7,0,55,206,254,7,
225,254,7,4,0,55,224,63,41,1,0,32,38,137,2,0,0,0,0,0,6,32,196,41,
1,0,32,38,137,2,0,0,0,0,0,6,212,71,2,0,0,0,202,194,168,202,194,169,
0,0,6,136,31,120,68,12,221,8,4,160,225,79,80,1,0,58,8,20,160,8,64,120,
243,255,7,8,52,160,8,64,120,240,255,7,8,64,120,0,52,161,237,255,7,79,4,120,
0,0,6,136,159,190,128,68,120,1,68,120,136,128,251,196,8,222,9,64,120,233,255,7,
239,64,100,9,64,120,230,255,7,79,4,190,0,0,6,2,194,179,130,64,64,0,64,235,
240,255,7,240,0,32,94,105,2,0,0,0,129,192,179,0,64,235,234,255,7,240,0,32,
94,105,2,0,0,0,17,192,179,0,64,235,228,255,7,240,0,32,94,105,2,0,0,0,
225,192,179,0,64,235,222,255,7,240,0,32,94,105,2,0,0,0,0,0,6,0,64,235,
195,255,7,240,0,32,94,105,2,0,0,0,49,192,179,0,64,235,194,255,7,80,0,32,
94,105,2,0,0,0,49,192,179,0,64,235,188,255,7,0,10,32,104,105,2,0,0,0,
49,192,179,0,64,235,182,255,7,0,10,32,104,105,2,0,0,0,33,192,179,0,64,235,
176,255,7,0,10,32,104,105,2,0,0,0,0,0,6,128,0,120,192,243,128,0,0,209,
97,64,96,183,255,7,0,16,32,0,0,6,128,231,33,16,20,161,128,140,41,1,0,32,
38,137,2,0,0,0,0,0,6,128,231,33,16,20,160,96,141,41,1,0,32,38,137,2,
0,0,0,0,0,6,96,229,33,16,68,160,0,0,6,128,231,33,16,20,160,0,254,47,
66,53,128,130,0,96,113,0,179,65,53,136,242,15,46,65,54,128,1,1,97,1,112,32,
130,128,112,65,54,136,200,6,182,0,48,160,64,54,136,202,194,169,200,194,169,201,2,169,
200,226,168,201,34,169,208,19,32,16,18,136,68,34,239,64,34,239,65,226,168,160,103,41,
1,0,32,38,137,2,0,0,0,0,0,6,136,159,190,138,159,190,140,31,120,0,4,120,
224,235,128,0,0,232,0,128,251,224,235,136,36,28,2,0,0,0,158,26,2,0,0,0,
0,5,120,96,229,33,16,116,161,227,15,84,48,0,50,3,0,62,8,0,224,8,0,50,
12,0,55,230,15,84,62,0,50,231,15,84,84,0,50,228,15,84,106,0,58,48,0,55,
128,218,40,1,0,32,38,137,2,0,0,0,117,0,55,0,214,41,1,0,32,38,137,2,
0,0,0,133,255,7,225,15,84,110,0,50,10,5,233,225,15,85,17,0,54,128,52,41,
1,0,32,38,137,2,0,0,0,128,1,235,34,0,32,17,0,32,2,0,120,150,28,2,
0,0,0,225,15,80,3,0,58,0,64,235,88,255,7,2,0,55,128,192,179,85,255,7,
0,218,41,1,0,32,38,137,2,0,0,0,85,0,55,17,192,179,0,64,235,65,255,7,
240,0,32,94,105,2,0,0,0,64,218,41,1,0,32,38,137,2,0,0,0,74,0,55,
138,64,120,1,116,160,0,64,235,53,255,7,32,193,41,1,0,32,38,137,2,0,0,0,
65,0,55,17,192,179,0,64,235,45,255,7,240,0,32,94,105,2,0,0,0,10,0,224,
45,0,50,45,0,60,55,0,55,8,4,232,139,15,84,1,0,54,12,4,120,136,64,120,
16,192,179,31,255,7,8,64,120,98,106,2,0,0,0,137,4,232,137,15,85,243,255,60,
41,0,55,17,192,179,0,64,235,21,255,7,240,0,32,94,105,2,0,0,0,10,0,224,
27,0,50,27,0,60,31,0,55,8,4,232,139,15,84,1,0,54,12,4,120,136,64,120,
16,192,179,7,255,7,8,64,120,98,106,2,0,0,0,137,4,232,137,15,85,243,255,60,
17,0,55,224,63,41,1,0,32,38,137,2,0,0,0,12,0,55,10,5,32,128,4,235,
8,3,32,155,3,32,8,6,120,211,255,55,10,5,32,128,4,235,24,2,32,251,7,32,
8,6,120,229,255,55,79,6,120,79,5,190,79,4,190,0,0,6,32,196,41,1,0,32,
38,137,2,0,0,0,0,0,6,206,29,0,94,0,0,2,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,240,0,0,0,106,30,0,4,0,0,
0,0,0,60,30,0,12,0,0,0,0,0,2,24,0,104,2,0,0,0,0,110,30,0,
4,0,0,0,0,0,72,30,0,12,0,0,2,0,0,2,8,0,0,0,0,0,0,0,
0,0,0,84,30,0,10,0,0,0,0,0,2,8,0,0,16,0,0,0,0,114,30,0,
2,0,0,0,0,0,116,30,0,2,0,0,0,0,0,118,30,0,2,0,0,0,0,0,
44,30,0,16,0,0,0,0,0,94,30,0,6,0,0,0,0,0,120,30,0,2,0,0,
0,0,0,122,30,0,2,0,0,0,0,0,124,30,0,2,0,0,0,0,0,126,30,0,
2,0,0,0,0,0,128,30,0,2,0,0,0,0,0,122,28,0,84,1,0,0,0,0,
106,26,0,16,2,0,2,0,0,128,138,128,138,128,138,128,138,112,138,96,138,128,138,128,
138,128,138,128,138,96,138,128,138,80,138,80,138,66,138,238,137,238,137,238,137,56,138,48,
138,72,105,90,0,0,0,0,0,146,101,146,101,128,138,128,138,14,101,6,101,128,138,128,
138,58,101,44,101,36,101,26,101,20,101,80,138,214,101,72,101,130,101,238,137,136,101,48,
138,49,45,87,73,82,69,0,0,92,80,92,80,112,80,112,80,204,76,132,76,128,138,128,
138,128,138,128,138,96,138,128,138,80,138,130,80,92,79,80,77,6,79,86,79,8,81,212,
76,85,65,82,84,0,0,0,0,90,86,90,86,170,86,170,86,244,85,188,85,128,138,128,
138,128,138,128,138,96,138,128,138,80,138,80,138,244,87,4,87,196,87,228,87,4,89,222,
86,73,50,67,0,0,0,0,0,142,67,136,67,148,67,148,67,146,73,164,73,128,138,128,
138,128,138,128,138,96,138,128,138,80,138,80,138,26,71,40,68,254,69,208,71,146,69,186,
67,83,80,73,0,0,0,0,0,236,123,236,123,4,124,4,124,228,123,222,123,40,138,32,
138,24,138,16,138,8,138,0,138,250,137,80,138,216,124,76,124,196,124,238,137,254,124,26,
124,50,87,73,82,69,0,0,0,246,133,226,133,10,134,10,134,210,133,202,133,40,138,32,
138,24,138,16,138,8,138,0,138,250,137,80,138,66,138,86,134,28,135,238,137,54,135,28,
134,51,87,73,82,69,0,0,0,128,138,128,138,128,138,128,138,186,116,170,116,128,138,128,
138,128,138,128,138,96,138,128,138,80,138,80,138,230,116,154,116,160,116,238,137,56,138,48,
138,75,69,89,66,0,0,0,0,200,125,200,125,214,125,214,125,186,125,96,138,128,138,128,
138,128,138,128,138,96,138,128,138,80,138,80,138,40,126,228,125,234,125,24,70,92,127,48,
138,76,67,68,0,0,0,0,0,130,111,130,111,172,111,172,111,18,112,190,111,128,138,128,
138,128,138,128,138,96,138,128,138,80,138,80,138,134,112,212,110,118,111,124,111,18,113,48,
138,80,73,67,0,0,0,0,0,128,138,128,138,128,138,128,138,28,148,24,148,128,138,128,
138,128,138,128,138,96,138,128,138,80,138,80,138,66,138,232,137,232,137,238,137,56,138,48,
138,68,73,79,0,0,0,0,0,100,30,0,6,0,0,0,0,0,0,8,0,2,0,0,
0,0,0,0,0,0,202,194,169,203,34,169,203,2,169,203,66,169,202,2,169,226,79,80,
6,0,58,206,194,168,207,2,168,207,34,168,207,66,168,202,2,168,8,0,55,206,194,169,
207,2,169,207,34,169,207,66,169,202,2,169,0,64,233,225,79,80,7,0,62,200,194,169,
201,34,169,201,2,169,201,66,169,200,2,169,200,226,168,6,0,55,200,194,168,201,34,168,
201,2,168,201,66,168,200,2,168,200,226,168,0,0,6,228,79,80,35,0,50,5,0,62,
225,79,80,8,0,50,226,79,80,42,0,58,13,0,55,232,79,80,33,0,50,240,79,80,
37,0,58,16,0,55,1,4,224,3,0,50,192,34,169,194,34,168,31,0,55,192,34,169,
194,34,169,28,0,55,1,4,224,3,0,50,192,2,169,194,2,168,23,0,55,192,2,169,
194,2,169,20,0,55,1,4,224,3,0,50,201,98,168,203,98,169,15,0,55,203,98,169,
201,98,169,12,0,55,225,128,96,80,22,128,0,0,161,0,128,112,80,22,136,6,0,55,
225,128,96,202,8,221,80,22,128,0,160,161,0,128,112,80,22,136,0,0,6,136,159,190,
138,159,190,140,159,190,142,31,120,74,242,128,16,0,32,240,243,136,192,222,41,1,0,32,
204,136,2,0,0,0,200,222,41,9,0,32,14,0,34,141,228,33,90,106,2,0,0,0,
227,79,80,20,0,50,4,0,62,225,79,80,52,0,50,55,0,62,157,0,55,230,79,80,
9,0,50,3,0,62,229,79,80,144,0,58,104,0,55,231,79,80,65,0,50,232,79,80,
139,0,58,52,0,55,8,0,190,204,136,2,0,0,0,232,255,55,48,192,179,0,77,120,
128,192,179,16,69,152,33,227,168,192,0,32,4,105,2,0,0,0,200,0,222,33,69,152,
48,69,152,176,0,32,4,105,2,0,0,0,200,0,222,65,69,152,80,69,152,160,0,32,
4,105,2,0,0,0,200,0,222,97,69,152,112,69,152,144,0,32,4,105,2,0,0,0,
200,0,222,1,77,152,16,77,152,33,227,169,161,0,32,10,0,120,28,109,2,0,0,0,
197,255,55,90,106,2,0,0,0,94,255,7,193,255,55,90,106,2,0,0,0,128,69,120,
90,106,2,0,0,0,128,64,120,11,64,120,120,255,7,184,255,55,90,106,2,0,0,0,
128,69,120,90,106,2,0,0,0,200,93,221,0,128,251,139,5,112,251,243,136,174,255,55,
90,106,2,0,0,0,128,69,120,225,207,85,4,0,58,48,0,32,174,109,2,0,0,0,
3,0,55,32,2,32,174,109,2,0,0,0,90,106,2,0,0,0,0,70,120,90,106,2,
0,0,0,161,202,179,129,79,86,5,0,58,81,197,179,129,79,80,2,0,58,139,133,251,
4,0,55,32,2,32,174,109,2,0,0,0,128,5,235,112,192,179,0,77,120,27,69,152,
33,0,32,10,0,120,28,109,2,0,0,0,137,255,55,90,106,2,0,0,0,0,70,120,
90,106,2,0,0,0,133,96,169,200,101,221,128,128,251,139,133,112,142,143,85,1,0,54,
142,5,120,81,192,179,1,77,120,28,69,152,32,69,152,49,0,32,10,0,120,28,109,2,
0,0,0,29,0,120,224,241,136,231,128,69,195,8,222,129,128,64,241,241,136,0,1,235,
2,242,136,67,131,32,131,0,64,17,242,136,34,242,136,50,242,136,149,96,168,243,243,128,
11,1,120,72,140,2,0,0,0,98,255,55,0,64,235,0,77,120,16,69,152,17,0,32,
10,0,120,28,109,2,0,0,0,90,255,55,79,7,120,79,6,190,79,5,190,79,4,190,
0,0,6,194,34,169,104,32,239,106,32,239,96,32,239,98,32,239,30,33,239,240,255,40,
172,32,182,28,33,239,0,100,32,208,8,136,0,0,33,96,241,136,0,0,235,112,241,136,
1,227,33,128,72,120,0,0,6,145,241,128,225,143,80,61,0,50,4,0,57,0,64,235,
226,143,80,162,0,58,66,0,55,226,79,80,16,0,50,5,0,62,0,4,224,10,0,50,
225,79,80,38,0,58,16,0,55,241,79,80,150,0,50,243,79,80,148,0,50,228,79,80,
31,0,58,24,0,55,214,255,7,16,192,179,143,0,55,65,0,32,64,44,41,28,109,2,
0,0,0,0,64,235,137,0,55,194,34,168,30,33,239,22,33,239,20,33,239,30,97,168,
134,96,169,150,96,169,240,255,40,172,0,182,0,192,160,96,5,136,16,0,32,112,241,136,
0,64,235,122,0,55,81,1,32,128,44,41,28,109,2,0,0,0,0,64,235,116,0,55,
65,227,33,128,72,120,65,192,179,144,227,33,1,72,120,160,227,33,0,72,235,16,0,32,
144,241,136,0,64,235,105,0,55,162,227,33,146,64,232,1,73,120,1,129,251,67,227,33,
128,113,121,0,64,235,146,227,33,146,207,80,95,0,57,64,227,33,16,64,120,17,200,179,
129,79,80,32,0,50,1,204,179,129,79,80,4,0,50,1,200,179,129,79,80,79,0,58,
40,0,55,0,227,33,0,72,235,80,227,33,16,128,251,0,72,163,1,0,50,98,0,168,
0,56,163,1,0,50,98,160,168,0,40,163,1,0,50,98,192,168,0,24,163,1,0,50,
98,224,168,0,8,163,1,0,50,99,0,168,127,0,96,57,0,50,17,192,179,0,227,33,
1,72,120,53,0,55,80,227,33,144,128,251,129,0,232,96,227,33,16,128,251,72,0,221,
0,128,64,66,0,221,96,241,136,1,0,33,129,15,80,41,0,54,1,0,120,96,241,136,
38,0,55,80,227,33,16,129,251,128,1,235,97,1,65,224,129,73,112,227,33,16,128,251,
128,0,235,192,0,221,0,0,32,2,1,64,131,129,72,96,227,33,16,130,251,128,2,235,
72,40,221,200,32,222,129,0,112,72,32,221,2,1,64,131,129,72,66,24,221,206,16,222,
129,0,112,66,16,221,146,1,32,3,0,32,6,6,2,0,0,0,18,0,32,3,0,32,
240,15,80,224,143,88,2,0,54,112,1,80,224,129,88,227,8,136,210,8,136,0,0,235,
144,241,136,0,64,235,1,0,55,0,64,235,0,0,6,136,31,120,112,241,128,225,15,80,
36,0,58,134,96,171,4,0,58,0,64,235,1,227,33,17,4,224,31,0,58,31,225,168,
135,128,169,104,241,128,8,0,224,28,0,50,0,1,235,1,0,33,83,22,128,198,25,222,
64,242,128,3,112,121,51,4,128,3,128,96,253,255,50,135,128,169,2,1,232,136,15,81,
245,255,57,98,32,239,30,33,239,64,242,128,8,0,64,112,248,151,98,106,2,0,0,0,
8,4,233,249,255,58,30,255,7,16,192,179,1,0,55,0,64,235,79,4,120,0,0,6,
98,32,239,30,33,239,247,255,55,0,124,32,200,34,183,19,255,7,32,192,179,35,255,7,
174,105,2,0,0,0,0,4,224,3,0,50,90,106,2,0,0,0,28,255,7,197,255,7,
247,255,55,136,31,120,1,68,120,97,229,33,97,64,96,70,0,221,17,65,120,2,100,161,
128,72,113,8,128,251,0,0,233,79,0,222,120,118,2,0,0,0,8,4,224,3,0,50,
240,194,179,98,106,2,0,0,0,79,4,120,0,0,6,240,15,32,142,119,2,0,0,0,
0,0,6,142,119,2,0,0,0,97,229,33,2,196,179,145,64,97,1,0,58,0,0,235,
0,0,6,64,231,33,144,64,120,1,4,162,16,192,179,219,255,7,160,198,41,1,0,32,
38,137,2,0,0,0,0,0,6,64,231,33,144,64,120,1,4,162,0,64,235,209,255,7,
160,198,41,1,0,32,38,137,2,0,0,0,0,0,6,64,231,33,144,64,120,0,64,235,
200,255,7,32,198,41,1,0,32,38,137,2,0,0,0,0,0,6,128,147,41,1,0,32,
204,136,2,0,0,0,64,229,33,16,64,120,62,107,2,0,0,0,0,194,179,98,106,2,
0,0,0,64,231,33,16,64,120,62,107,2,0,0,0,0,194,179,98,106,2,0,0,0,
96,229,33,16,128,251,68,0,222,97,64,96,62,107,2,0,0,0,64,227,41,1,0,32,
38,137,2,0,0,0,0,0,6,136,159,190,36,28,2,0,0,0,158,26,2,0,0,0,
0,4,120,36,28,2,0,0,0,158,26,2,0,0,0,128,4,120,36,28,2,0,0,0,
158,26,2,0,0,0,136,0,233,227,143,80,6,0,62,137,0,233,225,143,80,3,0,62,
128,0,233,225,143,80,50,0,54,128,11,41,1,0,32,38,137,2,0,0,0,128,1,235,
66,0,32,17,0,32,1,0,120,150,28,2,0,0,0,65,229,33,128,72,233,96,112,41,
1,0,32,38,137,2,0,0,0,128,1,235,34,0,32,17,0,32,2,0,120,150,28,2,
0,0,0,128,0,233,1,240,167,129,0,234,129,0,234,207,8,222,64,231,33,1,72,120,
0,239,40,1,0,32,38,137,2,0,0,0,128,1,235,34,0,32,17,0,32,1,0,120,
150,28,2,0,0,0,0,0,162,0,240,167,0,0,234,0,0,233,97,229,33,79,0,222,
68,0,221,243,198,179,17,193,97,128,72,113,20,0,55,65,229,33,136,72,233,9,0,162,
9,240,167,137,4,234,137,4,234,207,76,222,65,231,33,137,72,120,0,0,162,0,240,167,
0,0,234,0,0,233,97,229,33,79,0,222,68,0,221,17,65,120,2,68,161,128,72,113,
133,255,7,96,229,33,241,201,179,16,200,96,79,4,190,0,0,6,64,229,33,144,128,251,
48,192,179,72,117,2,0,0,0,201,34,169,201,2,169,200,226,168,64,231,33,16,64,120,
120,118,2,0,0,0,0,0,6,224,165,41,1,0,32,38,137,2,0,0,0,0,0,6,
136,31,120,0,4,120,16,192,179,98,106,2,0,0,0,90,106,2,0,0,0,0,76,120,
242,79,80,164,0,50,13,0,62,226,79,80,114,0,50,5,0,62,0,4,224,31,0,50,
225,79,80,176,0,58,101,0,55,227,79,80,111,0,50,240,79,80,171,0,58,118,0,55,
245,79,80,160,0,50,5,0,62,243,79,80,149,0,50,244,79,80,163,0,58,150,0,55,
17,192,179,146,198,179,130,79,80,159,0,50,97,201,179,129,79,80,3,0,50,246,79,80,
153,0,58,148,0,55,16,192,179,98,106,2,0,0,0,17,192,179,148,0,55,33,227,168,
144,0,32,4,105,2,0,0,0,72,0,222,98,106,2,0,0,0,144,0,32,4,105,2,
0,0,0,98,106,2,0,0,0,160,0,32,4,105,2,0,0,0,72,0,222,98,106,2,
0,0,0,160,0,32,4,105,2,0,0,0,98,106,2,0,0,0,80,0,32,4,105,2,
0,0,0,72,0,222,98,106,2,0,0,0,80,0,32,4,105,2,0,0,0,98,106,2,
0,0,0,64,0,32,4,105,2,0,0,0,72,0,222,98,106,2,0,0,0,64,0,32,
4,105,2,0,0,0,98,106,2,0,0,0,176,0,32,4,105,2,0,0,0,72,0,222,
98,106,2,0,0,0,176,0,32,4,105,2,0,0,0,98,106,2,0,0,0,192,0,32,
4,105,2,0,0,0,72,0,222,98,106,2,0,0,0,192,0,32,4,105,2,0,0,0,
98,106,2,0,0,0,33,227,169,16,192,179,98,106,2,0,0,0,17,192,179,75,0,55,
192,66,168,194,66,169,16,192,179,98,106,2,0,0,0,17,192,179,68,0,55,16,192,179,
98,106,2,0,0,0,17,192,179,63,0,55,16,22,128,66,0,222,97,0,96,98,106,2,
0,0,0,16,192,179,98,106,2,0,0,0,17,192,179,53,0,55,192,2,169,194,2,168,
33,227,168,32,0,32,94,105,2,0,0,0,160,0,32,4,105,2,0,0,0,1,27,32,
129,15,80,12,0,54,144,0,32,4,105,2,0,0,0,113,43,32,129,15,80,6,0,54,
192,2,169,194,2,169,16,192,179,98,106,2,0,0,0,5,0,55,192,2,169,194,2,169,
0,64,235,98,106,2,0,0,0,33,227,169,17,192,179,21,0,55,88,65,2,0,0,0,
17,192,179,17,0,55,0,65,2,0,0,0,17,192,179,13,0,55,16,65,2,0,0,0,
17,192,179,9,0,55,24,65,2,0,0,0,17,192,179,5,0,55,44,65,2,0,0,0,
17,192,179,1,0,55,128,64,235,1,64,120,79,4,120,0,0,6,136,159,190,138,159,190,
140,159,190,142,31,120,0,4,190,44,3,32,28,70,120,128,69,235,11,69,120,45,192,179,
30,192,179,64,72,222,1,0,32,50,224,183,225,79,85,7,0,50,3,0,57,226,79,85,
12,0,58,7,0,55,152,5,186,14,69,120,8,0,55,152,5,186,200,93,222,13,69,120,
4,0,55,152,133,186,98,4,68,224,132,76,0,69,235,11,4,224,4,0,50,11,64,120,
98,106,2,0,0,0,230,255,55,140,64,120,32,3,32,1,72,120,79,7,120,79,6,190,
79,5,190,79,4,190,0,0,6,210,255,7,128,49,41,182,108,2,0,0,0,0,0,6,
128,71,41,1,0,32,248,255,7,96,242,40,1,0,32,245,255,7,32,54,41,1,0,32,
242,255,7,224,43,41,1,0,32,239,255,7,224,130,41,1,0,32,236,255,7,32,106,41,
1,0,32,233,255,7,160,45,41,1,0,32,230,255,7,96,107,41,1,0,32,227,255,7,
32,34,41,1,0,32,224,255,7,224,131,41,1,0,32,221,255,7,0,73,41,1,0,32,
218,255,7,32,36,41,1,0,32,215,255,7,160,108,41,1,0,32,212,255,7,128,74,41,
1,0,32,209,255,7,224,109,41,1,0,32,206,255,7,192,55,41,1,0,32,203,255,7,
0,76,41,1,0,32,200,255,7,160,120,41,1,0,32,197,255,7,128,90,41,1,0,32,
194,255,7,96,47,41,1,0,32,191,255,7,96,57,41,1,0,32,188,255,7,0,14,41,
1,0,32,185,255,7,0,1,41,1,0,32,182,255,7,160,3,41,1,0,32,179,255,7,
0,0,6,136,31,120,128,176,41,1,0,32,129,255,7,90,106,2,0,0,0,0,68,120,
8,64,120,98,106,2,0,0,0,128,49,41,182,108,2,0,0,0,248,13,178,152,133,178,
8,0,233,79,0,222,79,4,120,0,0,6,96,229,33,16,116,161,0,0,6,129,192,179,
80,229,33,1,72,120,96,229,33,16,84,161,0,0,6,36,119,2,0,0,0,0,0,6,
16,0,32,156,117,2,0,0,0,0,0,6,30,119,2,0,0,0,0,128,251,0,0,6,
0,64,235,140,118,2,0,0,0,0,0,6,16,192,179,140,118,2,0,0,0,0,0,6,
0,64,235,130,118,2,0,0,0,0,0,6,16,192,179,130,118,2,0,0,0,0,0,6,
160,49,41,182,108,2,0,0,0,0,0,6,224,165,41,1,0,32,38,137,2,0,0,0,
0,0,6,128,77,41,1,0,32,38,137,2,0,0,0,96,229,33,16,116,160,0,0,6,
128,77,41,1,0,32,38,137,2,0,0,0,96,229,33,16,116,160,0,64,235,0,0,6,
128,77,41,1,0,32,38,137,2,0,0,0,96,229,33,16,116,160,0,0,235,0,0,6,
128,77,41,1,0,32,38,137,2,0,0,0,96,229,33,16,116,160,0,16,32,0,0,6,
128,77,41,1,0,32,38,137,2,0,0,0,96,229,33,16,116,160,0,0,6,4,0,250,
136,31,120,240,255,47,1,0,32,208,191,159,225,191,159,69,7,169,44,163,239,66,39,169,
95,184,151,239,184,151,97,1,80,224,129,88,210,191,159,227,191,159,224,15,80,224,143,88,
247,255,58,82,105,2,0,0,0,0,254,47,35,53,128,131,0,96,81,0,179,33,53,136,
196,6,182,48,0,179,32,54,136,136,192,179,240,228,33,8,72,120,0,229,33,0,72,235,
200,104,2,0,0,0,136,105,2,0,0,0,0,12,32,104,32,183,128,0,235,240,15,32,
60,105,2,0,0,0,80,242,136,33,0,32,240,15,32,60,105,2,0,0,0,96,242,136,
81,22,128,194,8,222,227,128,96,210,48,41,224,228,33,98,200,120,240,243,47,104,32,182,
32,229,33,16,0,161,80,229,33,8,72,120,222,54,2,0,0,0,50,96,239,192,48,41,
182,108,2,0,0,0,88,32,2,0,0,0,79,4,120,0,128,250,0,0,6,96,146,37,
17,20,35,224,63,137,241,63,137,179,255,7,150,33,2,0,0,0,0,0,235,0,0,6,
49,87,73,82,69,32,82,79,77,32,67,79,77,77,65,78,68,32,77,65,67,82,79,115,
58,13,10,32,53,49,46,82,69,65,68,32,82,79,77,32,40,48,120,51,51,41,32,42,
102,111,114,32,115,105,110,103,108,101,32,100,101,118,105,99,101,32,98,117,115,13,10,32,
56,53,46,77,65,84,67,72,32,82,79,77,32,40,48,120,53,53,41,32,42,102,111,108,
108,111,119,101,100,32,98,121,32,54,52,98,105,116,32,97,100,100,114,101,115,115,13,10,
32,50,48,52,46,83,75,73,80,32,82,79,77,32,40,48,120,67,67,41,32,42,102,111,
108,108,111,119,101,100,32,98,121,32,99,111,109,109,97,110,100,13,10,32,50,51,54,46,
65,76,65,82,77,32,83,69,65,82,67,72,32,40,48,120,69,67,41,13,10,32,50,52,
48,46,83,69,65,82,67,72,32,82,79,77,32,40,48,120,70,48,41,0,0,32,48,46,
77,97,99,114,111,32,109,101,110,117,13,10,32,49,46,83,110,105,102,102,32,67,83,32,
108,111,119,13,10,32,50,46,83,110,105,102,102,32,97,108,108,32,116,114,97,102,102,105,
99,13,10,49,48,46,83,101,116,32,99,108,111,99,107,32,105,100,108,101,32,108,111,119,
13,10,49,49,46,83,101,116,32,99,108,111,99,107,32,105,100,108,101,32,104,105,103,104,
13,10,49,50,46,83,101,116,32,101,100,103,101,32,105,100,108,101,32,116,111,32,97,99,
116,105,118,101,13,10,49,51,46,83,101,116,32,101,100,103,101,32,97,99,116,105,118,101,
32,116,111,32,105,100,108,101,13,10,49,52,46,83,97,109,112,108,101,32,112,104,97,115,
101,32,111,110,32,109,105,100,100,108,101,13,10,49,53,46,83,97,109,112,108,101,32,112,
104,97,115,101,32,111,110,32,101,110,100,0,136,159,190,138,159,190,140,31,120,128,2,235,
2,3,233,128,3,120,1,4,120,3,6,120,169,44,32,154,0,32,107,0,32,240,241,128,
228,130,66,0,40,230,5,0,120,64,30,227,254,255,53,55,1,120,183,1,120,2,128,253,
3,2,105,244,15,178,4,1,105,132,129,105,2,128,253,240,0,32,0,48,230,6,0,120,
128,0,120,12,128,9,0,0,0,25,128,161,2,129,211,25,80,173,131,129,211,25,88,173,
12,128,9,0,0,0,25,128,160,25,112,163,4,130,211,129,0,233,242,255,49,239,0,16,
1,34,222,4,28,120,67,0,222,0,0,232,70,62,180,149,128,171,8,0,50,32,242,128,
70,30,227,5,0,50,133,128,169,149,128,168,66,30,180,16,0,120,32,17,136,112,3,83,
206,255,49,79,6,120,79,5,190,79,4,190,188,109,4,0,0,0,225,128,96,201,8,221,
117,22,128,5,144,161,133,130,112,117,22,136,1,254,47,53,53,128,133,128,96,113,0,179,
49,53,136,245,15,46,65,54,128,129,130,98,1,80,32,133,128,112,65,54,136,192,17,136,
2,129,251,68,16,221,0,48,160,4,130,251,0,0,114,227,129,97,131,129,65,128,129,113,
131,17,136,78,17,221,146,17,136,0,0,6,49,226,168,51,66,168,135,192,169,201,34,169,
0,0,6,49,226,169,51,66,169,240,1,32,166,38,183,240,15,46,200,38,182,207,34,169,
201,34,168,0,0,6,2,32,32,147,17,128,131,0,97,253,255,58,0,128,251,160,17,136,
0,0,6,50,2,171,254,255,50,176,17,128,0,0,6,144,17,128,97,0,96,0,0,6,
83,101,116,32,115,112,101,101,100,58,13,10,32,49,46,32,32,51,48,75,72,122,13,10,
32,50,46,32,49,50,53,75,72,122,13,10,32,51,46,32,50,53,48,75,72,122,13,10,
32,52,46,32,32,32,49,77,72,122,13,10,32,53,46,32,32,53,48,75,72,122,13,10,
32,54,46,32,49,46,51,77,72,122,13,10,32,55,46,32,32,32,50,77,72,122,13,10,
32,56,46,32,50,46,54,77,72,122,13,10,32,57,46,32,51,46,50,77,72,122,13,10,
49,48,46,32,32,32,52,77,72,122,13,10,49,49,46,32,53,46,51,77,72,122,13,10,
49,50,46,32,32,32,56,77,72,122,0,0,32,48,46,77,97,99,114,111,32,109,101,110,
117,13,10,32,49,46,76,67,68,32,82,101,115,101,116,13,10,32,50,46,73,110,105,116,
32,76,67,68,13,10,32,51,46,67,108,101,97,114,32,76,67,68,13,10,32,52,46,67,
117,114,115,111,114,32,112,111,115,105,116,105,111,110,32,101,120,58,40,52,41,32,48,13,
10,32,54,46,87,114,105,116,101,32,116,101,115,116,32,110,117,109,98,101,114,115,32,101,
120,58,40,54,41,32,56,48,13,10,32,55,46,87,114,105,116,101,32,116,101,115,116,32,
99,104,97,114,97,99,116,101,114,115,32,101,120,58,40,55,41,32,56,48,0,83,101,116,
32,115,101,114,105,97,108,32,112,111,114,116,32,115,112,101,101,100,58,32,40,98,112,115,
41,13,10,32,49,46,32,51,48,48,13,10,32,50,46,32,49,50,48,48,13,10,32,51,
46,32,50,52,48,48,13,10,32,52,46,32,52,56,48,48,13,10,32,53,46,32,57,54,
48,48,13,10,32,54,46,32,49,57,50,48,48,13,10,32,55,46,32,51,56,52,48,48,
13,10,32,56,46,32,53,55,54,48,48,13,10,32,57,46,32,49,49,53,50,48,48,13,
10,49,48,46,32,66,82,71,32,114,97,119,32,118,97,108,117,101,0,0,0,32,48,46,
77,97,99,114,111,32,109,101,110,117,13,10,32,49,46,84,114,97,110,115,112,97,114,101,
110,116,32,98,114,105,100,103,101,13,10,32,50,46,76,105,118,101,32,109,111,110,105,116,
111,114,13,10,32,51,46,66,114,105,100,103,101,32,119,105,116,104,32,102,108,111,119,32,
99,111,110,116,114,111,108,10,13,32,52,46,65,117,116,111,32,66,97,117,100,32,68,101,
116,101,99,116,105,111,110,0,0,68,97,116,97,32,98,105,116,115,32,97,110,100,32,112,
97,114,105,116,121,58,13,10,32,49,46,32,56,44,32,78,79,78,69,32,42,100,101,102,
97,117,108,116,32,13,10,32,50,46,32,56,44,32,69,86,69,78,32,13,10,32,51,46,
32,56,44,32,79,68,68,32,13,10,32,52,46,32,57,44,32,78,79,78,69,0,0,0,
83,101,108,101,99,116,32,111,117,116,112,117,116,32,116,121,112,101,58,13,10,32,49,46,
32,79,112,101,110,32,100,114,97,105,110,32,40,72,61,72,105,45,90,44,32,76,61,71,
78,68,41,13,10,32,50,46,32,78,111,114,109,97,108,32,40,72,61,51,46,51,86,44,
32,76,61,71,78,68,41,0,0,32,45,45,45,45,45,45,45,45,45,45,45,45,45,45,
45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,
45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,45,
45,45,45,45,45,45,45,45,45,45,45,45,45,0,0,49,46,40,66,82,41,9,50,46,
40,82,68,41,9,51,46,40,79,82,41,9,52,46,40,89,87,41,9,53,46,40,71,78,
41,9,54,46,40,66,76,41,9,55,46,40,80,85,41,9,56,46,40,71,82,41,9,57,
46,40,87,84,41,9,48,46,40,66,108,107,41,0,0,79,117,116,112,117,116,32,99,108,
111,99,107,32,101,100,103,101,58,13,10,32,49,46,32,73,100,108,101,32,116,111,32,97,
99,116,105,118,101,13,10,32,50,46,32,65,99,116,105,118,101,32,116,111,32,105,100,108,
101,32,42,100,101,102,97,117,108,116,0,0,68,105,115,99,111,110,110,101,99,116,32,97,
110,121,32,100,101,118,105,99,101,115,13,10,67,111,110,110,101,99,116,32,40,86,112,117,
32,116,111,32,43,53,86,41,32,97,110,100,32,40,65,68,67,32,116,111,32,43,51,46,
51,86,41,0,0,0,73,83,79,32,55,56,49,54,45,51,32,65,84,82,32,40,82,69,
83,69,84,32,111,110,32,67,83,41,13,10,82,69,83,69,84,32,72,73,71,72,44,32,
67,76,79,67,75,32,84,73,67,75,44,32,82,69,83,69,84,32,76,79,87,0,0,0,
32,119,47,87,9,80,83,85,32,40,111,102,102,47,79,78,41,9,9,60,120,62,47,60,
120,61,32,62,47,60,48,62,9,85,115,101,114,109,97,99,114,111,32,120,47,97,115,115,
105,103,110,32,120,47,108,105,115,116,32,97,108,108,0,32,101,47,69,9,84,80,48,32,
40,105,110,112,117,116,45,108,111,119,43,82,69,65,68,47,111,117,116,112,117,116,45,108,
111,119,41,32,40,84,80,48,32,105,115,32,99,111,110,110,101,99,116,101,100,32,116,111,
32,86,80,85,41,0,83,101,116,32,115,112,101,101,100,58,13,10,32,49,46,32,83,116,
97,110,100,97,114,100,32,40,126,49,54,46,51,107,98,112,115,41,32,13,10,32,50,46,
32,79,118,101,114,100,114,105,118,101,32,40,126,49,54,48,107,112,115,41,0,84,111,32,
102,105,110,105,115,104,32,115,101,116,117,112,44,32,115,116,97,114,116,32,117,112,32,116,
104,101,32,112,111,119,101,114,32,115,117,112,112,108,105,101,115,32,119,105,116,104,32,99,
111,109,109,97,110,100,32,39,87,39,0,0,83,101,116,32,115,112,101,101,100,58,13,10,
32,49,46,32,126,53,75,72,122,13,10,32,50,46,32,126,53,48,75,72,122,13,10,32,
51,46,32,126,49,48,48,75,72,122,13,10,32,52,46,32,126,52,48,48,75,72,122,0,
32,118,9,83,104,111,119,32,118,111,108,116,115,47,115,116,97,116,101,115,9,9,46,9,
66,105,116,115,32,116,111,32,114,101,97,100,47,119,114,105,116,101,32,101,46,103,46,32,
48,120,53,53,46,50,0,0,0,32,48,46,77,97,99,114,111,32,109,101,110,117,13,10,
32,49,46,73,83,79,55,56,49,54,45,51,32,65,84,82,13,10,32,50,46,73,83,79,
55,56,49,54,45,51,32,112,97,114,115,101,32,111,110,108,121,0,42,42,32,66,97,117,
100,62,49,54,109,58,32,84,104,101,32,66,80,32,99,97,110,110,111,116,32,109,101,97,
115,117,114,101,32,97,98,111,118,101,32,49,54,48,48,48,48,48,48,44,32,68,111,110,
101,46,0,87,65,82,78,73,78,71,58,32,72,65,82,68,87,65,82,69,32,73,50,67,
32,105,115,32,98,114,111,107,101,110,32,111,110,32,116,104,105,115,32,80,73,67,33,32,
40,82,69,86,32,65,51,41,0,32,48,46,77,97,99,114,111,32,109,101,110,117,13,10,
32,49,46,55,98,105,116,32,97,100,100,114,101,115,115,32,115,101,97,114,99,104,13,10,
32,50,46,73,50,67,32,115,110,105,102,102,101,114,0,67,108,111,99,107,32,112,111,108,
97,114,105,116,121,58,13,10,32,49,46,32,73,100,108,101,32,108,111,119,32,42,100,101,
102,97,117,108,116,13,10,32,50,46,32,73,100,108,101,32,104,105,103,104,0,73,110,112,
117,116,32,115,97,109,112,108,101,32,112,104,97,115,101,58,13,10,32,49,46,32,77,105,
100,100,108,101,32,42,100,101,102,97,117,108,116,13,10,32,50,46,32,69,110,100,0,0,
82,101,99,101,105,118,101,32,112,111,108,97,114,105,116,121,58,13,10,32,49,46,32,73,
100,108,101,32,49,32,42,100,101,102,97,117,108,116,13,10,32,50,46,32,73,100,108,101,
32,48,0,83,101,97,114,99,104,105,110,103,32,73,50,67,32,97,100,100,114,101,115,115,
32,115,112,97,99,101,46,32,70,111,117,110,100,32,100,101,118,105,99,101,115,32,97,116,
58,0,0,32,97,47,65,47,64,9,65,85,88,80,73,78,32,40,108,111,119,47,72,73,
47,82,69,65,68,41,9,9,34,97,98,99,34,9,83,101,110,100,32,115,116,114,105,110,
103,0,0,32,100,47,68,9,77,101,97,115,117,114,101,32,65,68,67,32,40,111,110,99,
101,47,67,79,78,84,46,41,9,48,98,49,49,48,9,83,101,110,100,32,118,97,108,117,
101,0,0,73,83,79,32,55,56,49,54,45,51,32,114,101,112,108,121,32,40,117,115,101,
115,32,99,117,114,114,101,110,116,32,76,83,66,32,115,101,116,116,105,110,103,41,58,32,
0,0,0,83,101,116,32,115,112,101,101,100,58,13,10,32,49,46,32,49,48,48,75,72,
122,13,10,32,50,46,32,52,48,48,75,72,122,13,10,32,51,46,32,49,77,72,122,0,
68,101,118,105,99,101,32,73,68,115,32,97,114,101,32,97,118,97,105,108,97,98,108,101,
32,98,121,32,77,65,67,82,79,44,32,115,101,101,32,40,48,41,46,0,0,32,61,88,
47,124,88,9,67,111,110,118,101,114,116,115,32,88,47,114,101,118,101,114,115,101,32,88,
9,9,40,120,41,9,77,97,99,114,111,32,120,0,0,32,36,9,74,117,109,112,32,116,
111,32,98,111,111,116,108,111,97,100,101,114,9,9,123,9,83,116,97,114,116,32,119,105,
116,104,32,114,101,97,100,0,0,32,112,47,80,9,80,117,108,108,117,112,32,114,101,115,
105,115,116,111,114,115,32,40,111,102,102,47,79,78,41,9,33,9,66,105,116,32,114,101,
97,100,0,78,111,32,100,101,118,105,99,101,44,32,116,114,121,32,40,65,76,65,82,77,
41,32,83,69,65,82,67,72,32,109,97,99,114,111,32,102,105,114,115,116,0,73,50,67,
32,109,111,100,101,58,13,10,32,49,46,32,83,111,102,116,119,97,114,101,13,10,32,50,
46,32,72,97,114,100,119,97,114,101,0,0,68,105,115,112,108,97,121,32,108,105,110,101,
115,58,13,10,32,49,46,32,49,32,13,10,32,50,46,32,77,117,108,116,105,112,108,101,
0,0,0,32,63,9,84,104,105,115,32,104,101,108,112,9,9,9,40,48,41,9,76,105,
115,116,32,99,117,114,114,101,110,116,32,109,97,99,114,111,115,0,32,105,9,86,101,114,
115,105,111,110,105,110,102,111,47,115,116,97,116,117,115,105,110,102,111,9,9,94,9,67,
76,75,32,116,105,99,107,0,0,32,115,9,83,99,114,105,112,116,32,101,110,103,105,110,
101,9,9,9,58,9,82,101,112,101,97,116,32,101,46,103,46,32,114,58,49,48,0,0,
32,42,110,101,120,116,32,99,108,111,99,107,32,40,94,41,32,119,105,108,108,32,117,115,
101,32,116,104,105,115,32,118,97,108,117,101,0,0,0,32,48,46,32,77,97,99,114,111,
32,109,101,110,117,13,10,32,49,46,32,76,105,118,101,32,105,110,112,117,116,32,109,111,
110,105,116,111,114,0,70,114,101,113,117,101,110,99,105,101,115,32,60,32,49,72,122,32,
97,114,101,32,110,111,116,32,115,117,112,112,111,114,116,101,100,46,0,0,0,85,110,107,
110,111,119,110,32,109,97,99,114,111,44,32,116,114,121,32,63,32,111,114,32,40,48,41,
32,102,111,114,32,104,101,108,112,0,0,0,80,22,128,0,0,6,0,120,163,4,0,50,
214,58,2,0,0,0,0,128,251,3,0,55,92,58,2,0,0,0,0,128,251,0,0,6,
79,112,101,110,32,100,114,97,105,110,32,111,117,116,112,117,116,115,32,40,72,61,72,105,
45,90,44,32,76,61,71,78,68,41,0,0,32,49,46,32,72,69,88,13,10,32,50,46,
32,68,69,67,13,10,32,51,46,32,66,73,78,13,10,32,52,46,32,82,65,87,0,0,
87,65,82,78,73,78,71,58,32,112,105,110,115,32,110,111,116,32,111,112,101,110,32,100,
114,97,105,110,32,40,72,105,90,41,0,0,32,71,101,110,101,114,97,108,9,9,9,9,
9,80,114,111,116,111,99,111,108,32,105,110,116,101,114,97,99,116,105,111,110,0,0,0,
32,99,47,67,9,65,85,88,32,97,115,115,105,103,110,109,101,110,116,32,40,97,117,120,
47,67,83,41,9,9,48,120,49,50,51,0,32,103,47,83,9,71,101,110,101,114,97,116,
101,32,80,87,77,47,83,101,114,118,111,9,9,47,9,67,76,75,32,104,105,0,0,0,
32,108,47,76,9,66,105,116,111,114,100,101,114,32,40,109,115,98,47,76,83,66,41,9,
9,45,9,68,65,84,32,104,105,0,0,0,69,82,82,79,82,58,32,99,111,109,109,97,
110,100,32,104,97,115,32,110,111,32,101,102,102,101,99,116,32,104,101,114,101,0,0,0,
87,97,114,110,105,110,103,58,32,110,111,32,118,111,108,116,97,103,101,32,111,110,32,86,
112,117,108,108,117,112,32,112,105,110,0,0,67,111,109,109,97,110,100,109,111,100,101,63,
13,10,49,46,32,54,98,47,49,52,98,13,10,50,46,32,52,98,47,49,54,98,0,0,
83,116,111,112,32,98,105,116,115,58,13,10,32,49,46,32,49,32,42,100,101,102,97,117,
108,116,13,10,32,50,46,32,50,0,0,0,87,65,82,78,73,78,71,58,32,80,111,115,
115,105,98,108,101,32,98,117,102,102,101,114,32,111,118,101,114,102,108,111,119,0,0,0,
69,82,82,79,82,58,32,80,87,77,32,97,99,116,105,118,101,44,32,103,32,116,111,32,
100,105,115,97,98,108,101,0,0,104,116,116,112,58,47,47,100,97,110,103,101,114,111,117,
115,112,114,111,116,111,116,121,112,101,115,46,99,111,109,0,0,0,78,111,114,109,97,108,
32,111,117,116,112,117,116,115,32,40,72,61,51,46,51,118,44,32,76,61,71,78,68,41,
0,0,0,77,79,68,69,32,97,110,100,32,86,82,69,71,32,76,69,68,115,32,115,104,
111,117,108,100,32,98,101,32,111,110,33,0,32,111,9,83,101,116,32,111,117,116,112,117,
116,32,116,121,112,101,9,9,9,46,9,68,65,84,32,114,101,97,100,0,0,80,108,101,
97,115,101,32,101,120,105,116,32,80,73,67,32,112,114,111,103,114,97,109,109,105,110,103,
32,109,111,100,101,0,83,80,73,32,40,115,112,100,32,99,107,112,32,115,107,101,32,115,
109,112,32,99,115,108,32,104,105,122,41,61,40,32,0,85,65,82,84,32,40,115,112,100,
32,98,114,103,32,100,98,112,32,115,98,32,114,120,112,32,104,105,122,41,61,40,32,0,
86,82,69,71,32,116,111,111,32,108,111,119,44,32,105,115,32,116,104,101,114,101,32,97,
32,115,104,111,114,116,63,0,0,68,83,49,56,83,50,48,32,72,105,103,104,32,80,114,
101,99,32,68,105,103,32,84,104,101,114,109,0,0,0,67,111,109,109,97,110,100,32,110,
111,116,32,117,115,101,100,32,105,110,32,116,104,105,115,32,109,111,100,101,0,77,83,66,
32,115,101,116,58,32,77,79,83,84,32,115,105,103,32,98,105,116,32,102,105,114,115,116,
0,0,0,76,83,66,32,115,101,116,58,32,76,69,65,83,84,32,115,105,103,32,98,105,
116,32,102,105,114,115,116,0,0,13,10,73,110,118,97,108,105,100,32,99,104,111,105,99,
101,44,32,116,114,121,32,97,103,97,105,110,0,0,0,84,104,105,115,32,109,111,100,101,
32,114,101,113,117,105,114,101,115,32,97,110,32,97,100,97,112,116,101,114,0,78,117,109,
98,101,114,32,111,102,32,98,105,116,115,32,114,101,97,100,47,119,114,105,116,101,58,32,
0,0,0,32,35,9,82,101,115,101,116,32,116,104,101,32,66,80,32,32,32,9,9,9,
93,9,83,116,111,112,0,0,0,32,38,47,37,9,68,101,108,97,121,32,49,32,117,115,
47,109,115,9,9,9,125,9,83,116,111,112,0,0,0,32,102,9,77,101,97,115,117,114,
101,32,102,114,101,113,117,101,110,99,121,9,9,114,9,82,101,97,100,0,0,32,104,9,
67,111,109,109,97,110,100,104,105,115,116,111,114,121,9,9,9,92,9,67,76,75,32,108,
111,0,0,73,110,112,117,116,32,109,111,110,105,116,111,114,44,32,97,110,121,32,107,101,
121,32,101,120,105,116,115,0,0,67,83,58,13,10,32,49,46,32,67,83,13,10,32,50,
46,32,47,67,83,32,42,100,101,102,97,117,108,116,0,85,65,82,84,32,76,73,86,69,
32,68,73,83,80,76,65,89,44,32,125,32,84,79,32,83,84,79,80,0,0,82,97,119,
32,118,97,108,117,101,32,102,111,114,32,66,82,71,32,40,77,73,68,73,61,49,50,55,
41,0,0,68,83,49,56,66,50,48,32,80,114,111,103,32,82,101,115,32,68,105,103,32,
84,104,101,114,109,0,68,83,50,52,48,52,32,69,99,111,110,111,82,65,77,32,116,105,
109,101,32,67,104,105,112,0,0,83,101,108,102,45,116,101,115,116,32,105,110,32,72,105,
90,32,109,111,100,101,32,111,110,108,121,0,71,78,68,9,51,46,51,86,9,53,46,48,
86,9,65,68,67,9,86,80,85,9,65,85,88,9,0,32,109,9,67,104,97,110,103,101,
32,109,111,100,101,9,9,9,95,9,68,65,84,32,108,111,0,0,68,97,116,97,32,117,
110,105,116,32,108,101,110,103,116,104,32,40,98,105,116,115,41,58,32,0,0,68,83,49,
56,50,50,32,69,99,111,110,111,32,68,105,103,32,84,104,101,114,109,0,0,97,47,65,
47,64,32,99,111,110,116,114,111,108,115,32,65,85,88,32,112,105,110,0,0,97,47,65,
47,64,32,99,111,110,116,114,111,108,115,32,67,83,32,112,105,110,0,0,0,80,117,108,
108,45,117,112,32,114,101,115,105,115,116,111,114,115,32,79,70,70,0,0,0,65,85,88,
32,73,78,80,85,84,47,72,73,45,90,44,32,82,69,65,68,58,32,0,0,83,121,110,
116,97,120,32,101,114,114,111,114,32,97,116,32,99,104,97,114,32,0,0,0,120,46,32,
101,120,105,116,40,119,105,116,104,111,117,116,32,99,104,97,110,103,101,41,0,78,111,110,
101,120,105,115,116,101,110,116,32,112,114,111,116,111,99,111,108,33,0,0,0,32,126,9,
83,101,108,102,116,101,115,116,9,9,9,91,9,83,116,97,114,116,0,0,0,32,98,9,
83,101,116,32,98,97,117,100,114,97,116,101,9,9,9,49,50,51,0,0,0,77,97,99,
114,111,32,32,32,32,32,49,87,73,82,69,32,97,100,100,114,101,115,115,0,32,99,108,
111,110,101,32,119,47,100,105,102,102,101,114,101,110,116,32,80,73,67,0,0,78,111,116,
32,105,109,112,108,101,109,101,110,116,101,100,32,40,121,101,116,41,0,0,0,69,110,116,
101,114,32,114,97,119,32,118,97,108,117,101,32,102,111,114,32,66,82,71,0,42,83,104,
111,114,116,32,111,114,32,110,111,32,112,117,108,108,45,117,112,32,0,0,0,80,117,108,
108,45,117,112,32,114,101,115,105,115,116,111,114,115,32,79,78,0,68,105,115,112,108,97,
121,32,102,111,114,109,97,116,32,115,101,116,0,0,0,65,100,106,117,115,116,32,121,111,
117,114,32,116,101,114,109,105,110,97,108,0,82,83,32,76,79,87,44,32,67,79,77,77,
65,78,68,32,77,79,68,69,0,82,83,32,72,73,71,72,44,32,68,65,84,65,32,77,
79,68,69,0,0,0,49,46,40,66,82,41,9,50,46,40,82,68,41,9,51,46,40,79,
82,41,0,80,111,115,105,116,105,111,110,32,105,110,32,100,101,103,114,101,101,115,0,0,
65,76,65,82,77,32,83,69,65,82,67,72,32,40,48,120,69,67,41,0,0,42,78,111,
32,100,101,118,105,99,101,32,100,101,116,101,99,116,101,100,32,0,67,108,117,116,99,104,
32,100,105,115,101,110,103,97,103,101,100,33,33,33,0,80,79,87,69,82,32,83,85,80,
80,76,73,69,83,32,79,70,70,0,0,0,82,51,87,32,40,115,112,100,32,99,115,108,
32,104,105,122,41,61,40,32,0,76,73,86,69,32,68,73,83,80,76,65,89,32,83,84,
79,80,80,69,68,0,87,97,105,116,105,110,103,32,97,99,116,105,118,105,116,121,46,46,
46,0,0,68,83,50,52,51,49,32,49,75,32,69,69,80,82,79,77,0,0,49,75,72,
122,45,52,44,48,48,48,75,72,122,32,80,87,77,0,70,114,101,113,117,101,110,99,121,
32,105,110,32,75,72,122,32,0,68,117,116,121,32,99,121,99,108,101,32,105,110,32,37,
32,0,0,65,85,88,32,70,114,101,113,117,101,110,99,121,58,32,0,0,0,73,50,67,
32,40,109,111,100,32,115,112,100,41,61,40,32,0,0,84,80,48,32,73,78,80,85,84,
44,32,82,69,65,68,58,32,0,32,42,112,105,110,32,105,115,32,110,111,119,32,72,105,
90,0,0,83,112,97,99,101,32,116,111,32,99,111,110,116,105,110,117,101,0,77,65,84,
67,72,32,82,79,77,32,40,48,120,53,53,41,0,0,82,69,65,68,32,82,79,77,32,
40,48,120,51,51,41,58,32,0,83,75,73,80,32,82,79,77,32,40,48,120,67,67,41,
0,0,0,86,79,76,84,65,71,69,32,80,82,79,66,69,58,32,0,0,0,65,110,121,
32,107,101,121,32,116,111,32,101,120,105,116,0,0,0,67,108,117,116,99,104,32,101,110,
103,97,103,101,100,33,33,33,0,32,42,115,116,97,114,116,98,105,116,32,101,114,114,111,
114,0,0,32,42,115,116,111,112,98,105,116,32,101,114,114,111,114,0,0,0,80,79,87,
69,82,32,83,85,80,80,76,73,69,83,32,79,78,0,118,97,114,105,97,98,108,101,32,
108,101,110,103,116,104,0,0,0,82,50,87,32,40,115,112,100,32,104,105,122,41,61,40,
32,0,0,67,111,117,108,100,110,39,116,32,107,101,101,112,32,117,112,0,0,67,76,75,
9,77,79,83,73,9,67,83,9,77,73,83,79,0,0,10,13,67,97,108,99,117,108,97,
116,101,100,58,32,9,0,0,0,10,13,69,115,116,105,109,97,116,101,100,58,32,32,9,
0,0,0,70,65,73,76,69,68,44,32,78,79,32,68,65,84,65,0,0,0,42,66,121,
116,101,115,32,100,114,111,112,112,101,100,42,0,0,0,85,110,107,110,111,119,110,32,100,
101,118,105,99,101,0,80,87,77,32,100,105,115,97,98,108,101,100,0,0,0,65,85,88,
32,73,78,80,85,84,47,72,73,45,90,0,83,121,110,116,97,120,32,101,114,114,111,114,
0,0,0,68,65,84,65,32,83,84,65,84,69,58,32,0,0,0,68,65,84,65,32,79,
85,84,80,85,84,44,32,49,0,68,65,84,65,32,79,85,84,80,85,84,44,32,48,0,
67,76,79,67,75,32,84,73,67,75,83,58,32,0,0,110,111,32,109,111,100,101,32,99,
104,97,110,103,101,0,42,45,45,45,45,45,45,45,45,45,45,42,0,0,0,32,66,111,
111,116,108,111,97,100,101,114,32,118,0,0,65,114,101,32,121,111,117,32,115,117,114,101,
63,32,0,65,68,67,32,97,110,100,32,115,117,112,112,108,121,0,50,46,53,86,9,49,
46,56,86,9,84,80,48,9,0,83,101,114,118,111,32,97,99,116,105,118,101,0,0,0,
65,68,68,82,69,83,83,32,77,65,67,82,79,32,0,32,48,46,77,97,99,114,111,32,
109,101,110,117,0,0,83,69,65,82,67,72,32,40,48,120,70,48,41,0,0,86,79,76,
84,77,69,84,69,82,32,77,79,68,69,0,32,40,50,52,70,74,54,52,71,65,48,48,
32,0,0,73,50,67,32,83,84,65,82,84,32,66,73,84,0,0,73,50,67,32,83,84,
79,80,32,66,73,84,0,0,0,32,42,112,97,114,105,116,121,32,101,114,114,111,114,0,
32,85,78,75,78,79,87,78,32,69,82,82,79,82,0,40,49,41,32,103,101,116,32,100,
101,118,73,68,0,0,80,73,67,40,109,111,100,32,100,108,121,41,61,40,0,117,110,107,
110,111,119,110,32,109,111,100,101,0,0,0,68,97,116,97,32,117,110,105,116,115,58,32,
0,0,0,110,111,32,105,110,100,105,99,97,116,105,111,110,0,0,82,101,115,101,116,32,
116,111,32,101,120,105,116,0,0,42,42,32,69,97,114,108,121,32,69,120,105,116,33,0,
82,97,119,32,85,65,82,84,32,105,110,112,117,116,0,80,87,77,32,97,99,116,105,118,
101,0,0,32,64,112,103,109,115,112,97,99,101,58,0,84,111,111,32,108,111,110,103,33,
0,0,0,66,79,79,84,76,79,65,68,69,82,0,0,82,69,65,68,32,66,73,84,58,
32,0,0,66,117,115,32,72,105,45,90,32,48,0,0,66,117,115,32,72,105,45,90,32,
49,0,0,67,85,82,83,79,82,32,83,69,84,0,0,80,105,110,115,116,97,116,101,115,
58,0,0,80,9,80,9,80,9,73,9,73,9,0,0,32,97,117,116,111,114,97,110,103,
101,32,0,66,85,83,32,82,69,83,69,84,32,0,0,45,9,79,87,68,9,45,9,45,
0,0,0,83,67,76,9,83,68,65,9,45,9,45,0,80,71,67,9,80,71,68,9,45,
9,45,0,80,114,111,116,111,99,111,108,58,32,0,0,82,101,97,100,32,116,121,112,101,
58,32,0,67,83,32,68,73,83,65,66,76,69,68,0,67,83,32,69,78,65,66,76,69,
68,0,0,85,65,82,84,32,98,114,105,100,103,101,0,45,9,84,120,68,9,45,9,82,
120,68,0,87,97,114,110,105,110,103,58,32,0,0,0,65,85,88,32,72,73,71,72,0,
65,85,88,32,76,79,87,0,0,69,114,114,111,114,40,0,0,0,41,32,64,108,105,110,
101,58,0,32,98,121,116,101,115,46,0,0,40,66,65,83,73,67,41,0,0,68,69,76,
65,89,32,0,0,0,87,82,73,84,69,58,32,0,0,67,76,79,67,75,44,32,49,0,
67,76,79,67,75,44,32,48,0,120,46,32,101,120,105,116,0,0,68,69,86,73,68,58,
0,0,0,32,67,70,71,50,58,0,0,0,77,79,68,69,32,76,69,68,0,80,85,76,
76,85,80,32,72,0,80,85,76,76,85,80,32,76,0,66,117,115,32,104,105,103,104,0,
70,111,117,110,100,32,0,0,0,32,101,114,114,111,114,115,46,0,32,82,69,86,73,68,
58,0,0,13,10,32,32,32,42,0,0,0,32,84,73,77,69,79,85,84,0,68,101,108,
97,121,63,0,0,0,68,101,118,73,68,32,61,32,0,110,111,32,114,101,97,100,0,0,
32,82,101,118,32,61,32,0,0,50,32,119,105,114,101,0,0,0,51,32,119,105,114,101,
0,0,0,115,101,114,105,97,108,0,0,0,117,110,107,110,111,119,110,0,0,116,111,32,
101,110,100,0,0,0,40,92,45,47,95,92,45,41,0,40,92,95,47,45,41,0,0,0,
82,69,65,68,58,32,0,0,0,83,110,105,102,102,101,114,0,0,82,101,97,100,121,0,
82,69,83,69,84,0,67,70,71,49,58,0,67,116,114,108,0,0,65,85,88,0,0,0,
86,82,69,71,0,0,50,86,53,0,0,0,49,86,56,0,0,0,86,80,85,0,0,0,
51,46,51,86,0,0,65,68,67,0,0,0,77,79,83,73,0,0,67,76,75,0,0,0,
77,73,83,79,0,0,84,80,48,0,0,0,32,79,75,0,0,0,32,70,65,73,76,0,
73,78,73,84,0,0,67,76,69,65,82,0,80,9,80,9,0,0,71,78,68,9,0,0,
49,87,48,49,0,0,65,67,75,0,0,0,32,61,32,0,0,0,66,66,73,79,49,0,
85,78,75,0,0,0,27,91,50,75,13,0,27,91,68,0,0,0,27,91,50,68,0,0,
27,91,67,0,0,0,8,32,8,0,0,0,73,50,67,49,0,0,32,82,41,32,0,0,
32,87,41,32,0,0,32,78,79,78,69,0,78,65,67,75,0,0,79,67,68,49,0,0,
67,77,68,0,0,0,68,84,65,0,0,0,80,73,67,49,0,0,32,72,122,0,0,0,
82,70,85,0,0,0,82,65,87,49,0,0,83,80,73,49,0,0,32,98,112,115,0,0,
45,102,32,0,0,0,65,82,84,49,0,0,45,112,32,0,0,0,117,115,0,53,86,0,
67,83,0,109,115,0,48,98,0,65,51,0,66,52,0,66,53,0,66,56,0,50,32,0,
52,32,0,48,120,0,32,41,0,86,0,0,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,0,2,4,0,0,0,
47,140,32,66,39,169,69,7,169,66,167,174,254,255,55,240,255,47,96,25,136,192,127,34,
176,0,120,98,146,37,2,8,225,8,0,58,144,0,120,18,20,35,2,8,225,4,0,58,
160,127,34,16,4,224,0,72,235,12,0,58,160,127,34,0,72,235,204,34,169,200,34,168,
104,160,168,204,2,169,200,2,169,0,0,0,0,0,0,202,34,175,162,0,55,0,4,235,
38,53,128,39,54,128,164,6,168,164,38,169,164,70,168,164,102,169,164,134,169,196,6,168,
196,38,168,196,70,169,196,102,169,196,134,169,196,34,168,192,34,169,128,0,235,32,2,32,
64,17,136,32,98,168,33,226,168,35,66,168,115,0,7,16,12,177,11,0,50,48,2,177,
127,0,50,32,4,32,32,17,136,192,4,32,32,17,136,64,3,32,32,17,136,176,2,32,
32,17,136,115,0,55,64,13,32,32,17,136,16,0,32,32,17,136,32,0,32,32,17,136,
176,4,32,32,17,136,0,7,235,92,0,7,144,1,136,90,0,7,3,225,183,88,0,7,
2,225,183,2,129,191,128,2,120,0,2,120,83,0,7,128,6,120,81,0,7,128,1,120,
2,128,32,78,0,7,0,89,120,131,1,233,252,255,58,14,4,224,3,0,50,224,4,32,
32,17,136,232,255,55,3,124,42,3,40,225,16,0,62,5,0,224,18,0,58,2,128,32,
64,192,179,0,89,120,0,192,179,0,89,120,128,202,179,0,89,120,0,192,179,0,89,120,
0,192,179,0,89,120,0,192,179,0,89,120,4,0,55,128,0,235,0,5,32,32,17,136,
209,255,55,2,128,32,13,16,167,2,0,55,241,255,47,202,255,55,1,0,166,4,0,55,
133,10,187,32,4,36,26,0,7,128,0,235,3,4,32,178,202,187,178,90,187,178,90,187,
131,1,233,251,255,58,16,0,36,17,0,7,3,4,32,2,128,32,20,192,186,50,4,225,
9,0,58,52,64,186,50,4,225,6,0,58,52,64,186,50,4,225,3,0,58,131,1,233,
245,255,58,174,255,55,96,5,32,32,17,136,173,255,55,0,59,136,80,5,32,48,59,136,
160,10,32,48,59,136,97,231,168,0,0,0,0,0,0,97,231,175,254,255,55,0,0,6,
26,1,32,128,5,235,0,96,254,34,2,174,3,0,55,48,17,128,0,7,71,0,0,6,
139,5,233,248,255,58,10,5,233,245,255,58,47,140,32,8,0,166,202,34,174,104,255,55,
133,96,169,133,128,169,35,66,169,33,226,169,2,33,239,196,34,169,192,34,168,38,53,136,
39,54,136,104,160,169,200,2,168,0,0,32,96,25,136,56,255,55,248,255,47,88,255,55,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,201,254,55,5,4,0,223,249,0,127,63,0,
};

const image_t embedded_image = {
    .name = "BPCOMPBZ-pickit-firmware-v7.1.12-bootloader-v4.5.hex",
    .data = image_data,
    .crc32 = 0xEA68E9AEu,
    .used_rows = 323u,
};

/* Its ds30 bootloader alone ('bootloader ds30'): the reset GOTO and the bootloader page. */
static const uint8_t ds30_data[IMAGE_BL_BYTES] = {
0,168,4,0,0,0,47,140,32,66,39,169,69,7,169,66,167,174,254,255,55,240,255,47,
96,25,136,192,127,34,176,0,120,98,146,37,2,8,225,8,0,58,144,0,120,18,20,35,
2,8,225,4,0,58,160,127,34,16,4,224,0,72,235,12,0,58,160,127,34,0,72,235,
204,34,169,200,34,168,104,160,168,204,2,169,200,2,169,0,0,0,0,0,0,202,34,175,
162,0,55,0,4,235,38,53,128,39,54,128,164,6,168,164,38,169,164,70,168,164,102,169,
164,134,169,196,6,168,196,38,168,196,70,169,196,102,169,196,134,169,196,34,168,192,34,169,
128,0,235,32,2,32,64,17,136,32,98,168,33,226,168,35,66,168,115,0,7,16,12,177,
11,0,50,48,2,177,127,0,50,32,4,32,32,17,136,192,4,32,32,17,136,64,3,32,
32,17,136,176,2,32,32,17,136,115,0,55,64,13,32,32,17,136,16,0,32,32,17,136,
32,0,32,32,17,136,176,4,32,32,17,136,0,7,235,92,0,7,144,1,136,90,0,7,
3,225,183,88,0,7,2,225,183,2,129,191,128,2,120,0,2,120,83,0,7,128,6,120,
81,0,7,128,1,120,2,128,32,78,0,7,0,89,120,131,1,233,252,255,58,14,4,224,
3,0,50,224,4,32,32,17,136,232,255,55,3,124,42,3,40,225,16,0,62,5,0,224,
18,0,58,2,128,32,64,192,179,0,89,120,0,192,179,0,89,120,128,202,179,0,89,120,
0,192,179,0,89,120,0,192,179,0,89,120,0,192,179,0,89,120,4,0,55,128,0,235,
0,5,32,32,17,136,209,255,55,2,128,32,13,16,167,2,0,55,241,255,47,202,255,55,
1,0,166,4,0,55,133,10,187,32,4,36,26,0,7,128,0,235,3,4,32,178,202,187,
178,90,187,178,90,187,131,1,233,251,255,58,16,0,36,17,0,7,3,4,32,2,128,32,
20,192,186,50,4,225,9,0,58,52,64,186,50,4,225,6,0,58,52,64,186,50,4,225,
3,0,58,131,1,233,245,255,58,174,255,55,96,5,32,32,17,136,173,255,55,0,59,136,
80,5,32,48,59,136,160,10,32,48,59,136,97,231,168,0,0,0,0,0,0,97,231,175,
254,255,55,0,0,6,26,1,32,128,5,235,0,96,254,34,2,174,3,0,55,48,17,128,
0,7,71,0,0,6,139,5,233,248,255,58,10,5,233,245,255,58,47,140,32,8,0,166,
202,34,174,104,255,55,133,96,169,133,128,169,35,66,169,33,226,169,2,33,239,196,34,169,
192,34,168,38,53,136,39,54,136,104,160,169,200,2,168,0,0,32,96,25,136,56,255,55,
248,255,47,88,255,55,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,201,254,55,5,4,0,
223,249,0,127,63,0,
};

const image_t ds30_image = {
    .name = "bootloader v4.5 of BPCOMPBZ-pickit-firmware-v7.1.12-bootloader-v4.5.hex",
    .data = ds30_data,
    .crc32 = 0x23B303A0u,
    .used_rows = 6u,
    .bootloader_only = true,
};

/* bp3mitloader alone ('bootloader'): the reset GOTO and the bootloader page. */
static const uint8_t mit_data[IMAGE_BL_BYTES] = {
0,168,4,0,0,0,47,140,32,0,144,32,0,1,136,0,0,0,69,7,169,240,255,47,
66,167,175,2,0,55,0,0,233,252,255,58,0,4,235,0,3,235,128,3,235,97,127,34,
112,244,36,17,0,225,5,0,58,96,116,37,17,1,144,2,0,225,1,0,58,22,0,32,
128,24,235,128,24,235,17,130,251,128,88,235,129,0,232,96,146,37,17,0,225,7,0,58,
16,20,35,17,1,144,2,0,225,3,0,58,4,0,224,1,0,50,23,0,32,128,24,235,
128,8,235,6,0,224,4,0,50,195,0,7,2,0,50,252,167,4,0,0,0,7,0,224,
2,0,50,8,0,160,15,0,55,44,163,239,204,2,169,200,2,169,104,160,168,255,63,9,
0,0,0,202,34,174,7,0,55,179,0,7,5,0,50,104,160,169,200,2,168,44,35,239,
252,167,4,0,0,0,44,163,239,196,34,168,192,34,169,80,240,33,32,53,136,48,0,32,
32,54,136,32,2,32,64,17,136,128,0,40,0,17,136,35,66,168,2,161,239,0,33,239,
0,3,40,32,8,136,128,132,235,164,0,7,16,0,49,17,12,32,1,4,225,15,0,50,
65,14,32,1,4,225,2,0,58,136,0,7,245,255,55,32,4,32,169,0,7,192,4,32,
167,0,7,64,3,32,165,0,7,176,2,32,163,0,7,122,0,7,235,255,55,128,132,235,
64,13,32,158,0,7,16,0,32,156,0,7,32,0,32,154,0,7,176,4,32,152,0,7,
135,0,7,243,255,49,17,12,32,1,4,225,242,255,50,65,14,32,1,4,225,2,0,58,
107,0,7,246,255,55,0,5,120,128,5,120,123,0,7,231,255,49,10,5,64,72,6,221,
119,0,7,227,255,49,10,5,64,0,6,118,115,0,7,223,255,49,10,5,64,128,6,120,
111,0,7,219,255,49,10,5,64,0,7,120,5,128,32,23,140,32,14,3,120,6,0,224,
8,0,50,102,0,7,210,255,49,10,5,64,7,40,225,1,0,49,128,90,120,6,3,233,
248,255,58,224,4,32,10,4,224,211,255,58,11,0,224,10,0,58,1,128,42,1,96,225,
7,0,49,17,0,32,1,104,225,7,0,50,33,0,32,1,104,225,10,0,50,199,255,55,
128,132,235,0,5,32,196,255,55,97,112,225,194,255,58,1,192,47,129,4,102,176,4,32,
190,255,55,17,12,32,1,112,225,187,255,58,241,7,32,129,0,102,184,255,58,12,0,224,
11,0,58,1,128,32,64,0,32,128,88,120,0,0,32,128,88,120,128,10,32,128,88,120,
128,88,235,0,0,32,128,88,120,128,72,235,0,5,235,9,0,232,16,0,50,224,4,32,
1,192,47,129,0,102,9,8,225,163,255,58,9,0,224,5,0,58,12,0,224,159,255,58,
1,64,42,66,0,7,0,5,117,137,0,120,63,0,7,128,132,235,0,5,117,65,0,7,
0,5,117,89,0,7,0,5,117,176,4,32,10,0,224,145,255,50,96,5,32,143,255,55,
8,0,167,0,0,6,202,34,174,0,0,6,10,0,7,66,32,175,0,0,6,35,2,174,
254,255,55,97,127,34,112,244,36,128,24,120,96,116,37,128,8,120,0,0,254,50,32,239,
193,127,42,17,1,186,145,129,186,2,129,234,131,193,234,131,129,251,3,1,113,0,0,6,
0,33,239,0,96,254,34,2,175,8,0,55,34,34,175,34,34,169,0,8,128,33,161,39,
1,0,225,247,255,57,66,0,168,0,0,6,48,17,128,0,128,251,66,0,169,0,0,6,
35,34,175,254,255,55,32,17,136,0,0,6,50,66,120,4,130,251,178,66,120,133,130,251,
50,67,120,72,51,221,134,130,114,0,0,6,32,4,36,0,59,136,50,32,239,129,8,187,
11,0,55,16,0,36,0,59,136,50,32,239,2,128,32,140,1,120,7,4,32,236,255,7,
133,9,187,132,153,187,135,3,233,251,255,58,5,0,252,80,5,32,48,59,136,160,10,32,
48,59,136,97,231,168,0,0,0,0,0,0,97,231,175,254,255,55,0,59,128,97,199,169,
1,0,34,1,0,96,0,0,6,50,32,239,2,128,32,140,1,120,7,4,32,212,255,7,
19,0,186,5,0,225,8,0,58,51,128,186,0,128,251,4,0,225,4,0,58,135,3,233,
246,255,58,0,0,235,0,0,6,0,128,235,0,0,6,16,0,252,161,127,34,16,0,32,
128,72,120,193,127,34,96,146,37,128,24,120,16,20,35,128,8,120,0,0,254,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,70,255,55,6,4,0,
223,249,0,127,63,0,
};

const image_t mit_image = {
    .name = "bootloader v4.6 of bp3mitloader.hex",
    .data = mit_data,
    .crc32 = 0x4285EBB3u,
    .used_rows = 8u,
    .bootloader_only = true,
};
/* END GENERATED IMAGE */
