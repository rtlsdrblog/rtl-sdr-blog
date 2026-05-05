/*
 * fic.c - FIC decoder for DAB, ported from dabtools
 *
 * Uses hard-decision DQPSK demapping and dabtools Viterbi convention:
 * - Input bits: 0 or 1
 * - Viterbi symbols: 127 = "0", 129 = "1", 128 = erasure
 */

#include <string.h>
#include <stdio.h>
#include "dab_time.h"

/* ─── Viterbi symbol convention (dabtools) ───────────────────────────── */
#define VITERBI_OFFSET 128
#define VITERBI_SYM(x) ((x) ? 129 : 127)

/* ─── Puncturing tables ──────────────────────────────────────────────── */

/* PI_16: 1110 repeating (keep 3 of 4) */
/* PI_15: 1110 × 7, then 1100 (keep 23 of 32) */

static void fic_depuncture(uint8_t *obuf, uint8_t *inbuf)
{
	int i, j;

	/* 21 blocks of PI_16: for each group of 4, keep 3, insert 1 erasure */
	for (i = 0; i < 21 * 128; i += 32) {
		for (j = 0; j < 8; j++) {
			*(obuf++) = VITERBI_SYM(*(inbuf++));
			*(obuf++) = VITERBI_SYM(*(inbuf++));
			*(obuf++) = VITERBI_SYM(*(inbuf++));
			*(obuf++) = VITERBI_OFFSET;
		}
	}
	/* 3 blocks of PI_15: 7 groups of (keep 3, drop 1) + 1 group of (keep 2, drop 2) */
	for (i = 21 * 128; i < 24 * 128; i += 32) {
		for (j = 0; j < 7; j++) {
			*(obuf++) = VITERBI_SYM(*(inbuf++));
			*(obuf++) = VITERBI_SYM(*(inbuf++));
			*(obuf++) = VITERBI_SYM(*(inbuf++));
			*(obuf++) = VITERBI_OFFSET;
		}
		*(obuf++) = VITERBI_SYM(*(inbuf++));
		*(obuf++) = VITERBI_SYM(*(inbuf++));
		*(obuf++) = VITERBI_OFFSET;
		*(obuf++) = VITERBI_OFFSET;
	}
	/* Tail: 6 groups of (keep 2, drop 2) */
	for (j = 0; j < 6; j++) {
		*(obuf++) = VITERBI_SYM(*(inbuf++));
		*(obuf++) = VITERBI_SYM(*(inbuf++));
		*(obuf++) = VITERBI_OFFSET;
		*(obuf++) = VITERBI_OFFSET;
	}
}

/* ─── PRBS for energy dispersal ──────────────────────────────────────── */

static void dab_descramble_bytes(uint8_t *buf, int len)
{
	uint16_t reg = 0x1FF;
	int i, j, bit;
	for (i = 0; i < len; i++) {
		for (j = 7; j >= 0; j--) {
			bit = ((reg >> 8) ^ (reg >> 4)) & 1;
			buf[i] ^= (bit << j);
			reg = ((reg << 1) | bit) & 0x1FF;
		}
	}
}

/* ─── CRC-16 for FIB (on packed bytes) ───────────────────────────────── */

static int check_fib_crc(uint8_t *fib)
{
	uint16_t crc = 0xFFFF;
	int i, j;
	/* CRC over first 30 bytes */
	for (i = 0; i < 30; i++) {
		crc ^= (uint16_t)fib[i] << 8;
		for (j = 0; j < 8; j++) {
			if (crc & 0x8000)
				crc = (crc << 1) ^ 0x1021;
			else
				crc <<= 1;
		}
	}
	crc ^= 0xFFFF;
	/* Compare with bytes 30-31 */
	uint16_t recv = ((uint16_t)fib[30] << 8) | fib[31];
	return crc == recv;
}

/* ─── FIC Decode ─────────────────────────────────────────────────────
 * Input: 9216 hard-decision bits (0 or 1) from 3 FIC symbols
 * Split into 4 blocks of 2304, each decoded to 3 FIBs (96 bytes)
 * Returns number of valid FIBs. fib_data gets 30 bytes per valid FIB. */

int fic_decode(uint8_t *soft_bits, int len, uint8_t *fib_data)
{
	uint8_t depunctured[3096];
	uint8_t decoded[96];  /* 768 bits packed into 96 bytes */
	int total_valid = 0;
	int block, fib;

	if (len < 9216) return 0;

	for (block = 0; block < 4; block++) {
		/* Depuncture: 2304 bits → 3096 symbols */
		fic_depuncture(depunctured, &soft_bits[block * 2304]);

		/* Viterbi decode: 3096 symbols → 768 bits → 96 bytes */
		viterbi_decode(depunctured, 3096, decoded, 768);

		/* Energy dispersal (descramble) on 96 bytes */
		dab_descramble_bytes(decoded, 96);

		/* Check CRC of 3 FIBs (each 32 bytes) */
		for (fib = 0; fib < 3; fib++) {
			if (check_fib_crc(&decoded[fib * 32])) {
				memcpy(&fib_data[total_valid * 30], &decoded[fib * 32], 30);
				total_valid++;
			}
		}
	}

	return total_valid;
}

void fic_init(struct fic_state *s)
{
	memset(s, 0, sizeof(*s));
}

/* ─── FIG 0/10 Parser ────────────────────────────────────────────────── */

int fig_parse_time(uint8_t *fib, int fib_len, struct dab_time *t)
{
	int pos = 0;
	int fig_type, fig_len, fig_ext;
	uint32_t mjd;
	double yp, mp;

	while (pos < fib_len) {
		fig_type = (fib[pos] >> 5) & 0x07;
		fig_len = fib[pos] & 0x1F;
		pos++;

		if (fig_type == 7 && fig_len == 31) break;

		if (fig_type == 0 && pos + fig_len <= fib_len) {
			fig_ext = fib[pos] & 0x1F;

			if (fig_ext == 10 && fig_len >= 5) {
				int long_form;
				uint8_t *d = &fib[pos + 1];

				mjd = ((uint32_t)d[0] << 9) |
				      ((uint32_t)d[1] << 1) |
				      ((uint32_t)(d[2] >> 7) & 1);

				t->lsi = (d[2] >> 6) & 1;
				long_form = (d[2] >> 5) & 1;
				t->hours = d[2] & 0x1F;
				t->minutes = (d[3] >> 2) & 0x3F;

				if (long_form && fig_len >= 7) {
					t->seconds = ((d[3] & 0x03) << 4) | ((d[4] >> 4) & 0x0F);
					t->milliseconds = ((d[4] & 0x0F) << 6) | ((d[5] >> 2) & 0x3F);
				} else {
					t->seconds = 0;
					t->milliseconds = -1;
				}

				if (t->hours > 23 || t->minutes > 59 || t->seconds > 59)
					return 0;
				if (mjd < 40000 || mjd > 80000)
					return 0;

				yp = (double)((int)mjd - 15078.2) / 365.25;
				mp = (double)((int)mjd - 14956.1 - (int)(yp * 365.25)) / 30.6001;
				t->day = (int)mjd - 14956 - (int)(yp * 365.25) - (int)(mp * 30.6001);
				if (mp == 14.0 || mp == 15.0) {
					t->year = (int)yp + 1901;
					t->month = (int)mp - 13;
				} else {
					t->year = (int)yp + 1900;
					t->month = (int)mp - 1;
				}

				t->valid = 1;
				return 1;
			}
		}
		pos += fig_len;
	}
	return 0;
}
