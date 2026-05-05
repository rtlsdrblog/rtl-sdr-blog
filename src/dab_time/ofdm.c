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
static int freq_interleave_init_done = 0;

static void init_freq_interleave(void)
{
	int i, j;
	int16_t tmp[2048];
	int T_u = DAB_T_U;  /* 2048 */
	int K = DAB_K;       /* 1536 */
	int V1 = 511;
	int lwb = 256;
	int upb = 256 + K;  /* 1792 */

	if (freq_interleave_init_done) return;

	/* Generate permutation sequence */
	tmp[0] = 0;
	for (i = 1; i < T_u; i++)
		tmp[i] = (13 * tmp[i - 1] + V1) % T_u;

	/* Map: keep only entries in [lwb, upb), subtract T_u/2 to center */
	j = 0;
	for (i = 0; i < T_u; i++) {
		if (tmp[i] == T_u / 2)
			continue;
		if (tmp[i] < lwb || tmp[i] > upb)
			continue;
		freq_interleave_map[j] = tmp[i] - T_u / 2;
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
		for (i = 0; i < DAB_K; i++) {
			index = freq_interleave_map[i];
			if (index < 0)
				index += DAB_T_U;

			/* Differential decode against phase reference */
			r1 = s->fft_out[index] * conjf(s->prev_carriers[index]);

			/* Update phase reference for this carrier */
			s->prev_carriers[index] = s->fft_out[index];

			/* Normalize: L1 norm */
			ab1 = fabsf(crealf(r1)) + fabsf(cimagf(r1));
			if (ab1 > 0.0f)
				ab1 = 127.0f / ab1;
			else
				ab1 = 0.0f;

			/* Output soft bits: all reals first, then all imags
			 * Negated per DAB standard convention */
			int sb_re = (int)(crealf(r1) * ab1);
			int sb_im = (int)(cimagf(r1) * ab1);
			if (sb_re < -127) sb_re = -127;
			if (sb_re > 127) sb_re = 127;
			if (sb_im < -127) sb_im = -127;
			if (sb_im > 127) sb_im = 127;

			soft_bits[i]         = (uint8_t)(int8_t)sb_re;
			soft_bits[DAB_K + i] = (uint8_t)(int8_t)sb_im;
		}
	} else {
		/* PRS: just store phase reference */
		memcpy(s->prev_carriers, s->fft_out, DAB_T_U * sizeof(cfloat));
	}

	s->symbol_count++;
}
