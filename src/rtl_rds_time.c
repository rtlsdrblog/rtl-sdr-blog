/*
 * rtl_rds_time - RDS time-of-day decoder for RTL-SDR
 * Decodes RDS Group 4A (CT - Clock Time) from FM broadcast stations.
 *
 * Based on rtl_fm architecture from rtl-sdr project.
 * Copyright (C) 2026 - GPL v2+
 *
 * Usage: rtl_rds_time -f <freq_hz> [-d device_index] [-g gain] [-p ppm]
 * Example: rtl_rds_time -f 98.1M
 *
 * RDS signal chain:
 *   RTL2832U @ 228kHz → FM demod → 57kHz BPSK extraction → RDS decode → CT group
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include "getopt/getopt.h"
#define usleep(x) Sleep(x/1000)
#define _USE_MATH_DEFINES
#endif

#include <pthread.h>
#include <libusb.h>

#include "rtl-sdr.h"
#include "convenience/convenience.h"

/* ─── Configuration ─────────────────────────────────────────────────── */

/* Capture at 228kHz - gives us baseband up to 114kHz after FM demod,
 * enough for the 57kHz RDS subcarrier with margin */
#define CAPTURE_RATE    228000

/* FM demod output rate = capture rate (no decimation needed at this BW) */
#define FM_RATE         228000

/* RDS parameters per EN 50067 / IEC 62106 */
#define RDS_CARRIER_HZ  57000.0
#define RDS_BITRATE     1187.5
#define PILOT_HZ        19000.0

/* Samples per RDS bit at FM_RATE */
#define SAMPLES_PER_BIT ((int)(FM_RATE / RDS_BITRATE))  /* 192 */

/* Buffer sizes */
#define BUF_LENGTH      (16 * 16384)
#define MAX_FM_LEN      (BUF_LENGTH)

static volatile int do_exit = 0;
static rtlsdr_dev_t *dev = NULL;

/* ─── Signal handler ────────────────────────────────────────────────── */

#ifndef _WIN32
static void sighandler(int signum)
{
	(void)signum;
	fprintf(stderr, "\nSignal caught, exiting...\n");
	do_exit = 1;
	rtlsdr_cancel_async(dev);
}
#else
BOOL WINAPI sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "\nSignal caught, exiting...\n");
		do_exit = 1;
		rtlsdr_cancel_async(dev);
		return TRUE;
	}
	return FALSE;
}
#endif

/* ─── FM Demodulator ────────────────────────────────────────────────── */

static int fm_pre_r = 0, fm_pre_j = 0;

static int fm_demod_sample(int ar, int aj, int br, int bj)
{
	/* polar discriminator: arg(z[n] * conj(z[n-1])) */
	int cr = ar * br + aj * bj;
	int cj = aj * br - ar * bj;
	return (int)(atan2((double)cj, (double)cr) / M_PI * 32768.0);
}

static int fm_demodulate(int16_t *iq, int iq_len, int16_t *audio)
{
	int i, out_len = 0;
	int ar, aj;

	ar = iq[0]; aj = iq[1];
	audio[0] = (int16_t)fm_demod_sample(ar, aj, fm_pre_r, fm_pre_j);
	out_len = 1;

	for (i = 2; i < iq_len; i += 2) {
		audio[out_len] = (int16_t)fm_demod_sample(iq[i], iq[i+1], iq[i-2], iq[i-1]);
		out_len++;
	}

	fm_pre_r = iq[iq_len - 2];
	fm_pre_j = iq[iq_len - 1];
	return out_len;
}

/* ─── RDS Decoder State ─────────────────────────────────────────────── */

/* 57 kHz bandpass filter state (2nd order IIR) */
struct bpf_state {
	double x1, x2, y1, y2;
};

/* Costas loop for carrier recovery + BPSK demod */
struct costas_state {
	double phase;
	double freq;       /* in radians/sample */
	double alpha, beta; /* loop filter gains */
};

/* RDS bit recovery */
struct clock_recovery {
	double phase;       /* bit clock phase accumulator */
	double freq;        /* nominal: 2*pi*1187.5/228000 */
	int last_sign;
	int bits_count;
};

