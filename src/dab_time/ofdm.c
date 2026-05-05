/*
 * ofdm.c - Minimal OFDM demodulator for DAB Mode I
 *
 * Based on the approach from welle.io (Jan van Katwijk / Matthias P. Braendli)
 * Implements: null symbol detection, FFT, DQPSK demodulation with
 * frequency de-interleaving.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "dab_time.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ─── Radix-2 DIT FFT ───────────────────────────────────────────────── */

static void bit_reverse(cfloat *x, int n)
{
	int i, j, k;
	for (i = 1, j = 0; i < n; i++) {
		k = n >> 1;
		while (j & k) { j ^= k; k >>= 1; }
		j ^= k;
		if (i < j) {
			cfloat tmp = x[i];
			x[i] = x[j];
			x[j] = tmp;
		}
	}
}

void ofdm_fft(cfloat *in, cfloat *out, int n, int inverse)
{
	int step, half, i, j;
	double angle_sign = inverse ? 1.0 : -1.0;
	cfloat w, wn, t;

	memcpy(out, in, n * sizeof(cfloat));
	bit_reverse(out, n);

	for (step = 2; step <= n; step <<= 1) {
		half = step >> 1;
		wn = cosf((float)(angle_sign * 2.0 * M_PI / step))
		   + I * sinf((float)(angle_sign * 2.0 * M_PI / step));
		for (i = 0; i < n; i += step) {
			w = 1.0f;
			for (j = 0; j < half; j++) {
				t = w * out[i + j + half];
				out[i + j + half] = out[i + j] - t;
				out[i + j] = out[i + j] + t;
				w *= wn;
			}
		}
	}

	if (inverse) {
		for (i = 0; i < n; i++)
			out[i] /= n;
	}
}

/* ─── Frequency Interleaving Table (Mode I) ─────────────────────────
 * EN 300 401 §14.6
 * mapIn(i) returns the FFT bin index for logical carrier i.
 * Values range from -768..+768 (excluding 0).
 * For negative values, add T_u to get the actual FFT array index. */

static int16_t freq_interleave_map[DAB_K];
static uint16_t rev_freq_deint[DAB_K];
static int freq_interleave_init_done = 0;

static void init_freq_interleave(void)
{
	int i, j, n, k;
	int16_t tmp[2048];
	int freq_deint_tab[DAB_K];

	if (freq_interleave_init_done) return;

	tmp[0] = 0;
	for (i = 1; i < DAB_T_U; i++)
		tmp[i] = (13 * tmp[i - 1] + 511) % DAB_T_U;

	/* Build forward table (same as dabtools) */
	n = 0;
	for (i = 0; i < DAB_T_U; i++) {
		if (tmp[i] >= 256 && tmp[i] <= 1792 && tmp[i] != 1024) {
			k = tmp[i] - 1024;
			if (k < 0) k = DAB_K/2 + k;
			else if (k > 0) k = DAB_K/2 + k - 1;
			freq_deint_tab[n] = k;
			n++;
		}
	}

	/* Build reverse table */
	for (i = 0; i < DAB_K; i++)
		rev_freq_deint[freq_deint_tab[i]] = i;

	/* Also keep welle.io style map for reference */
	j = 0;
	for (i = 0; i < DAB_T_U; i++) {
		if (tmp[i] == DAB_T_U/2) continue;
		if (tmp[i] < 256 || tmp[i] > 1792) continue;
		freq_interleave_map[j] = tmp[i] - DAB_T_U/2;
		j++;
	}

	freq_interleave_init_done = 1;
}

/* ─── OFDM Init ─────────────────────────────────────────────────────── */

void ofdm_init(struct ofdm_state *s)
{
	memset(s, 0, sizeof(*s));
	init_freq_interleave();
}

/* ─── Null Symbol Detection ──────────────────────────────────────────
 * The null symbol has significantly lower power than data symbols. */

int ofdm_find_null(cfloat *samples, int len)
{
	int i, best_pos = -1;
	float power, min_power = 1e30f;
	float avg_power = 0.0f;
	int window = DAB_T_NULL / 4;
	int num_windows = 0;

	if (len < DAB_T_NULL + DAB_T_S) return -1;

	for (i = 0; i < len - window; i += window / 2) {
		int j;
		power = 0.0f;
		for (j = 0; j < window; j++) {
			float re = crealf(samples[i + j]);
			float im = cimagf(samples[i + j]);
			power += re * re + im * im;
		}
		avg_power += power;
		num_windows++;
		if (power < min_power) {
			min_power = power;
			best_pos = i;
		}
	}

	if (num_windows == 0) return -1;
	avg_power /= num_windows;

	if (avg_power > 0.0f && min_power < avg_power * 0.5f) {
		return best_pos;
	}
	return -1;
}

