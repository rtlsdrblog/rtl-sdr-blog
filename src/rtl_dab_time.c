/*
 * rtl_dab_time.c - DAB FIC time extractor and system clock discipliner
 *
 * Decodes FIG 0/10 from DAB broadcasts and sets/disciplines the Linux
 * system clock using adjtimex(). Intended as a lightweight NTP replacement
 * for systems with DAB reception.
 *
 * Features:
 *   - Auto-scan: scans all DAB Band III blocks to find a signal
 *   - Manual frequency: use -f to specify a known block
 *   - Clock discipline via adjtimex() (slew) or clock_settime() (step)
 *
 * Usage: rtl_dab_time [-f freq] [-d device] [-g gain] [-p ppm] [-s] [-1]
 *
 * Requires: root (or CAP_SYS_TIME) to set the system clock.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/timex.h>
#else
#error "rtl_dab_time is Linux-only (requires adjtimex)"
#endif

#include <pthread.h>
#include <libusb.h>

#include "rtl-sdr.h"
#include "convenience/convenience.h"
#include "dab_time/dab_time.h"

/* ─── DAB Band III Channel Table ────────────────────────────────────── */

struct dab_channel {
	const char *name;
	uint32_t freq_hz;
};

static const struct dab_channel dab_channels[] = {
	{"5A",  174928000}, {"5B",  176640000}, {"5C",  178352000}, {"5D",  180064000},
	{"6A",  181936000}, {"6B",  183648000}, {"6C",  185360000}, {"6D",  187072000},
	{"7A",  188928000}, {"7B",  190640000}, {"7C",  192352000}, {"7D",  194064000},
	{"8A",  195936000}, {"8B",  197648000}, {"8C",  199360000}, {"8D",  201072000},
	{"9A",  202928000}, {"9B",  204640000}, {"9C",  206352000}, {"9D",  208064000},
	{"10A", 209936000}, {"10B", 211648000}, {"10C", 213360000}, {"10D", 215072000},
	{"11A", 216928000}, {"11B", 218640000}, {"11C", 220352000}, {"11D", 222064000},
	{"12A", 223936000}, {"12B", 225648000}, {"12C", 227360000}, {"12D", 229072000},
	{NULL, 0}
};

#define NUM_DAB_CHANNELS 32

/* ─── Configuration ─────────────────────────────────────────────────── */

#define BUF_LEN         (DAB_T_F * 2)  /* One full DAB frame, complex */
#define MAX_FRAMES      10             /* Give up after this many frames without sync */
#define SCAN_DWELL_MS   800            /* Time to dwell on each channel during scan */
#define SCAN_SETTLE_MS  100            /* Settling time after retune (AGC) */
#define SCAN_ATTEMPTS   2              /* Read attempts per channel */

static volatile int do_exit = 0;
static rtlsdr_dev_t *dev = NULL;

/* ─── Clock discipline ──────────────────────────────────────────────── */

#define SLEW_THRESHOLD_US  500000  /* 0.5 seconds: step vs slew */

