/*
 * rtl_dab_time - Minimal DAB FIC time extractor for system clock discipline
 * Copyright (C) 2026 - GPL v2+
 *
 * DAB Mode I parameters (most common, used in Band III)
 */

#ifndef DAB_TIME_H
#define DAB_TIME_H

#include <stdint.h>
#include <complex.h>

/* DAB Mode I parameters */
#define DAB_SAMPLE_RATE     2048000  /* 2.048 MS/s */
#define DAB_T_NULL          2656     /* Null symbol samples */
#define DAB_T_S             2552     /* Symbol duration (useful + guard) */
#define DAB_T_U             2048     /* Useful symbol duration (FFT size) */
#define DAB_T_G             504      /* Guard interval (cyclic prefix) */
#define DAB_T_F             196608   /* Frame duration in samples (96ms) */
#define DAB_L_SYMBOLS       76       /* Symbols per frame */
#define DAB_K              1536      /* Active carriers */
#define DAB_NUM_FIC_SYMBOLS 3        /* FIC symbols (symbols 1-3 after PRS) */

/* FIC parameters */
#define FIC_BITS_PER_SYMBOL (DAB_K * 2)  /* DQPSK = 2 bits/carrier */
#define FIC_PUNCTURED_CODEWORD_BITS 2304  /* Per sub-channel after puncturing */
#define FIC_DECODED_BYTES   32            /* Per FIB (Fast Information Block) */
#define FIC_NUM_SUBCH       4             /* Sub-channels in FIC (Mode I) */
#define FIC_FIBS_PER_SUBCH  3             /* FIBs per sub-channel */
#define NUM_FIBS            (FIC_NUM_SUBCH * FIC_FIBS_PER_SUBCH)  /* 12 FIBs per frame */

/* Viterbi constraint length 7, rate 1/4 */
#define VITERBI_K           7
#define VITERBI_STATES      64
#define VITERBI_POLY_A      0133  /* octal: G1 */
#define VITERBI_POLY_B      0171  /* octal: G2 */
#define VITERBI_POLY_C      0145  /* octal: G3 */
#define VITERBI_POLY_D      0133  /* octal: G4 - same as A for DAB */

/* Phase reference symbol (PRS) - carrier phases for Mode I
 * Used for frame sync and initial channel estimation */

/* OFDM state */
typedef float complex cfloat;

struct ofdm_state {
	cfloat  fft_out[DAB_T_U];
	cfloat  prev_carriers[DAB_T_U];  /* Full FFT phase reference */
	int     symbol_count;
	float   freq_offset;             /* Fine frequency offset in Hz */
};

/* FIC decoder state */
struct fic_state {
	uint8_t soft_bits[FIC_BITS_PER_SYMBOL * DAB_NUM_FIC_SYMBOLS];
	int     soft_bits_count;
};

/* Time result */
struct dab_time {
	int     valid;
	int     year, month, day;
	int     hours, minutes, seconds;
	int     milliseconds;  /* -1 if not available (short form) */
	int     lsi;           /* Leap second indicator */
};

/* Function prototypes */
void ofdm_init(struct ofdm_state *s);
void ofdm_estimate_freq(struct ofdm_state *s, cfloat *symbol_time);
int  ofdm_find_null(cfloat *samples, int len);
void ofdm_fft(cfloat *in, cfloat *out, int n, int inverse);
void ofdm_demod_symbol(struct ofdm_state *s, cfloat *symbol, uint8_t *soft_bits);

void fic_init(struct fic_state *s);
int  fic_decode(uint8_t *soft_bits, int len, uint8_t *fib_data);

int  fig_parse_time(uint8_t *fib, int fib_len, struct dab_time *t);

/* Viterbi decoder */
void viterbi_decode(uint8_t *input, int input_len, uint8_t *output, int output_bits);

/* Phase reference table for Mode I (subset needed for sync) */
void prs_generate(cfloat *prs_carriers);

#endif /* DAB_TIME_H */
