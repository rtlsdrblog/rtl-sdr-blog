/*
 * viterbi.c - Viterbi decoder for DAB FIC
 *
 * Rate 1/4, K=7, polynomials: 0133, 0171, 0145, 0133 (octal)
 * Input: symbols in dabtools convention (127="0", 129="1", 128=erasure)
 * Output: packed bytes (MSB first)
 */

#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "dab_time.h"

#define NUM_STATES VITERBI_STATES  /* 64 */
#define RATE 4

static const int polys[RATE] = {0133, 0171, 0145, 0133};

/* Expected symbol value for each state/input/poly
 * Encoder output 1 → symbol 129, output 0 → symbol 127 */
static uint8_t expected_output[NUM_STATES][2][RATE];
static int tables_initialized = 0;

static void init_tables(void)
{
	int state, input, poly, bit, reg, j;
	if (tables_initialized) return;

	for (state = 0; state < NUM_STATES; state++) {
		for (input = 0; input < 2; input++) {
			/* Shift register: input at MSB (bit 6), state in bits 5:0 */
			reg = (input << 6) | state;
			for (poly = 0; poly < RATE; poly++) {
				bit = 0;
				for (j = 0; j < 7; j++) {
					if (polys[poly] & (1 << j))
						bit ^= (reg >> j) & 1;
				}
				/* Soft convention: 0 = strong "1", 255 = strong "0" */
				expected_output[state][input][poly] = bit ? 129 : 127;
			}
		}
	}
	tables_initialized = 1;
}

/* Branch metric: sum of |received - expected| */
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
 * input: symbols (127/128/129 convention), length input_len
 * output: packed bytes (MSB first), nbits decoded */
void viterbi_decode(uint8_t *input, int input_len, uint8_t *output, int nbits)
{
	int num_symbols = input_len / RATE;
	int *pm_cur, *pm_prev, *tmp;
	uint8_t *traceback;
	int state, next_state, sym, input_bit;
	int metric, new_metric;
	int out_bytes = (nbits + 7) / 8;

	if (num_symbols <= 0) return;
	init_tables();

	pm_cur = (int *)malloc(NUM_STATES * sizeof(int));
	pm_prev = (int *)malloc(NUM_STATES * sizeof(int));
	traceback = (uint8_t *)malloc(num_symbols * NUM_STATES);
	if (!pm_cur || !pm_prev || !traceback) {
		free(pm_cur); free(pm_prev); free(traceback);
		return;
	}

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
				/* Right-shift: input enters at MSB */
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

		tmp = pm_prev; pm_prev = pm_cur; pm_cur = tmp;
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

		/* Traceback - output packed bytes MSB first */
		memset(output, 0, out_bytes);
		state = best_state;
		for (sym = num_symbols - 1; sym >= 0; sym--) {
			int prev_state = traceback[sym * NUM_STATES + state];
			int decoded_bit = (state >> (VITERBI_K - 2)) & 1;
			if (sym < nbits) {
				output[sym / 8] |= (decoded_bit << (7 - (sym % 8)));
			}
			state = prev_state;
		}
	}

	free(pm_cur);
	free(pm_prev);
	free(traceback);
}
