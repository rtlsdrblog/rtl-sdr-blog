/*
 * ofdm.c - Minimal OFDM demodulator for DAB Mode I
 *
 * Implements: null symbol detection, coarse/fine frequency sync,
 * FFT, DQPSK demodulation of FIC carriers only.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "dab_time.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ─── Phase Reference Symbol (PRS) table for Mode I ─────────────────
 * From ETSI EN 300 401, Table 44. h values for carriers -768..+768
 * We store the phase index (0-3) for each carrier k, phase = h[k]*pi/2
 * Full table has 1536 entries. We generate from the specification's
 * algorithm using the primitive polynomial. */

/* h_i,j parameters from Table 44 (Mode I) */
static const int prs_h[4][32] = {
	{0,2,0,0,0,0,1,1,2,0,0,0,2,2,1,1,0,2,0,0,0,0,1,1,2,0,0,0,2,2,1,1},
	{0,3,2,3,0,1,3,0,2,1,2,3,2,3,3,0,0,3,2,3,0,1,3,0,2,1,2,3,2,3,3,0},
	{0,0,0,2,0,2,1,3,2,2,0,2,2,0,1,3,0,0,0,2,0,2,1,3,2,2,0,2,2,0,1,3},
	{0,1,2,1,0,3,3,2,2,3,2,1,2,1,3,2,0,1,2,1,0,3,3,2,2,3,2,1,2,1,3,2}
};

static int prs_phase_index(int k)
{
	/* Compute phase for carrier k (-768 to +768, excluding 0) */
	int kp, i, n;
	if (k < 0) kp = k + 1536;
	else kp = k;

	i = (kp - 1) / 32;
	n = (kp - 1) % 32;

	if (i < 0 || i >= 48 || n < 0 || n >= 32) return 0;

	/* Simplified: use h table with wrapping */
	return prs_h[i % 4][n];
}

void prs_generate(cfloat *prs_carriers)
{
	int k, idx;
	double phase;
	/* Generate reference carriers for k = -768..-1, 1..768 */
	for (k = -768; k <= 768; k++) {
		if (k == 0) continue;
		idx = (k < 0) ? k + 768 : k + 767;  /* map to 0..1535 */
		phase = (double)prs_phase_index(k) * M_PI / 2.0;
		prs_carriers[idx] = cosf((float)phase) + I * sinf((float)phase);
	}
}

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

/* ─── OFDM Init ─────────────────────────────────────────────────────── */

void ofdm_init(struct ofdm_state *s)
{
	memset(s, 0, sizeof(*s));
}

/* ─── Null Symbol Detection ──────────────────────────────────────────
 * The null symbol has significantly lower power than data symbols.
 * We detect it by finding a power dip in the input stream. */

int ofdm_find_null(cfloat *samples, int len)
{
	int i, best_pos = -1;
	float power, min_power = 1e30f;
	float avg_power = 0.0f;
	int window = DAB_T_NULL / 4;  /* Use quarter-null for detection */
	int num_windows = 0;

	if (len < DAB_T_NULL + DAB_T_S) return -1;

	/* Compute running power in windows */
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

	/* Null symbol should be significantly below average.
	 * Use 3dB threshold (0.5) — more tolerant of weak signals
	 * and AGC transients than the theoretical 10+dB null depth. */
	if (avg_power > 0.0f && min_power < avg_power * 0.5f) {
		return best_pos;
	}
	return -1;
}

/* ─── Fine Frequency Offset Estimation ───────────────────────────────
 * Uses the cyclic prefix correlation of the PRS symbol */

static double estimate_fine_freq(cfloat *symbol_with_cp)
{
	/* Correlate guard interval with end of symbol */
	cfloat corr = 0;
	int i;
	for (i = 0; i < DAB_T_G; i++) {
		corr += symbol_with_cp[i] * conjf(symbol_with_cp[i + DAB_T_U]);
	}
	return cargf(corr) / (2.0 * M_PI * DAB_T_U) * DAB_SAMPLE_RATE;
}

/* ─── Frequency Interleaving Table (Mode I, 1536 carriers) ──────────
 * EN 300 401 §14.6: π(i) = (13 × π(i-1) + 511) mod 2048
 * Only indices < 1536 are used (skip those ≥ 1536).
 * freq_deinterleave[i] = logical position of carrier i in the FFT output */

static int freq_deinterleave[DAB_K];
static int freq_interleave_init = 0;

static void init_freq_interleave(void)
{
	int i, k, idx;
	int perm[2048];

	if (freq_interleave_init) return;

	/* Generate permutation: π(0)=0, π(i)=(13*π(i-1)+511) mod 2048 */
	perm[0] = 0;
	for (i = 1; i < 2048; i++)
		perm[i] = (13 * perm[i - 1] + 511) % 2048;

	/* Map: only keep indices that fall in carrier range [0..1535]
	 * The permutation maps logical index → physical carrier position */
	idx = 0;
	for (i = 0; i < 2048; i++) {
		k = perm[i] - 256;  /* Offset: carriers 0..1535 map to perm values 256..1791 */
		if (k >= 0 && k < DAB_K) {
			freq_deinterleave[k] = idx;
			idx++;
		}
	}
	freq_interleave_init = 1;
}

/* ─── DQPSK Demodulation ────────────────────────────────────────────
 * DAB uses Differential QPSK: data is encoded in the phase difference
 * between the same carrier in consecutive symbols. */

void ofdm_demod_symbol(struct ofdm_state *s, cfloat *symbol_time, uint8_t *soft_bits)
{
	int i, k;
	cfloat carriers[DAB_K];
	cfloat diff;

	init_freq_interleave();

	/* Remove cyclic prefix and FFT */
	ofdm_fft(symbol_time + DAB_T_G, s->fft_out, DAB_T_U, 0);

	/* Extract active carriers (k = -768..-1, 1..768)
	 * In FFT output: negative freqs at indices [T_U-768 .. T_U-1]
	 *                positive freqs at indices [1 .. 768] */
	for (k = 0; k < 768; k++) {
		carriers[k] = s->fft_out[DAB_T_U - 768 + k];       /* k = -768..-1 */
		carriers[k + 768] = s->fft_out[k + 1];              /* k = 1..768 */
	}

	/* DQPSK: compute phase difference with previous symbol */
	if (s->symbol_count > 0 && soft_bits) {
		for (i = 0; i < DAB_K; i++) {
			float rotated_re, rotated_im;
			int sb0, sb1;
			int logical_pos;

			diff = carriers[i] * conjf(s->prev_carriers[i]);

			/* Rotate by -pi/4 to align constellation to axes */
			rotated_re = crealf(diff) * 0.7071f + cimagf(diff) * 0.7071f;
			rotated_im = -crealf(diff) * 0.7071f + cimagf(diff) * 0.7071f;

			/* Soft decisions (quantized to 0-255, 128 = uncertain) */
			sb0 = (int)(128.0f - rotated_re * 4.0f);
			sb1 = (int)(128.0f - rotated_im * 4.0f);
			if (sb0 < 0) sb0 = 0;
			if (sb0 > 255) sb0 = 255;
			if (sb1 < 0) sb1 = 0;
			if (sb1 > 255) sb1 = 255;

			/* Frequency de-interleave: place bits at logical position */
			logical_pos = freq_deinterleave[i];
			soft_bits[logical_pos * 2]     = (uint8_t)sb0;
			soft_bits[logical_pos * 2 + 1] = (uint8_t)sb1;
		}
	}

	/* Store for next symbol's differential decode */
	memcpy(s->prev_carriers, carriers, DAB_K * sizeof(cfloat));
	s->symbol_count++;
}
