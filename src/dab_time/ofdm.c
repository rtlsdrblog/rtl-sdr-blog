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

/* ─── Fine Frequency Offset Estimation ───────────────────────────────
 * Uses cyclic prefix correlation of a symbol to estimate freq offset */

void ofdm_estimate_freq(struct ofdm_state *s, cfloat *symbol_time)
{
	cfloat corr = 0;
	int i;
	for (i = 0; i < DAB_T_G; i++)
		corr += symbol_time[i] * conjf(symbol_time[i + DAB_T_U]);
	s->freq_offset = -cargf(corr) / (2.0f * M_PI * DAB_T_U) * DAB_SAMPLE_RATE;
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
	cfloat corrected[DAB_T_U];

	/* Apply fine frequency correction before FFT */
	for (i = 0; i < DAB_T_U; i++) {
		float phase = -2.0f * M_PI * s->freq_offset * (float)(i + DAB_T_G) / (float)DAB_SAMPLE_RATE;
		corrected[i] = symbol_time[DAB_T_G + i] * (cosf(phase) + I * sinf(phase));
	}

	/* FFT */
	ofdm_fft(corrected, s->fft_out, DAB_T_U, 0);

	/* fftshift: swap first and second half (like dabtools) */
	{
		int j;
		cfloat tmp;
		for (j = 0; j < DAB_T_U / 2; j++) {
			tmp = s->fft_out[j];
			s->fft_out[j] = s->fft_out[j + DAB_T_U / 2];
			s->fft_out[j + DAB_T_U / 2] = tmp;
		}
	}

	if (soft_bits) {
		/* Iterate FFT bins exactly like dabtools does AFTER its fftshift.
		 * Since we DON'T fftshift, and DQPSK is differential (cancels
		 * any constant phase), just iterate bins 256..1792 directly. */
		int k = 0;
		for (i = 256; i < 1793; i++) {
			int kk;
			float dr, di, denom;

			if (i == 1024) continue;  /* skip DC */

			dr = crealf(s->fft_out[i]) * crealf(s->prev_carriers[i])
			   + cimagf(s->fft_out[i]) * cimagf(s->prev_carriers[i]);
			di = crealf(s->fft_out[i]) * cimagf(s->prev_carriers[i])
			   - cimagf(s->fft_out[i]) * crealf(s->prev_carriers[i]);
			denom = crealf(s->prev_carriers[i]) * crealf(s->prev_carriers[i])
			      + cimagf(s->prev_carriers[i]) * cimagf(s->prev_carriers[i]);
			if (denom > 0) { dr /= denom; di /= denom; }

			s->prev_carriers[i] = s->fft_out[i];

			kk = rev_freq_deint[k];
			soft_bits[kk]         = (dr > 0) ? 0 : 1;
			soft_bits[DAB_K + kk] = (di > 0) ? 1 : 0;
			k++;
		}
	} else {
		/* PRS: store full FFT as phase reference */
		memcpy(s->prev_carriers, s->fft_out, DAB_T_U * sizeof(cfloat));
	}

	s->symbol_count++;
}