/* RDS group assembly */
#define RDS_BLOCK_LEN 26  /* 16 data + 10 check bits */
struct rds_state {
	uint32_t shift_reg;
	int bit_count;
	int block_count;
	uint16_t blocks[4]; /* A, B, C/C', D */
	int synced;
	int good_blocks;
	int bad_blocks;
};

static struct bpf_state bpf = {0};
static struct costas_state costas = {0};
static struct clock_recovery clk = {0};
static struct rds_state rds = {0};

/* ─── 57 kHz Bandpass Filter ────────────────────────────────────────── */
/* 2nd order IIR bandpass, center=57kHz, BW≈4kHz at fs=228kHz
 * Designed with bilinear transform */

/* Pre-computed coefficients for 57kHz BPF at 228kHz sample rate
 * Q ≈ 14 (BW ≈ 4 kHz) */
static const double bpf_b0 =  0.006761;
static const double bpf_b2 = -0.006761;
static const double bpf_a1 = -0.000000;  /* cos(2*pi*57000/228000) = cos(pi/2) = 0 */
static const double bpf_a2 = -0.986478;

static double bandpass_57k(double x)
{
	double y = bpf_b0 * x + bpf_b2 * bpf.x2 - bpf_a1 * bpf.y1 - bpf_a2 * bpf.y2;
	bpf.x2 = bpf.x1;
	bpf.x1 = x;
	bpf.y2 = bpf.y1;
	bpf.y1 = y;
	return y;
}

/* ─── Costas Loop (BPSK carrier recovery + demod) ───────────────────── */

static void costas_init(struct costas_state *c)
{
	double bw, damp;
	c->phase = 0.0;
	c->freq = 2.0 * M_PI * RDS_CARRIER_HZ / FM_RATE;
	/* Loop bandwidth ~30 Hz for stable lock */
	bw = 2.0 * M_PI * 30.0 / FM_RATE;
	damp = 0.707;
	c->alpha = 2.0 * damp * bw;
	c->beta = bw * bw;
}

static double costas_process(struct costas_state *c, double input)
{
	/* Mix with local oscillator */
	double i_out = input * cos(c->phase);
	double q_out = input * -sin(c->phase);

	/* BPSK error signal: sign(I) * Q */
	double error = (i_out > 0 ? 1.0 : -1.0) * q_out;

	/* Loop filter (2nd order) */
	c->freq += c->beta * error;
	c->phase += c->freq + c->alpha * error;

	/* Wrap phase */
	while (c->phase > 2.0 * M_PI) c->phase -= 2.0 * M_PI;
	while (c->phase < 0.0) c->phase += 2.0 * M_PI;

	return i_out; /* demodulated BPSK baseband */
}

/* ─── Clock Recovery (Gardner timing) ──────────────────────────────── */

static void clock_init(struct clock_recovery *c)
{
	c->phase = 0.0;
	c->freq = 2.0 * M_PI * RDS_BITRATE / FM_RATE;
	c->last_sign = 0;
	c->bits_count = 0;
}

static int prev_sample = 0;
static double bit_accum = 0.0;
static int bit_sample_count = 0;

/* Returns 1 if a bit decision was made, stores bit in *out_bit */
static int clock_process(struct clock_recovery *c, double sample, int *out_bit)
{
	bit_accum += sample;
	bit_sample_count++;

	c->phase += c->freq;
	if (c->phase >= 2.0 * M_PI) {
		c->phase -= 2.0 * M_PI;
		/* Bit decision point */
		*out_bit = (bit_accum > 0) ? 1 : 0;
		bit_accum = 0.0;
		bit_sample_count = 0;
		c->bits_count++;
		return 1;
	}
	return 0;
}

/* ─── RDS Syndrome Check (EN 50067) ─────────────────────────────────── */

/* RDS check word generator polynomial: x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1 */
#define RDS_POLY 0x1B9
/* Offset words for blocks A, B, C, C', D */
static const uint16_t rds_offsets[5] = {0x0FC, 0x198, 0x168, 0x350, 0x1B4};