/* ─── Process PRS (Phase Reference Symbol) ───────────────────────────
 * Store the raw FFT output as phase reference for DQPSK. */

void ofdm_process_prs(struct ofdm_state *s, cfloat *symbol_time)
{
	/* FFT the useful part (skip guard interval) */
	ofdm_fft(symbol_time + DAB_T_G, s->fft_out, DAB_T_U, 0);
	/* Store as phase reference */
	memcpy(s->prev_carriers, s->fft_out, DAB_T_U * sizeof(cfloat));
	s->symbol_count = 1;
}

/* ─── DQPSK Demodulation (data symbols) ─────────────────────────────
 * Following welle.io's approach:
 * - FFT the symbol
 * - For each logical carrier i, get FFT bin via interleaver
 * - Compute differential: fft[bin] * conj(phase_ref[bin])
 * - Update phase reference
 * - Output: soft_bits[0..K-1] = -real, soft_bits[K..2K-1] = -imag
 * Soft bits are in range -127..+127 (stored as int8_t cast to uint8_t) */

void ofdm_demod_symbol(struct ofdm_state *s, cfloat *symbol_time, uint8_t *soft_bits)
{
	int i;
	int16_t index;
	cfloat r1;
	float ab1;

	/* FFT */
	ofdm_fft(symbol_time + DAB_T_G, s->fft_out, DAB_T_U, 0);

	if (soft_bits) {
		/* Iterate carriers in dabtools order but with UN-shifted FFT:
		 * dabtools shifts FFT so DC is at bin 1024.
		 * Without shift: DC at bin 0, neg freq at 1025..2047, pos at 1..1023.
		 * dabtools bins 256..1023 (neg after shift) = our bins 1280..2047
		 * dabtools bins 1025..1792 (pos after shift) = our bins 1..768
		 * Combined: iterate our bins 1280..2047, then 1..768 (skip 0=DC) */
		int k = 0;
		/* Negative frequencies: bins 1280..2047 (= dabtools 256..1023) */
		for (i = 1280; i < 2048; i++) {
			int kk;
			r1 = s->fft_out[i] * conjf(s->prev_carriers[i]);
			s->prev_carriers[i] = s->fft_out[i];

			ab1 = fabsf(crealf(r1)) + fabsf(cimagf(r1));
			if (ab1 > 0.0f) ab1 = 127.0f / ab1; else ab1 = 0.0f;

			int sb_re = (int)(-crealf(r1) * ab1);
			int sb_im = (int)(cimagf(r1) * ab1);
			if (sb_re < -127) sb_re = -127; if (sb_re > 127) sb_re = 127;
			if (sb_im < -127) sb_im = -127; if (sb_im > 127) sb_im = 127;

			kk = rev_freq_deint[k];
			soft_bits[kk]         = (uint8_t)(int8_t)sb_re;
			soft_bits[DAB_K + kk] = (uint8_t)(int8_t)sb_im;
			k++;
		}
		/* Positive frequencies: bins 1..768 (= dabtools 1025..1792) */
		for (i = 1; i <= 768; i++) {
			int kk;
			r1 = s->fft_out[i] * conjf(s->prev_carriers[i]);
			s->prev_carriers[i] = s->fft_out[i];

			ab1 = fabsf(crealf(r1)) + fabsf(cimagf(r1));
			if (ab1 > 0.0f) ab1 = 127.0f / ab1; else ab1 = 0.0f;

			int sb_re = (int)(-crealf(r1) * ab1);
			int sb_im = (int)(cimagf(r1) * ab1);
			if (sb_re < -127) sb_re = -127; if (sb_re > 127) sb_re = 127;
			if (sb_im < -127) sb_im = -127; if (sb_im > 127) sb_im = 127;

			kk = rev_freq_deint[k];
			soft_bits[kk]         = (uint8_t)(int8_t)sb_re;
			soft_bits[DAB_K + kk] = (uint8_t)(int8_t)sb_im;
			k++;
		}
	} else {
		/* PRS: store full FFT as phase reference */
		memcpy(s->prev_carriers, s->fft_out, DAB_T_U * sizeof(cfloat));
	}

	s->symbol_count++;
}
