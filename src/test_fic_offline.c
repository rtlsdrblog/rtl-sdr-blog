/*
 * test_fic_offline.c - Test FIC decoding on a WAV IQ file
 * Usage: ./test_fic_offline /tmp/dab.2021-12-16T14_26_44_664.wav
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dab_time/dab_time.h"

#define BUF_LEN DAB_T_F

static cfloat samples[BUF_LEN];

int main(int argc, char **argv)
{
	FILE *f;
	int16_t iq[2];
	int i, n = 0;
	struct ofdm_state ofdm;
	uint8_t soft_bits[FIC_BITS_PER_SYMBOL * DAB_NUM_FIC_SYMBOLS];
	uint8_t fib_data[30 * NUM_FIBS];
	int null_pos, valid_fibs, total_fibs = 0, frames = 0;
	struct fic_state fic;
	struct dab_time t;

	if (argc < 2) { fprintf(stderr, "Usage: %s file.wav\n", argv[0]); return 1; }

	f = fopen(argv[1], "rb");
	if (!f) { perror("open"); return 1; }

	/* Skip WAV header (find 'data' chunk) */
	{
		char hdr[4];
		uint32_t chunk_size;
		fseek(f, 12, SEEK_SET);  /* Skip RIFF header */
		while (fread(hdr, 1, 4, f) == 4) {
			fread(&chunk_size, 4, 1, f);
			if (memcmp(hdr, "data", 4) == 0) break;
			fseek(f, chunk_size, SEEK_CUR);
		}
	}

	ofdm_init(&ofdm);
	fic_init(&fic);

	fprintf(stderr, "Reading IQ data...\n");

	/* Read samples and process frame by frame */
	while (frames < 20) {
		/* Fill one frame */
		n = 0;
		while (n < BUF_LEN) {
			if (fread(iq, sizeof(int16_t), 2, f) != 2) goto done;
			samples[n++] = (float)iq[0] / 32768.0f + I * (float)iq[1] / 32768.0f;
		}

		null_pos = ofdm_find_null(samples, BUF_LEN);
		if (null_pos < 0) {
			fprintf(stderr, "Frame %d: no null found\n", frames);
			frames++;
			continue;
		}

		int pos = null_pos + DAB_T_NULL;
		if (pos + (DAB_NUM_FIC_SYMBOLS + 1) * DAB_T_S > BUF_LEN) {
			fprintf(stderr, "Frame %d: null too late (pos=%d)\n", frames, null_pos);
			frames++;
			continue;
		}

		/* PRS */
		ofdm.freq_offset = -399.7f; fprintf(stderr,"[using -399.7 Hz]\n"); ofdm_demod_symbol(&ofdm, &samples[pos], NULL);
		pos += DAB_T_S;

		/* FIC symbols */
		int soft_bits_len = 0;
		int sym;
		for (sym = 0; sym < DAB_NUM_FIC_SYMBOLS; sym++) {
			if (pos + DAB_T_S > BUF_LEN) break;
			ofdm_demod_symbol(&ofdm, &samples[pos],
					  &soft_bits[sym * FIC_BITS_PER_SYMBOL]);
			soft_bits_len += FIC_BITS_PER_SYMBOL;
			pos += DAB_T_S;
		}

		if (soft_bits_len < FIC_BITS_PER_SYMBOL * DAB_NUM_FIC_SYMBOLS) {
			frames++;
			continue;
		}

		valid_fibs = fic_decode(soft_bits, soft_bits_len, fib_data);
		total_fibs += valid_fibs;
		fprintf(stderr, "Frame %d: null@%d, %d valid FIBs\n", frames, null_pos, valid_fibs);

		if (valid_fibs > 0) {
			int fib_i;
			for (fib_i = 0; fib_i < valid_fibs; fib_i++) {
				memset(&t, 0, sizeof(t));
				if (fig_parse_time(&fib_data[fib_i * 30], 30, &t)) {
					fprintf(stderr, "*** TIME: %04d-%02d-%02d %02d:%02d:%02d UTC ***\n",
						t.year, t.month, t.day, t.hours, t.minutes, t.seconds);
				}
			}
		}
		frames++;
	}

done:
	fclose(f);
	fprintf(stderr, "\nTotal: %d frames, %d valid FIBs\n", frames, total_fibs);
	return total_fibs > 0 ? 0 : 1;
}
