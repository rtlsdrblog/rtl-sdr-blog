/*
 * fic.c - FIC (Fast Information Channel) decoder for DAB
 *
 * Handles: energy dispersal, depuncturing, Viterbi decoding, CRC-16 check,
 * and FIG (Fast Information Group) parsing for type 0/10 (date and time).
 */

#include <string.h>
#include <stdio.h>
#include "dab_time.h"

/* ─── Depuncturing ──────────────────────────────────────────────────
 * DAB FIC uses puncturing pattern PI_16 from EN 300 401 Table 31
 * After mother code rate 1/4, bits are punctured to achieve ~rate 1/3
 *
 * PI_16: 1110 1110 1110 1110 1110 1110 1110 1110 (period 32, 24 of 32 kept)
 * Plus tail bits with PI_X: 1111 1111 (all kept)
 */

/* Puncturing vector for PI_16 (1=keep, 0=punctured), period 32 */
static const uint8_t pi_16[32] = {
	1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,1,0,
	1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,1,0
};

/* Each FIC sub-channel (CIF) carries 2304 bits after depuncturing
 * Input: 2016 punctured soft bits + 24 tail bits = 2040 bits per sub-block
 * After depuncturing: 2688 bits (672 symbols × 4 bits/symbol)
 * Viterbi output: 768 bits = 256 bits × 3 FIBs... actually:
 *
 * FIC: 3 FIBs per CIF, each FIB = 256 bits = 30 bytes data + 2 bytes CRC
 * Total FIC per frame: 3 × 256 = 768 decoded bits
 *
 * Actual structure per sub-block:
 *   Input soft bits from 3 FIC symbols: 3 × 3072 = 9216 soft bits
 *   Split into 3 sub-blocks of 3072 soft bits each
 *   Each sub-block: depuncture → 4096 soft bits → Viterbi → 1024 bits → 128 bytes
 *   But we only need the FIB data (30+2 = 32 bytes per FIB)
 *
 * Simplified approach: treat all FIC bits as one stream
 */

/* CRC-16 for FIB (polynomial: x^16 + x^12 + x^5 + 1, init 0xFFFF) */
static uint16_t crc16_fib(uint8_t *data, int len)
{
	uint16_t crc = 0xFFFF;
	int i, j;
	for (i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (j = 0; j < 8; j++) {
			if (crc & 0x8000)
				crc = (crc << 1) ^ 0x1021;
			else
				crc <<= 1;
		}
	}
	return crc ^ 0xFFFF;
}

/* ─── Energy Dispersal (PRBS) ────────────────────────────────────────
 * XOR with PRBS sequence: x^9 + x^5 + 1, init = 0x1FF */

static void energy_dispersal(uint8_t *data, int len)
{
	uint16_t reg = 0x1FF;
	int i, j, bit;
	for (i = 0; i < len; i++) {
		for (j = 7; j >= 0; j--) {
			bit = ((reg >> 8) ^ (reg >> 4)) & 1;
			data[i] ^= (bit << j);
			reg = ((reg << 1) | bit) & 0x1FF;
		}
	}
}

/* ─── Depuncture soft bits ───────────────────────────────────────────
 * Insert "uncertain" (128) values where bits were punctured */

static int depuncture(uint8_t *in, int in_len, uint8_t *out, int max_out)
{
	int i_in = 0, i_out = 0;
	int pi_idx = 0;

	while (i_in < in_len && i_out < max_out) {
		if (pi_16[pi_idx % 32]) {
			out[i_out] = in[i_in];
			i_in++;
		} else {
			out[i_out] = 128;  /* Erasure: uncertain */
		}
		i_out++;
		pi_idx++;
	}
	/* Pad remaining with uncertain */
	while (i_out < max_out) {
		out[i_out++] = 128;
	}
	return i_out;
}

/* ─── FIC Decode ─────────────────────────────────────────────────────
 * Takes soft bits from 3 FIC OFDM symbols, returns decoded FIB data.
 * Returns number of valid FIBs decoded (0-3). */