static uint16_t rds_syndrome(uint32_t block)
{
	int i;
	uint32_t reg = 0;
	/* Process 26 bits (MSB first) */
	for (i = 25; i >= 0; i--) {
		reg = (reg << 1) | ((block >> i) & 1);
		if (reg & (1 << 10))
			reg ^= RDS_POLY;
	}
	return (uint16_t)(reg & 0x3FF);
}

static int rds_check_block(uint32_t block, int block_num)
{
	uint16_t syn = rds_syndrome(block);
	if (syn == rds_offsets[block_num])
		return 1;
	/* Try C' offset for block C */
	if (block_num == 2 && syn == rds_offsets[3])
		return 1;
	return 0;
}

/* ─── RDS Group 4A Decoder (Clock-Time) ─────────────────────────────── */

static void decode_group_4a(uint16_t *blocks)
{
	/* Group 4A: Clock-time and date
	 * Block B bits 0-1: day (high 2 bits)
	 * Block C: MJD (Modified Julian Day) bits 14-0 split across B[1:0] and C
	 * Block D: hours, minutes, UTC offset
	 *
	 * Layout:
	 *   B[1:0] = MJD bits 16:15
	 *   C[15:1] = MJD bits 14:0
	 *   C[0] = hours bit 4
	 *   D[15:12] = hours bits 3:0
	 *   D[11:6] = minutes
	 *   D[5] = UTC offset sign (0=positive, 1=negative)
	 *   D[4:0] = UTC offset in half-hours
	 */

	uint32_t mjd;
	int hours, minutes;
	int offset_sign, offset_half_hours;
	double offset_hours;
	int year, month, day;
	double yp, mp;

	/* Extract MJD (17 bits) */
	mjd = ((uint32_t)(blocks[1] & 0x03) << 15) | ((uint32_t)blocks[2] >> 1);

	/* Extract time */
	hours = ((blocks[2] & 0x01) << 4) | ((blocks[3] >> 12) & 0x0F);
	minutes = (blocks[3] >> 6) & 0x3F;
	offset_sign = (blocks[3] >> 5) & 0x01;
	offset_half_hours = blocks[3] & 0x1F;

	/* Sanity checks */
	if (hours > 23 || minutes > 59 || offset_half_hours > 24) {
		return;
	}
	if (mjd < 40000 || mjd > 80000) {
		return;
	}

	/* Convert MJD to calendar date (EN 50067 Annex G) */
	yp = (double)((int)mjd - 15078.2) / 365.25;
	mp = (double)((int)mjd - 14956.1 - (int)(yp * 365.25)) / 30.6001;
	day = (int)mjd - 14956 - (int)(yp * 365.25) - (int)(mp * 30.6001);
	if (mp == 14.0 || mp == 15.0) {
		year = (int)yp + 1;
		month = (int)mp - 13;
	} else {
		year = (int)yp;
		month = (int)mp - 1;
	}
	year += 1900;

	offset_hours = offset_half_hours * 0.5;
	if (offset_sign)
		offset_hours = -offset_hours;

	fprintf(stderr, "\n══════════════════════════════════════════\n");
	fprintf(stderr, "  RDS Clock-Time (Group 4A) received!\n");
	fprintf(stderr, "  UTC:  %04d-%02d-%02d %02d:%02d\n",
		year, month, day, hours, minutes);
	fprintf(stderr, "  Local offset: %+.1f hours\n", offset_hours);

	/* Compute local time */
	{
		int local_minutes = hours * 60 + minutes + (int)(offset_hours * 60);
		if (local_minutes < 0) local_minutes += 1440;
		if (local_minutes >= 1440) local_minutes -= 1440;
		fprintf(stderr, "  Local: %02d:%02d\n", local_minutes / 60, local_minutes % 60);
	}
	fprintf(stderr, "══════════════════════════════════════════\n\n");
}

/* ─── RDS Bit Processing ────────────────────────────────────────────── */

static int prev_bit = 0;

