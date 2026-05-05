/*
 * fic.c - FIC (Fast Information Channel) decoder for DAB
 *
 * Based on welle.io's fic-handler approach.
 * Handles: depuncturing (PI_16 + PI_15 + PI_X), Viterbi decoding,
 * energy dispersal, CRC-16 check, and FIG 0/10 parsing.
 */

#include <string.h>
#include <stdio.h>
#include "dab_time.h"

/* ─── Puncturing tables from EN 300 401 ─────────────────────────────── */

/* PI_16: period 32, keeps 24 of 32 */
static const uint8_t PI_16[32] = {
	1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,1,0,
	1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,1,0
};

/* PI_15: period 32, keeps 23 of 32 */
static const uint8_t PI_15[32] = {
	1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,1,0,
	1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,0,0
};

/* PI_X: 24 bits for tail */
static const uint8_t PI_X[24] = {
	1,1,0,0, 1,1,0,0, 1,1,0,0,
	1,1,0,0, 1,1,0,0, 1,1,0,0
};

/* ─── PRBS for energy dispersal ──────────────────────────────────────
 * x^9 + x^5 + 1, init all 1s, operates on individual bits */

static uint8_t PRBS[768];
static int prbs_init_done = 0;

static void init_prbs(void)
{
	uint8_t shift_reg[9];
	int i;

	if (prbs_init_done) return;

	memset(shift_reg, 1, 9);
	for (i = 0; i < 768; i++) {
		PRBS[i] = shift_reg[8] ^ shift_reg[4];
		int feedback = PRBS[i];
		int j;
		for (j = 8; j > 0; j--)
			shift_reg[j] = shift_reg[j - 1];
		shift_reg[0] = feedback;
	}
	prbs_init_done = 1;
}

/* ─── CRC-16 on bits ─────────────────────────────────────────────────
 * Polynomial: x^16 + x^12 + x^5 + 1, init 0xFFFF
 * Operates on a bit array (one bit per byte) */

static int check_crc_bits(uint8_t *bits, int nbits)
{
	uint16_t crc = 0xFFFF;
	int i;
	/* CRC covers first nbits-16 bits, last 16 bits are the CRC value */
	for (i = 0; i < nbits - 16; i++) {
		int bit = bits[i] & 1;
		if ((crc >> 15) ^ bit)
			crc = (crc << 1) ^ 0x1021;
		else
			crc = crc << 1;
		crc &= 0xFFFF;
	}
	crc ^= 0xFFFF;

	/* Compare with received CRC (last 16 bits) */
	uint16_t recv_crc = 0;
	for (i = nbits - 16; i < nbits; i++)
		recv_crc = (recv_crc << 1) | (bits[i] & 1);

	return crc == recv_crc;
}

/* ─── FIC Decode ─────────────────────────────────────────────────────
 * Input: 2304 soft bits (int8_t stored as uint8_t, range -127..+127)
 * Process: depuncture → 3096 soft bits → Viterbi → 768 bits → 3 FIBs
 * Returns number of valid FIBs (0-3). fib_data gets 30 bytes per valid FIB. */

