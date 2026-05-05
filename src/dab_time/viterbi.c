/*
 * viterbi.c - Viterbi decoder for DAB FIC
 *
 * DAB FIC uses a rate 1/4, constraint length 7 convolutional code.
 * Generator polynomials (octal): 133, 171, 145, 133
 *
 * Soft bit convention: 0 = strong "1", 255 = strong "0", 128 = uncertain
 * Output: one decoded bit per byte (0 or 1)
 */

#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "dab_time.h"

#define NUM_STATES VITERBI_STATES
#define RATE       4

/* Generator polynomials for DAB (octal notation, standard form) */
static const int polys[RATE] = {0133, 0171, 0145, 0133};

/* Precomputed: expected soft-bit value (0 or 255) for each state/input/poly */
static uint8_t expected_output[NUM_STATES][2][RATE];
static int tables_initialized = 0;

static void init_tables(void)
{
	int state, input, poly, bit, reg, j;
	if (tables_initialized) return;

	for (state = 0; state < NUM_STATES; state++) {
		for (input = 0; input < 2; input++) {
			reg = (input << 6) | state;
			for (poly = 0; poly < RATE; poly++) {
				bit = 0;
				for (j = 0; j < 7; j++) {
					if (polys[poly] & (1 << j))
						bit ^= (reg >> j) & 1;
				}
				expected_output[state][input][poly] = bit ? 255 : 0;
			}
		}
	}
	tables_initialized = 1;
}

/* Branch metric: sum of |received - expected| for each of RATE bits */
static int branch_metric(uint8_t *received, uint8_t *expected)
{
	int i, metric = 0;
	for (i = 0; i < RATE; i++) {
		int diff = (int)received[i] - (int)expected[i];
		if (diff < 0) diff = -diff;
		metric += diff;
	}
	return metric;
}

/* Viterbi decode
 * input: soft bits (0-255), length = input_len (must be multiple of RATE)
 * output: decoded bits, one per byte (0 or 1), length = output_bits */
void viterbi_decode(uint8_t *input, int input_len, uint8_t *output, int output_bits)
{
	int num_symbols = input_len / RATE;
	int *pm_cur, *pm_prev, *tmp;
	uint8_t *traceback;
	int state, next_state, sym, input_bit;
	int metric, new_metric;

	if (num_symbols <= 0) return;
	init_tables();

	pm_cur = (int *)malloc(NUM_STATES * sizeof(int));
	pm_prev = (int *)malloc(NUM_STATES * sizeof(int));
	traceback = (uint8_t *)malloc(num_symbols * NUM_STATES);

	if (!pm_cur || !pm_prev || !traceback) {
		free(pm_cur); free(pm_prev); free(traceback);
		return;
	}

	/* Initialize: start in state 0 */
	for (state = 0; state < NUM_STATES; state++)
		pm_prev[state] = INT_MAX / 2;
	pm_prev[0] = 0;

	/* Forward pass */
	for (sym = 0; sym < num_symbols; sym++) {
		for (state = 0; state < NUM_STATES; state++)
			pm_cur[state] = INT_MAX / 2;

		for (state = 0; state < NUM_STATES; state++) {
			if (pm_prev[state] >= INT_MAX / 2) continue;

			for (input_bit = 0; input_bit < 2; input_bit++) {
				next_state = (state >> 1) | (input_bit << (VITERBI_K - 2));

				metric = branch_metric(&input[sym * RATE],
				                       expected_output[state][input_bit]);
				new_metric = pm_prev[state] + metric;

				if (new_metric < pm_cur[next_state]) {
					pm_cur[next_state] = new_metric;
					traceback[sym * NUM_STATES + next_state] = (uint8_t)state;
				}
			}
		}

		tmp = pm_prev;
		pm_prev = pm_cur;
		pm_cur = tmp;
	}

	/* Find best final state */
	{
		int best_state = 0, best_metric = pm_prev[0];
		for (state = 1; state < NUM_STATES; state++) {
			if (pm_prev[state] < best_metric) {
				best_metric = pm_prev[state];
				best_state = state;
			}
		}

		/* Traceback */
		memset(output, 0, output_bits);
		state = best_state;
		for (sym = num_symbols - 1; sym >= 0; sym--) {
			int prev_state = traceback[sym * NUM_STATES + state];
			int decoded_bit = (state >> (VITERBI_K - 2)) & 1;
			if (sym < output_bits) {
				output[sym] = decoded_bit;
			}
			state = prev_state;
		}
	}

	free(pm_cur);
	free(pm_prev);
	free(traceback);
}