static void rds_process_bit(int bit)
{
	/* RDS uses differential encoding: data = current XOR previous */
	int data_bit = bit ^ prev_bit;
	prev_bit = bit;

	/* Shift into 26-bit register */
	rds.shift_reg = ((rds.shift_reg << 1) | data_bit) & 0x03FFFFFF;
	rds.bit_count++;

	if (!rds.synced) {
		/* Try to sync: check if current 26 bits match any block offset */
		int i;
		for (i = 0; i < 4; i++) {
			if (rds_check_block(rds.shift_reg, i)) {
				rds.synced = 1;
				rds.block_count = i;
				rds.blocks[i] = (uint16_t)(rds.shift_reg >> 10);
				rds.block_count = (i + 1) % 4;
				rds.bit_count = 0;
				rds.good_blocks++;
				fprintf(stderr, "RDS: synced on block %c\n", "ABCD"[i]);
				return;
			}
		}
	} else {
		/* Synced: collect bits until we have 26 */
		if (rds.bit_count >= 26) {
			rds.bit_count = 0;
			if (rds_check_block(rds.shift_reg, rds.block_count)) {
				rds.blocks[rds.block_count] = (uint16_t)(rds.shift_reg >> 10);
				rds.good_blocks++;

				/* If we just completed block D, we have a full group */
				if (rds.block_count == 3) {
					uint16_t group_type = (rds.blocks[1] >> 11) & 0x1F;
					int version = (rds.blocks[1] >> 10) & 0x01;

					fprintf(stderr, "RDS Group %d%c | PI=%04X\n",
						group_type, version ? 'B' : 'A', rds.blocks[0]);

					/* Group 4A = Clock-Time and date */
					if (group_type == 4 && version == 0) {
						decode_group_4a(rds.blocks);
					}
				}
			} else {
				rds.bad_blocks++;
				/* Lose sync after too many errors */
				if (rds.bad_blocks > 10 && rds.bad_blocks > rds.good_blocks) {
					fprintf(stderr, "RDS: lost sync (%d good, %d bad)\n",
						rds.good_blocks, rds.bad_blocks);
					rds.synced = 0;
					rds.good_blocks = 0;
					rds.bad_blocks = 0;
				}
			}
			rds.block_count = (rds.block_count + 1) % 4;
		}
	}
}

/* ─── Main Processing Callback ──────────────────────────────────────── */

static int16_t iq_buf[BUF_LENGTH];
static int16_t fm_buf[BUF_LENGTH / 2];

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	uint32_t i;
	int fm_len, bit;
	double filtered, demod_out;

	(void)ctx;
	if (do_exit) return;

	/* Convert uint8 IQ to int16, apply 90° rotation for DC offset avoidance */
	/* rotate_90: multiply by e^{-j*pi/2*n} to shift spectrum down by fs/4 */
	for (i = 0; i < len; i += 8) {
		/* Phase 0: × (1+0j) */
		iq_buf[i]   = (int16_t)buf[i] - 127;
		iq_buf[i+1] = (int16_t)buf[i+1] - 127;
		/* Phase 1: × (0-j) → swap and negate */
		iq_buf[i+2] = (int16_t)buf[i+3] - 127;
		iq_buf[i+3] = 127 - (int16_t)buf[i+2];
		/* Phase 2: × (-1+0j) */
		iq_buf[i+4] = 127 - (int16_t)buf[i+4];
		iq_buf[i+5] = 127 - (int16_t)buf[i+5];
		/* Phase 3: × (0+j) */
		iq_buf[i+6] = 127 - (int16_t)buf[i+7];
		iq_buf[i+7] = (int16_t)buf[i+6] - 127;
	}

	/* FM demodulate → composite baseband at 228 kHz */
	fm_len = fm_demodulate(iq_buf, (int)len, fm_buf);

	/* Process each FM baseband sample through RDS chain */
	for (i = 0; i < (uint32_t)fm_len; i++) {
		/* Bandpass filter around 57 kHz */
		filtered = bandpass_57k((double)fm_buf[i]);

		/* Costas loop: carrier recovery + BPSK demodulation */
		demod_out = costas_process(&costas, filtered);

		/* Clock recovery: extract bits at 1187.5 bps */
		if (clock_process(&clk, demod_out, &bit)) {
			rds_process_bit(bit);
		}
	}
}