static void apply_time(struct dab_time *t, int set_only)
{
	struct timespec ts_dab, ts_now;
	struct tm tm_dab;
	long offset_us;
	struct timex tx;

	memset(&tm_dab, 0, sizeof(tm_dab));
	tm_dab.tm_year = t->year - 1900;
	tm_dab.tm_mon  = t->month - 1;
	tm_dab.tm_mday = t->day;
	tm_dab.tm_hour = t->hours;
	tm_dab.tm_min  = t->minutes;
	tm_dab.tm_sec  = t->seconds;
	tm_dab.tm_isdst = 0;

	ts_dab.tv_sec = timegm(&tm_dab);
	ts_dab.tv_nsec = (t->milliseconds > 0) ? (long)t->milliseconds * 1000000L : 0;

	if (ts_dab.tv_sec < 0) {
		fprintf(stderr, "Error: invalid time from DAB\n");
		return;
	}

	clock_gettime(CLOCK_REALTIME, &ts_now);

	offset_us = (long)(ts_dab.tv_sec - ts_now.tv_sec) * 1000000L
	          + (long)(ts_dab.tv_nsec - ts_now.tv_nsec) / 1000L;

	fprintf(stderr, "DAB time: %04d-%02d-%02d %02d:%02d:%02d",
		t->year, t->month, t->day, t->hours, t->minutes, t->seconds);
	if (t->milliseconds >= 0)
		fprintf(stderr, ".%03d", t->milliseconds);
	fprintf(stderr, " UTC\n");
	fprintf(stderr, "System offset: %+ld µs\n", offset_us);

	if (set_only || labs(offset_us) > SLEW_THRESHOLD_US) {
		if (clock_settime(CLOCK_REALTIME, &ts_dab) == 0) {
			fprintf(stderr, "Clock stepped by %+ld µs\n", offset_us);
		} else {
			perror("clock_settime failed (need root/CAP_SYS_TIME?)");
		}
		return;
	}

	memset(&tx, 0, sizeof(tx));
	tx.modes = ADJ_OFFSET | ADJ_STATUS;
	tx.offset = offset_us;
	tx.status = STA_PLL;

	if (adjtimex(&tx) < 0) {
		perror("adjtimex failed");
		clock_settime(CLOCK_REALTIME, &ts_dab);
	} else {
		fprintf(stderr, "Clock slewed by %+ld µs\n", offset_us);
	}
}

/* ─── Signal handler ────────────────────────────────────────────────── */

static void sighandler(int signum)
{
	(void)signum;
	do_exit = 1;
	rtlsdr_cancel_async(dev);
}

/* ─── Receive buffer ────────────────────────────────────────────────── */

static cfloat sample_buf[BUF_LEN * 2];
static int sample_buf_fill = 0;
static pthread_mutex_t buf_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t buf_ready = PTHREAD_COND_INITIALIZER;
static int buf_ready_flag = 0;

static int cb_count = 0;

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	uint32_t i;
	(void)ctx;
	if (do_exit) return;

	if (cb_count < 3) {
		fprintf(stderr, "[cb %d len=%u fill=%d]\n", cb_count, len, sample_buf_fill);
		cb_count++;
	}

	pthread_mutex_lock(&buf_mutex);
	for (i = 0; i < len && sample_buf_fill < BUF_LEN * 2; i += 2) {
		sample_buf[sample_buf_fill++] =
			((float)buf[i] - 127.5f) + I * ((float)buf[i+1] - 127.5f);
	}
	if (sample_buf_fill >= BUF_LEN) {
		buf_ready_flag = 1;
		pthread_cond_signal(&buf_ready);
	}
	pthread_mutex_unlock(&buf_mutex);
}

/* ─── DAB Signal Detection ──────────────────────────────────────────
 * Checks if a buffer contains a DAB signal by looking for the null symbol.
 * Returns 1 if DAB detected, 0 otherwise. */

static int detect_dab_signal(cfloat *buf, int len)
{
	return ofdm_find_null(buf, len) >= 0;
}

/* ─── Channel Scanner ───────────────────────────────────────────────
 * Scans all DAB Band III channels, dwells on each for SCAN_DWELL_MS,
 * returns the index of the first channel with a DAB signal, or -1. */