static int fic_decode_block(const uint8_t *ficblock, uint8_t *fib_data)
{
	uint8_t viterbi_input[3096 + 24];  /* depunctured: 3072 + 24 tail */
	uint8_t decoded_bits[768];
	int input_counter = 0;
	int local = 0;
	int i, k;
	int valid_fibs = 0;

	memset(viterbi_input, 128, sizeof(viterbi_input));  /* 128 = uncertain (after +127 shift) */

	/* Depuncture: 21 blocks of PI_16 (128 coded bits each) */
	for (i = 0; i < 21; i++) {
		for (k = 0; k < 128; k++) {
			if (PI_16[k % 32]) {
				/* Convert from signed (-127..+127) to unsigned (0..255) for Viterbi */
				viterbi_input[local] = (uint8_t)((int8_t)ficblock[input_counter] + 127);
				input_counter++;
			} else {
				viterbi_input[local] = 128;  /* Erasure */
			}
			local++;
		}
	}

	/* 3 blocks of PI_15 */
	for (i = 0; i < 3; i++) {
		for (k = 0; k < 128; k++) {
			if (PI_15[k % 32]) {
				viterbi_input[local] = (uint8_t)((int8_t)ficblock[input_counter] + 127);
				input_counter++;
			} else {
				viterbi_input[local] = 128;
			}
			local++;
		}
	}

	/* 24 tail bits with PI_X */
	for (k = 0; k < 24; k++) {
		if (PI_X[k]) {
			viterbi_input[local] = (uint8_t)((int8_t)ficblock[input_counter] + 127);
			input_counter++;
		} else {
			viterbi_input[local] = 128;
		}
		local++;
	}

	/* Viterbi decode: 3096 soft bits → 768 data bits */
	viterbi_decode(viterbi_input, local, decoded_bits, 768);

	/* Debug: show first 16 decoded bits as a hex byte pair */
	{
		static int dbg_count = 0;
		if (dbg_count < 2) {
			int d;
			uint8_t byte;
			/* Show soft bit distribution */
			int hist[4] = {0,0,0,0};  /* <32, 32-127, 128-223, >223 */
			for (d = 0; d < 2304; d++) {
				uint8_t v = viterbi_input[d];
				if (v < 32) hist[0]++;
				else if (v < 128) hist[1]++;
				else if (v < 224) hist[2]++;
				else hist[3]++;
			}
			fprintf(stderr, "\n[FIC] soft dist: <32:%d 32-127:%d 128-223:%d >223:%d\n",
				hist[0], hist[1], hist[2], hist[3]);
			fprintf(stderr, "[FIC] depunc=%d vit_out: ", local);
			for (d = 0; d < 8; d++) {
				byte = 0;
				int b2;
				for (b2 = 0; b2 < 8; b2++)
					byte = (byte << 1) | (decoded_bits[d*8+b2] & 1);
				fprintf(stderr, "%02X ", byte);
			}
			fprintf(stderr, "\n");
			dbg_count++;
		}
	}

	/* Energy dispersal (XOR with PRBS, bit-level) */
	for (i = 0; i < 768; i++)
		decoded_bits[i] ^= PRBS[i];

	/* Check 3 FIBs (each 256 bits = 30 data bytes + 2 CRC bytes) */
	for (i = 0; i < 3; i++) {
		uint8_t *fib_bits = &decoded_bits[i * 256];

		if (check_crc_bits(fib_bits, 256)) {
			/* Pack bits into bytes (30 data bytes) */
			int b, bit;
			uint8_t *out = &fib_data[valid_fibs * 30];
			for (b = 0; b < 30; b++) {
				uint8_t byte = 0;
				for (bit = 0; bit < 8; bit++)
					byte = (byte << 1) | (fib_bits[b * 8 + bit] & 1);
				out[b] = byte;
			}
			valid_fibs++;
		}
	}

	return valid_fibs;
}

/* ─── FIC Decode (full frame) ────────────────────────────────────────
 * Takes soft bits from 3 FIC OFDM symbols.
 * Each symbol provides 2*K = 3072 soft bits.
 * Total: 9216 soft bits → 4 blocks of 2304 → decode each.
 * Returns total valid FIBs (0-12). */

int fic_decode(uint8_t *soft_bits, int len, uint8_t *fib_data)
{
	int total_valid = 0;
	int block;
	int index = 0;
	uint8_t ofdm_input[2304];
	int ofdm_idx = 0;

	init_prbs();

	/* Collect soft bits into 2304-bit blocks and decode each */
	for (index = 0; index < len; index++) {
		ofdm_input[ofdm_idx++] = soft_bits[index];
		if (ofdm_idx >= 2304) {
			int n = fic_decode_block(ofdm_input, &fib_data[total_valid * 30]);
			total_valid += n;
			ofdm_idx = 0;
		}
	}

	return total_valid;
}

void fic_init(struct fic_state *s)
{
	memset(s, 0, sizeof(*s));
	init_prbs();
}

/* ─── FIG 0/10 Parser (Date and Time) ────────────────────────────────
 * FIG type 0, extension 10
 * Input: 30 bytes of FIB data (packed) */

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

		if (fig_type == 7 && fig_len == 31)
			break;  /* End marker */

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

				/* MJD to calendar date */
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