/* ─── Main ──────────────────────────────────────────────────────────── */

void usage(void)
{
	fprintf(stderr,
		"rtl_rds_time - RDS Clock-Time decoder\n\n"
		"Usage: rtl_rds_time -f freq [options]\n"
		"\t-f frequency (Hz, supports k/M/G suffixes)\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-g gain in dB (default: automatic)]\n"
		"\t[-p ppm_error (default: 0)]\n"
		"\t[-T enable bias-T]\n\n"
		"Example: rtl_rds_time -f 98.1M\n"
		"Listens for RDS Group 4A and prints UTC time when received.\n"
		"Most stations transmit CT once per minute.\n\n");
	exit(1);
}

int main(int argc, char **argv)
{
	int r, opt;
	int dev_index = 0;
	int dev_given = 0;
	int gain = -100; /* auto */
	int ppm = 0;
	int enable_biastee = 0;
	uint32_t freq = 0;
	uint32_t capture_freq;

#ifndef _WIN32
	struct sigaction sigact;
#endif

	while ((opt = getopt(argc, argv, "f:d:g:p:Th")) != -1) {
		switch (opt) {
		case 'f':
			freq = (uint32_t)atofs(optarg);
			break;
		case 'd':
			dev_index = verbose_device_search(optarg);
			dev_given = 1;
			break;
		case 'g':
			gain = (int)(atof(optarg) * 10);
			break;
		case 'p':
			ppm = atoi(optarg);
			break;
		case 'T':
			enable_biastee = 1;
			break;
		case 'h':
		default:
			usage();
		}
	}

	if (freq == 0) {
		fprintf(stderr, "Error: please specify a frequency with -f\n");
		usage();
	}

	if (!dev_given) {
		dev_index = verbose_device_search("0");
	}
	if (dev_index < 0) {
		exit(1);
	}

	/* Initialize DSP state */
	costas_init(&costas);
	clock_init(&clk);
	memset(&rds, 0, sizeof(rds));
	memset(&bpf, 0, sizeof(bpf));

	/* Open device */
	r = rtlsdr_open(&dev, (uint32_t)dev_index);
	if (r < 0) {
		fprintf(stderr, "Failed to open device #%d.\n", dev_index);
		exit(1);
	}

	/* Signal handlers */
#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
#else
	SetConsoleCtrlHandler((PHANDLER_ROUTINE)sighandler, TRUE);
#endif

	/* Configure device */
	if (gain == -100) {
		verbose_auto_gain(dev);
	} else {
		gain = nearest_gain(dev, gain);
		verbose_gain_set(dev, gain);
	}

	verbose_ppm_set(dev, ppm);
	rtlsdr_set_bias_tee(dev, enable_biastee);

	/* Tune: offset by capture_rate/4 to avoid DC spike */
	capture_freq = freq + CAPTURE_RATE / 4;
	verbose_set_frequency(dev, capture_freq);
	verbose_set_sample_rate(dev, CAPTURE_RATE);
	verbose_reset_buffer(dev);

	fprintf(stderr, "\nListening for RDS on %.1f MHz...\n", freq / 1e6);
	fprintf(stderr, "Capture rate: %d Hz, tuned to %u Hz (offset +%d Hz)\n",
		CAPTURE_RATE, capture_freq, CAPTURE_RATE / 4);
	fprintf(stderr, "Waiting for Group 4A (Clock-Time)...\n");
	fprintf(stderr, "(Most stations transmit CT once per minute)\n\n");

	/* Start async read - blocks until cancelled */
	r = rtlsdr_read_async(dev, rtlsdr_callback, NULL, 0, BUF_LENGTH);

	/* Cleanup */
	rtlsdr_close(dev);
	fprintf(stderr, "\nRDS stats: %d good blocks, %d bad blocks\n",
		rds.good_blocks, rds.bad_blocks);
	return r >= 0 ? 0 : 1;
}