static int scan_channels(int gain, int ppm, int enable_biastee, int start_ch)
{
	int ch, found = -1;
	int n_read;
	unsigned char *scan_buf;
	cfloat *scan_cfloat;
	int scan_samples = (DAB_SAMPLE_RATE * SCAN_DWELL_MS) / 1000;
	int scan_bytes = scan_samples * 2;  /* I + Q bytes */
	int i, attempt;

	scan_buf = (unsigned char *)malloc(scan_bytes);
	scan_cfloat = (cfloat *)malloc(scan_samples * sizeof(cfloat));
	if (!scan_buf || !scan_cfloat) {
		fprintf(stderr, "Error: scan buffer allocation failed\n");
		free(scan_buf);
		free(scan_cfloat);
		return -1;
	}

	fprintf(stderr, "Scanning DAB Band III (from channel %d/%d)...\n",
		start_ch + 1, NUM_DAB_CHANNELS);

	for (ch = start_ch; ch < NUM_DAB_CHANNELS && !do_exit; ch++) {
		fprintf(stderr, "  Block %s (%6.3f MHz) ... ",
			dab_channels[ch].name, dab_channels[ch].freq_hz / 1e6);

		rtlsdr_set_center_freq(dev, dab_channels[ch].freq_hz);
		rtlsdr_reset_buffer(dev);

		/* Wait for AGC/PLL to settle after retune */
		usleep(SCAN_SETTLE_MS * 1000);

		/* Discard first read to flush stale samples */
		rtlsdr_read_sync(dev, scan_buf, scan_bytes, &n_read);

		for (attempt = 0; attempt < SCAN_ATTEMPTS; attempt++) {
			if (rtlsdr_read_sync(dev, scan_buf, scan_bytes, &n_read) < 0) {
				fprintf(stderr, "read error\n");
				break;
			}

			/* Convert to complex float */
			for (i = 0; i < (int)n_read / 2 && i < scan_samples; i++) {
				scan_cfloat[i] = ((float)scan_buf[i*2] - 127.5f)
				               + I * ((float)scan_buf[i*2+1] - 127.5f);
			}

			/* Check for null symbol (DAB signal indicator) */
			if (detect_dab_signal(scan_cfloat, i)) {
				fprintf(stderr, "DAB FOUND!\n");
				found = ch;
				break;
			}
		}

		if (found >= 0) break;
		fprintf(stderr, "no signal\n");
	}

	free(scan_buf);
	free(scan_cfloat);
	return found;
}

/* ─── Processing thread ─────────────────────────────────────────────── */

struct proc_args {
	int set_only;
	int one_shot;
	int times_set;
	uint32_t active_freq;
	const char *active_block;
	int rescan_needed;
};

static void *processing_thread(void *arg)
{
	struct proc_args *a = (struct proc_args *)arg;
	struct ofdm_state ofdm;
	struct fic_state fic;
	struct dab_time t;
	cfloat *frame_buf;
	uint8_t soft_bits[FIC_BITS_PER_SYMBOL * DAB_NUM_FIC_SYMBOLS];
	uint8_t fib_data[30 * NUM_FIBS];
	int null_pos, valid_fibs;
	int frames_without_sync = 0;
	int soft_bits_len = 0;

	fprintf(stderr, "[proc thread started]\n");

	frame_buf = (cfloat *)malloc(BUF_LEN * sizeof(cfloat));
	if (!frame_buf) {
		fprintf(stderr, "Error: frame buffer allocation failed\n");
		return NULL;
	}

	ofdm_init(&ofdm);
	fic_init(&fic);

	while (!do_exit) {
		int pos, sym, fib;

		pthread_mutex_lock(&buf_mutex);
		while (!buf_ready_flag && !do_exit)
			pthread_cond_wait(&buf_ready, &buf_mutex);
		if (do_exit) { pthread_mutex_unlock(&buf_mutex); break; }

		memcpy(frame_buf, sample_buf, BUF_LEN * sizeof(cfloat));
		sample_buf_fill = 0;
		buf_ready_flag = 0;
		pthread_mutex_unlock(&buf_mutex);

		fprintf(stderr, "F");

		null_pos = ofdm_find_null(frame_buf, BUF_LEN);
		if (null_pos < 0) {
			fprintf(stderr, "N");
			frames_without_sync++;
			if (frames_without_sync > MAX_FRAMES) {
				fprintf(stderr, "Lost DAB signal on block %s (%.3f MHz)\n",
					a->active_block, a->active_freq / 1e6);
				a->rescan_needed = 1;
				rtlsdr_cancel_async(dev);
				free(frame_buf);
				return NULL;
			}
			continue;
		}

		if (frames_without_sync > 0)
			fprintf(stderr, "OFDM sync acquired (null at sample %d)\n", null_pos);
		frames_without_sync = 0;

		pos = null_pos + DAB_T_NULL;
		if (pos + (DAB_NUM_FIC_SYMBOLS + 1) * DAB_T_S > BUF_LEN) {
			fprintf(stderr, "B");
			continue;
		}

		/* PRS - store as phase reference */
		ofdm_demod_symbol(&ofdm, &frame_buf[pos], NULL);
		pos += DAB_T_S;

		/* FIC symbols */
		soft_bits_len = 0;
		for (sym = 0; sym < DAB_NUM_FIC_SYMBOLS; sym++) {
			if (pos + DAB_T_S > BUF_LEN) break;
			ofdm_demod_symbol(&ofdm, &frame_buf[pos],
					  &soft_bits[sym * FIC_BITS_PER_SYMBOL]);
			soft_bits_len += FIC_BITS_PER_SYMBOL;
			pos += DAB_T_S;
		}

		if (soft_bits_len < FIC_BITS_PER_SYMBOL * DAB_NUM_FIC_SYMBOLS) continue;

		valid_fibs = fic_decode(soft_bits, soft_bits_len, fib_data);
		if (valid_fibs == 0) {
			fprintf(stderr, ".");  /* FIC CRC failed - show progress */
			continue;
		}
		fprintf(stderr, "\nFIC decoded: %d valid FIB(s)\n", valid_fibs);

		for (fib = 0; fib < valid_fibs; fib++) {
			memset(&t, 0, sizeof(t));
			if (fig_parse_time(&fib_data[fib * 30], 30, &t)) {
				apply_time(&t, a->set_only);
				a->times_set++;
				if (a->one_shot) {
					do_exit = 1;
					rtlsdr_cancel_async(dev);
					free(frame_buf);
					return NULL;
				}
			}
		}
	}
	free(frame_buf);
	return NULL;
}

