/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The six CS35L43 speaker amplifiers of the sheng board (three on QUP1 SE5, three on QUP2 SE0, I2C addresses 0x40-0x42, one shared
 * reset line). Each amplifier takes 16-bit stereo I2S from the secondary MI2S as a clock slave and plays its slot straight from the serial
 * port: the amplifier's own DSP is bypassed, so no amplifier firmware is needed. The top three play slot 0, the bottom three slot 1.
 * Registers and values are 32-bit big-endian on the wire. A program is a list of steps; the runner turns each step into one bus transfer
 * (a read-modify-write takes two) and never waits: the owner performs the transfer and feeds the result back. */
#ifndef SHENG_CS35L43_H
#define SHENG_CS35L43_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CS35_DEVID 0x0000000u
#define CS35_REVID 0x0000004u
#define CS35_GLOBAL_ENABLES 0x0002014u
#define CS35_BLOCK_ENABLES 0x0002018u
#define CS35_BLOCK_ENABLES2 0x000201Cu
#define CS35_GPIO_PAD_CONTROL 0x000242Cu
#define CS35_REFCLK_INPUT 0x0002C04u
#define CS35_GLOBAL_SAMPLE_RATE 0x0002C0Cu
#define CS35_FS_MON_0 0x0002D10u
#define CS35_BST_IPK_CTL 0x0003808u
#define CS35_BST_RSVD_1 0x0003850u
#define CS35_ASP_ENABLES1 0x0004800u
#define CS35_ASP_CONTROL2 0x0004808u
#define CS35_ASP_CONTROL3 0x000480Cu
#define CS35_ASP_FRAME_CONTROL5 0x0004820u
#define CS35_ASP_DATA_CONTROL5 0x0004840u
#define CS35_DACPCM1_INPUT 0x0004C00u
#define CS35_AMP_CTRL 0x0006000u
#define CS35_VPBR_CONFIG 0x0006404u
#define CS35_AMP_GAIN 0x0006C04u
#define CS35_TST_DAC_MSM_CONFIG 0x0007404u
#define CS35_ALIVE_DCIN_WD 0x0007424u
#define CS35_IRQ1_EINT_1 0x0010010u
#define CS35_IRQ1_EINT_2 0x0010014u
#define CS35_IRQ1_MASK_1 0x0010110u
#define CS35_IRQ1_MASK_2 0x0010114u
#define CS35_IRQ1_MASK_3 0x0010118u
#define CS35_TEST_KEY 0x0000040u

#define CS35_EINT2_PLL_LOCK 0x00000001u
#define CS35_EINT2_PLL_UNLOCK 0x00000008u
#define CS35_EINT1_ERRORS 0x1c400000u   /* DC watchdog, amplifier, temperature, boost short */

#define CS35_AMPS 6
#define CS35_GAIN_DB 18u                 /* the board's playback gain */

enum {Cs35Write = 1, Cs35Update = 2, Cs35Read = 3, Cs35DelayMs = 4};
typedef struct {uint8_t op; uint32_t reg, mask, value;} Cs35Step;

/* Programs: identify and configure one amplifier for the given ASP RX1 slot; power the amplifier up or down; read its interrupt status
 * (EINT1 then EINT2) and acknowledge it. Each returns the step count and writes at most CS35_MAX_STEPS steps. */
#define CS35_MAX_STEPS 40
unsigned cs35_init_program(Cs35Step *out, unsigned slot);
unsigned cs35_power_up_program(Cs35Step *out);
unsigned cs35_power_down_program(Cs35Step *out);
unsigned cs35_status_program(Cs35Step *out);
/* The slot an amplifier plays (index 0-2 on SE5 = top right/high/left, 3-5 on SE0 = bottom). */
static inline unsigned cs35_slot(unsigned amp) {return amp < 3 ? 0u : 1u;}

/* The runner. */
typedef struct {
    const Cs35Step *steps;
    unsigned count, index;
    uint8_t phase;                   /* 0 next step, 1 read of a read-modify-write in flight, 2 write in flight, 3 read in flight,
                                        4 the write of a read-modify-write is next */
    uint32_t pending;                /* that write's value */
    uint32_t reads[4];               /* values of Cs35Read steps, in order */
    unsigned read_count;
} Cs35Run;
enum {Cs35Transfer = 1, Cs35Wait = 2, Cs35Finished = 3};
typedef struct {
    uint8_t bytes[8];
    uint32_t length;                 /* bytes to write */
    uint32_t read_length;            /* 0 or 4: then a read follows the write */
    uint32_t delay_ms;               /* Cs35Wait */
} Cs35Action;
void cs35_run_start(Cs35Run *r, const Cs35Step *steps, unsigned count);
/* What to do next: Cs35Transfer (perform a, then call cs35_run_result), Cs35Wait (wait a.delay_ms, then call cs35_run_next again) or
 * Cs35Finished. */
int cs35_run_next(Cs35Run *r, Cs35Action *a);
/* The transfer finished; value: the 32-bit value read (when one was read). */
void cs35_run_result(Cs35Run *r, uint32_t value);
/* A value read on the wire (first byte in bits 7:0) as the register value. */
static inline uint32_t cs35_wire_value(uint32_t rx) {return (rx >> 24) | ((rx >> 8) & 0xff00u) | ((rx << 8) & 0xff0000u) | (rx << 24);}

#ifdef __cplusplus
}
#endif
#endif
