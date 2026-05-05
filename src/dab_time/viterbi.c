/*
 * viterbi.c - Viterbi decoder for DAB FIC
 *
 * DAB FIC uses a rate 1/4, constraint length 7 convolutional code.
 * Generator polynomials (octal): 133, 171, 145, 133
 *
 * This is a minimal soft-decision Viterbi decoder optimized for the
 * FIC channel only (short blocks, no tail-biting needed for our purpose).
 */

#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "dab_time.h"

#define NUM_STATES VITERBI_STATES
#define RATE       4

/* Generator polynomials for DAB (octal notation) */
static const int polys[RATE] = {0133, 0171, 0145, 0133};

/* Precomputed output bits for each state and input bit */
static uint8_t output_table[NUM_STATES][2][RATE];
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
				output_table[state][input][poly] = bit;
			}
		}
	}
	tables_initialized = 1;
}

/* Branch metric: distance between received soft bits and expected bits
 * Soft bits: 0 = confident 1, 255 = confident 0, 128 = uncertain */
static int branch_metric(uint8_t *received, uint8_t *expected, int n)
{
	int i, metric = 0;
	for (i = 0; i < n; i++) {
		/* If expected bit is 1, metric += received (low = good match)
		 * If expected bit is 0, metric += (255 - received) */
		if (expected[i])
			metric += received[i];
		else
			metric += 255 - received[i];
	}
	return metric;
}

void viterbi_decode(uint8_t *input, int input_len, uint8_t *output, int output_bits)
{
	/* Path metrics and traceback */
	int num_symbols = input_len / RATE;
	int *pm_cur, *pm_prev;
	uint8_t *traceback;  /* [num_symbols][NUM_STATES] */
	int state, next_state, sym, input_bit;
	int metric, new_metric;
	uint8_t expected[RATE];
	int i;

	if (num_symbols <= 0) return;
	init_tables();

	pm_cur = (int *)malloc(NUM_STATES * sizeof(int));
	pm_prev = (int *)malloc(NUM_STATES * sizeof(int));
	traceback = (uint8_t *)malloc(num_symbols * NUM_STATES);

	/* Initialize: start in state 0 */
	for (state = 0; state < NUM_STATES; state++)
		pm_prev[state] = INT_MAX / 2;
	pm_prev[0] = 0;

	/* Forward pass */
	for (sym = 0; sym < num_symbols; sym++) {
		int *tmp;
		for (state = 0; state < NUM_STATES; state++)
			pm_cur[state] = INT_MAX / 2;

		for (state = 0; state < NUM_STATES; state++) {
			if (pm_prev[state] >= INT_MAX / 2) continue;

			for (input_bit = 0; input_bit < 2; input_bit++) {
				next_state = ((state << 1) | input_bit) & (NUM_STATES - 1);

				/* Compute branch metric */
				for (i = 0; i < RATE; i++)
					expected[i] = output_table[state][input_bit][i] ? 0 : 255;

				metric = branch_metric(&input[sym * RATE], expected, RATE);
				new_metric = pm_prev[state] + metric;

				if (new_metric < pm_cur[next_state]) {
					pm_cur[next_state] = new_metric;
					traceback[sym * NUM_STATES + next_state] = (uint8_t)state;
				}
			}
		}

		/* Swap */
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
		memset(output, 0, (output_bits + 7) / 8);
		state = best_state;
		for (sym = num_symbols - 1; sym >= 0; sym--) {
			int prev_state = traceback[sym * NUM_STATES + state];
			/* The input bit that caused transition prev_state -> state */
			int decoded_bit = (state >> (VITERBI_K - 2)) & 1;
			if (sym < output_bits) {
				output[sym / 8] |= (decoded_bit << (7 - (sym % 8)));
			}
			state = prev_state;
		}
	}

	free(pm_cur);
	free(pm_prev);
	free(traceback);
}