/* ─── Main ──────────────────────────────────────────────────────────── */

void usage(void)
{
	fprintf(stderr,
		"rtl_dab_time - DAB FIC time extractor / NTP replacement\n\n"
		"Usage: rtl_dab_time [options]\n"
		"\t[-f frequency in Hz (default: auto-scan Band III)]\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-g gain in dB (default: automatic)]\n"
		"\t[-p ppm_error (default: 0)]\n"
		"\t[-s step clock only, no slewing (default: slew if offset < 0.5s)]\n"
		"\t[-1 one-shot: exit after first successful time decode]\n"
		"\t[-T enable bias-T]\n\n"
		"If -f is not specified, all 32 DAB Band III channels are scanned\n"
		"and the first channel with a valid signal is used.\n\n"
		"Examples:\n"
		"\trtl_dab_time                       # auto-scan, continuous\n"
		"\trtl_dab_time -1                    # auto-scan, set clock, exit\n"
		"\trtl_dab_time -f 202.928M           # manual freq, continuous\n"
		"\trtl_dab_time -f 202.928M -1        # manual freq, one-shot\n\n"
		"Requires root or CAP_SYS_TIME to set the system clock.\n"
		"DAB FIG 0/10 provides millisecond-accurate UTC time.\n\n"
		"DAB Band III channels:\n");

	{
		int i;
		for (i = 0; i < NUM_DAB_CHANNELS; i += 4) {
			fprintf(stderr, "\t  %3s: %7.3f MHz   %3s: %7.3f MHz"
				"   %3s: %7.3f MHz   %3s: %7.3f MHz\n",
				dab_channels[i].name, dab_channels[i].freq_hz / 1e6,
				dab_channels[i+1].name, dab_channels[i+1].freq_hz / 1e6,
				dab_channels[i+2].name, dab_channels[i+2].freq_hz / 1e6,
				dab_channels[i+3].name, dab_channels[i+3].freq_hz / 1e6);
		}
	}
	fprintf(stderr, "\n");
	exit(1);
}