int fic_decode(uint8_t *soft_bits, int len, uint8_t *fib_data)
{
	uint8_t depunctured[4096];
	uint8_t decoded[128];
	int valid_fibs = 0;
	int fib;
	int sub_len;
	uint16_t crc_calc, crc_recv;

	/* Process each FIB sub-block
	 * Each FIB: ~2016 punctured soft bits → depuncture → Viterbi → 256 bits */
	sub_len = len / NUM_FIBS;

	for (fib = 0; fib < NUM_FIBS; fib++) {
		int dp_len;

		/* Depuncture */
		dp_len = depuncture(&soft_bits[fib * sub_len], sub_len,
				    depunctured, 2688);

		/* Viterbi decode: 2688 soft bits (rate 1/4) → 672 bits → 84 bytes
		 * But FIB is only 32 bytes (256 bits), rest is tail/padding */
		viterbi_decode(depunctured, dp_len, decoded, 256);

		/* Energy dispersal */
		energy_dispersal(decoded, 32);

		/* CRC check: bytes 0-29 = data, bytes 30-31 = CRC */
		crc_calc = crc16_fib(decoded, 30);
		crc_recv = ((uint16_t)decoded[30] << 8) | decoded[31];

		if (crc_calc == crc_recv) {
			memcpy(&fib_data[valid_fibs * 30], decoded, 30);
			valid_fibs++;
		}
	}

	return valid_fibs;
}

/* ─── FIG 0/10 Parser (Date and Time) ────────────────────────────────
 * FIG type 0, extension 10: date and time
 *
 * Short form (no seconds):
 *   MJD (17 bits) | LSI (1) | Conf (1) | UTC hours (5) | UTC minutes (6)
 *
 * Long form (with seconds + milliseconds):
 *   MJD (17 bits) | LSI (1) | Conf (1) | UTC hours (5) | UTC minutes (6) |
 *   UTC seconds (6) | UTC milliseconds (10)
 */

int fig_parse_time(uint8_t *fib, int fib_len, struct dab_time *t)
{
	int pos = 0;
	int fig_type, fig_len, fig_ext;
	uint32_t mjd;
	double yp, mp;

	while (pos < fib_len) {
		/* FIG header: type (3 bits) | length (5 bits) */
		fig_type = (fib[pos] >> 5) & 0x07;
		fig_len = fib[pos] & 0x1F;
		pos++;

		if (fig_type == 7 && fig_len == 31) {
			break;  /* End marker */
		}

		if (fig_type == 0 && pos + fig_len <= fib_len) {
			/* FIG 0: C/N (1) | OE (1) | P/D (1) | Extension (5) */
			fig_ext = fib[pos] & 0x1F;

			if (fig_ext == 10 && fig_len >= 5) {
				/* FIG 0/10: Date and Time */
				int long_form;
				uint8_t *d = &fib[pos + 1];  /* Skip FIG 0 header byte */

				/* Byte layout (short form, 4 bytes after header):
				 * d[0]: MJD[16:9]
				 * d[1]: MJD[8:1]
				 * d[2]: MJD[0] | LSI | Conf | UTC_hours[4:2]
				 * d[3]: UTC_hours[1:0] | UTC_minutes[5:0]
				 *
				 * Long form adds:
				 * d[4]: UTC_seconds[5:0] | UTC_ms[9:8]
				 * d[5]: UTC_ms[7:0]
				 */

				mjd = ((uint32_t)d[0] << 9) |
				      ((uint32_t)d[1] << 1) |
				      ((d[2] >> 7) & 0x01);

				t->lsi = (d[2] >> 6) & 0x01;
				long_form = (d[2] >> 5) & 0x01;
				t->hours = ((d[2] & 0x1F) << 0);
				/* Wait - re-read the bit layout more carefully */

				/* Actually per EN 300 401 §8.1.3.1:
				 * d[0..1] + d[2] bit 7 = MJD (17 bits)
				 * d[2] bit 6 = LSI
				 * d[2] bit 5 = confidence/UTC flag (long form indicator)
				 * d[2] bits 4:0 = hours[4:0]
				 * d[3] bits 7:2 = minutes[5:0]
				 * d[3] bits 1:0 = (long form) seconds[5:4]
				 * d[4] bits 7:4 = seconds[3:0]  (if long form)
				 * d[4] bits 3:0 + d[5] bits 7:2 = milliseconds[9:0]
				 */

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
					t->milliseconds = -1;  /* Not available */
				}

				/* Sanity check */
				if (t->hours > 23 || t->minutes > 59 || t->seconds > 59)
					return 0;
				if (mjd < 40000 || mjd > 80000)
					return 0;

				/* MJD to calendar date (same algorithm as RDS) */
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

void fic_init(struct fic_state *s)
{
	memset(s, 0, sizeof(*s));
}
