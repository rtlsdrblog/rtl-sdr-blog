/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Link with an actual upstream tuner_r82xx.c; mock only the I2C transport.
 * This checks register policy and error propagation, not RF performance. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tuner_r82xx.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static int fail_vga;

int rtlsdr_i2c_write_fn(void *dev, uint8_t addr, uint8_t *buf, int len)
{
    (void)dev; (void)addr;
    return fail_vga && buf[0] == 0x0c ? -7 : len;
}

int rtlsdr_i2c_read_fn(void *dev, uint8_t addr, uint8_t *buf, int len)
{
    (void)dev; (void)addr;
    memset(buf, 0, len);
    /* Wire bytes are bit-reversed by the driver: PLL lock and filter code. */
    if (len > 2) buf[2] = 0x02;
    if (len > 4) buf[4] = 0x80;
    return len;
}

int rtlsdr_check_dongle_model(void *dev, char *manufacturer, char *product)
{
    (void)dev; (void)manufacturer; (void)product;
    return 0;
}

int rtlsdr_set_bias_tee_gpio(void *dev, int gpio, int on)
{
    (void)dev; (void)gpio; (void)on;
    return 0;
}

static unsigned reg(const struct r82xx_priv *p, unsigned addr)
{
    return p->regs[addr - REG_SHADOW_START];
}

int main(void)
{
    struct r82xx_config cfg = {.i2c_addr=0x34, .xtal=28800000,
        .rafael_chip=CHIP_R820T, .max_i2c_msg_len=8};
    struct r82xx_priv p = {.cfg=&cfg};
    const uint32_t frequencies[] = {145000000,435000000,1090000000,1296000000};
    unsigned i, before, after;
    CHECK(r82xx_init(&p) == 0);
    for (i=0; i<sizeof(frequencies)/sizeof(*frequencies); ++i) {
        CHECK(r82xx_set_gain(&p, 0, 0) == 0);
        before = reg(&p, 0x0c) & 0x0f;
        CHECK(r82xx_set_freq(&p, frequencies[i]) == 0);
        after = reg(&p, 0x0c) & 0x0f;
        printf("%u Hz: auto VGA before=%u after tune=%u\n", frequencies[i], before, after);
        CHECK(before == after);
        CHECK(!(reg(&p, 0x05) & 0x10));
        CHECK(reg(&p, 0x07) & 0x10);
        CHECK(r82xx_set_gain(&p, 0, 0) == 0);
        CHECK((reg(&p, 0x0c) & 0x0f) == after);
        CHECK(r82xx_set_gain(&p, 1, 496) == 0);
        before = reg(&p, 0x0c) & 0x0f;
        CHECK(r82xx_set_freq(&p, frequencies[i]) == 0);
        CHECK((reg(&p, 0x0c) & 0x0f) == before);
        CHECK(reg(&p, 0x05) & 0x10);
        CHECK(!(reg(&p, 0x07) & 0x10));
    }
    fail_vga = 1;
    CHECK(r82xx_set_gain(&p, 0, 0) == -7);
    puts("VGA order, AGC mode bits, manual control and I2C failure: passed");
    return 0;
}