int main(int argc, char **argv)
{
	int r, opt;
	int dev_index = 0, dev_given = 0;
	int gain = -100;
	int ppm = 0;
	int enable_biastee = 0;
	uint32_t freq = 0;
	const char *block_name = NULL;
	struct sigaction sigact;
	pthread_t proc_thread;
	struct proc_args args;

	memset(&args, 0, sizeof(args));

	while ((opt = getopt(argc, argv, "f:d:g:p:s1Th")) != -1) {
		switch (opt) {
		case 'f': freq = (uint32_t)atofs(optarg); break;
		case 'd': dev_index = verbose_device_search(optarg); dev_given = 1; break;
		case 'g': gain = (int)(atof(optarg) * 10); break;
		case 'p': ppm = atoi(optarg); break;
		case 's': args.set_only = 1; break;
		case '1': args.one_shot = 1; break;
		case 'T': enable_biastee = 1; break;
		case 'h': default: usage();
		}
	}

	if (!dev_given) dev_index = verbose_device_search("0");
	if (dev_index < 0) exit(1);

	r = rtlsdr_open(&dev, (uint32_t)dev_index);
	if (r < 0) { fprintf(stderr, "Failed to open device #%d\n", dev_index); exit(1); }

	/* Signal handlers */
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);

	/* Configure device */
	if (gain == -100) verbose_auto_gain(dev);
	else { gain = nearest_gain(dev, gain); verbose_gain_set(dev, gain); }

	verbose_ppm_set(dev, ppm);
	rtlsdr_set_bias_tee(dev, enable_biastee);
	rtlsdr_set_sample_rate(dev, DAB_SAMPLE_RATE);

	{
	int ch_idx = 0;
	int manual_freq = (freq != 0);

	/* Auto-scan if no frequency specified */
	if (!manual_freq) {
		ch_idx = scan_channels(gain, ppm, enable_biastee, 0);
		if (ch_idx < 0) {
			fprintf(stderr, "\nNo DAB signal found on any channel.\n");
			fprintf(stderr, "Check antenna connection and try with manual gain (-g 40).\n");
			rtlsdr_close(dev);
			exit(1);
		}
		freq = dab_channels[ch_idx].freq_hz;
		block_name = dab_channels[ch_idx].name;
		fprintf(stderr, "\n══════════════════════════════════════════\n");
		fprintf(stderr, "  DAB signal found: Block %s (%.3f MHz)\n",
			block_name, freq / 1e6);
		fprintf(stderr, "══════════════════════════════════════════\n\n");
	} else {
		int i;
		for (i = 0; i < NUM_DAB_CHANNELS; i++) {
			if (dab_channels[i].freq_hz == freq) {
				block_name = dab_channels[i].name;
				ch_idx = i;
				break;
			}
		}
		if (!block_name) block_name = "?";
	}

	while (!do_exit) {
		args.active_freq = freq;
		args.active_block = block_name;
		args.rescan_needed = 0;

		verbose_set_frequency(dev, freq);
		verbose_reset_buffer(dev);

		fprintf(stderr, "Receiving DAB block %s (%.3f MHz)\n", block_name, freq / 1e6);
		fprintf(stderr, "Waiting for FIG 0/10 time data...\n\n");

		sample_buf_fill = 0;
		buf_ready_flag = 0;

		pthread_create(&proc_thread, NULL, processing_thread, &args);
		r = rtlsdr_read_async(dev, rtlsdr_callback, NULL, 0, 65536);
		pthread_mutex_lock(&buf_mutex);
		buf_ready_flag = 1;
		pthread_cond_signal(&buf_ready);
		pthread_mutex_unlock(&buf_mutex);
		pthread_join(proc_thread, NULL);

		if (!args.rescan_needed || do_exit || manual_freq)
			break;

		/* Rescan from next channel */
		fprintf(stderr, "\nRescanning from next channel...\n");
		ch_idx = scan_channels(gain, ppm, enable_biastee,
				       (ch_idx + 1) % NUM_DAB_CHANNELS);
		if (ch_idx < 0)
			ch_idx = scan_channels(gain, ppm, enable_biastee, 0);
		if (ch_idx < 0) {
			fprintf(stderr, "\nNo DAB signal found on any channel.\n");
			break;
		}
		freq = dab_channels[ch_idx].freq_hz;
		block_name = dab_channels[ch_idx].name;
		fprintf(stderr, "\n══════════════════════════════════════════\n");
		fprintf(stderr, "  DAB signal found: Block %s (%.3f MHz)\n",
			block_name, freq / 1e6);
		fprintf(stderr, "══════════════════════════════════════════\n\n");
	}
	}

	rtlsdr_close(dev);
	fprintf(stderr, "\nTotal clock updates applied: %d\n", args.times_set);
	return 0;
}
