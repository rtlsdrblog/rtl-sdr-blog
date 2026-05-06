/*
 * dab_time_cli.cpp - DAB time extraction for system clock discipline
 *
 * Extracts FIG 0/10 UTC time with millisecond accuracy from DAB broadcasts
 * and sets the Linux system clock. Lightweight NTP replacement.
 *
 * Supports: direct RTL-SDR device or rtl_tcp remote connection.
 * Auto-scans DAB Band III channels if no channel specified.
 *
 * With -S option, writes time samples to NTP shared memory (SHM unit 2)
 * for consumption by chrony/ntpd as a refclock. This enables the kernel
 * frequency discipline loop, which is required by applications like
 * rtlsdr-ft8d that read ntp_adjtime() for crystal PPM correction.
 */

#include <iostream>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <csignal>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>
#include <sys/timex.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "radio-receiver.h"
#include "rtl_sdr.h"
#include "rtl_tcp.h"
#include "channels.h"

/*
 * NTP Shared Memory protocol (RFC 2783 / gpsd SHM interface).
 * chrony refclock SHM and ntpd driver 28 both use this struct at
 * shmget key 0x4e545030 + unit.
 */
#define NTP_SHM_KEY 0x4e545030
struct shmTime {
    int mode;            /* 0 - if valid, set time immediately; 1 - use valid/count protocol */
    volatile int count;
    time_t clockTimeStampSec;
    int clockTimeStampUSec;
    time_t receiveTimeStampSec;
    int receiveTimeStampUSec;
    int leap;
    int precision;       /* log2(seconds) of source precision, e.g. -10 for ms */
    int nsamples;
    volatile int valid;
    unsigned clockTimeStampNSec;   /* added in mode 1 */
    unsigned receiveTimeStampNSec; /* added in mode 1 */
    int dummy[8];
};

static struct shmTime *shm_time = nullptr;

static struct shmTime *shm_init(int unit) {
    int shmid = shmget(NTP_SHM_KEY + unit, sizeof(struct shmTime), IPC_CREAT | 0600);
    if (shmid < 0) {
        perror("shmget");
        return nullptr;
    }
    auto *p = (struct shmTime *)shmat(shmid, nullptr, 0);
    if (p == (void *)-1) {
        perror("shmat");
        return nullptr;
    }
    memset(p, 0, sizeof(*p));
    p->mode = 1;
    p->precision = -10;  /* ~1 ms */
    p->nsamples = 3;
    return p;
}

static void shm_feed(struct shmTime *shm, const struct timespec &ref_ts, const struct timespec &sys_ts) {
    if (!shm) return;
    shm->valid = 0;
    __sync_synchronize();
    shm->count++;
    shm->clockTimeStampSec = ref_ts.tv_sec;
    shm->clockTimeStampUSec = ref_ts.tv_nsec / 1000;
    shm->clockTimeStampNSec = ref_ts.tv_nsec;
    shm->receiveTimeStampSec = sys_ts.tv_sec;
    shm->receiveTimeStampUSec = sys_ts.tv_nsec / 1000;
    shm->receiveTimeStampNSec = sys_ts.tv_nsec;
    shm->leap = 0;
    __sync_synchronize();
    shm->count++;
    shm->valid = 1;
}

static std::atomic<bool> running{true};
static std::mutex time_mutex;
static std::condition_variable time_cv;
static bool time_received = false;
static dab_date_time_t received_time;
static struct timespec reception_time;  /* CLOCK_MONOTONIC at frame arrival */

class TimeReceiver : public RadioControllerInterface {
public:
    void onSNR(float) override {}
    void onFrequencyCorrectorChange(int, int) override {}
    void onSyncChange(char) override {}
    void onSignalPresence(bool) override {}
    void onServiceDetected(uint32_t) override {}
    void onNewEnsemble(uint16_t) override {}
    void onSetEnsembleLabel(DabLabel&) override {}
    void onDateTimeUpdate(const dab_date_time_t& dt) override {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        std::lock_guard<std::mutex> lock(time_mutex);
        received_time = dt;
        reception_time = now;
        time_received = true;
        time_cv.notify_one();
    }
    void onFIBDecodeSuccess(bool, const uint8_t*) override {}
    void onNewImpulseResponse(std::vector<float>&&) override {}
    void onConstellationPoints(std::vector<DSPCOMPLEX>&&) override {}
    void onNewNullSymbol(std::vector<DSPCOMPLEX>&&) override {}
    void onTIIMeasurement(tii_measurement_t&&) override {}
    void onMessage(message_level_t level, const std::string& text, const std::string& text2) override {
        if (level == message_level_t::Error)
            std::cerr << "Error: " << text << text2 << std::endl;
    }
};

static void sighandler(int) {
    running = false;
    time_cv.notify_all();
    _exit(0);  /* Force exit - welle.io threads don't stop cleanly */
}

static const char* DAB_CHANNELS[] = {
    "5A","5B","5C","5D","6A","6B","6C","6D",
    "7A","7B","7C","7D","8A","8B","8C","8D",
    "9A","9B","9C","9D","10A","10B","10C","10D",
    "11A","11B","11C","11D","12A","12B","12C","12D",
    NULL
};

static bool try_channel(CVirtualInput& input, RadioReceiver& rx,
                        const std::string& channel, int timeout_sec)
{
    Channels ch;
    int freq = ch.getFrequency(channel);
    if (freq == 0) return false;

    time_received = false;
    input.setFrequency(freq);
    input.reset();
    rx.restart(false);

    std::unique_lock<std::mutex> lock(time_mutex);
    return time_cv.wait_for(lock, std::chrono::seconds(timeout_sec),
                            [] { return time_received || !running; })
           && time_received;
}

static void apply_time(const dab_date_time_t& t, const struct timespec& rx_mono, bool step_only)
{
    struct tm tm_dab = {};
    tm_dab.tm_year = t.year - 1900;
    tm_dab.tm_mon = t.month - 1;
    tm_dab.tm_mday = t.day;
    tm_dab.tm_hour = t.hour;
    tm_dab.tm_min = t.minutes;
    tm_dab.tm_sec = t.seconds;

    struct timespec ts_dab;
    ts_dab.tv_sec = timegm(&tm_dab);
    ts_dab.tv_nsec = (long)t.milliseconds * 1000000L;

    /* Get system time at the same instant as reception (using monotonic delta) */
    struct timespec mono_now, real_now;
    clock_gettime(CLOCK_MONOTONIC, &mono_now);
    clock_gettime(CLOCK_REALTIME, &real_now);

    /* How much time elapsed since frame was received */
    long elapsed_ns = (mono_now.tv_sec - rx_mono.tv_sec) * 1000000000L
                    + (mono_now.tv_nsec - rx_mono.tv_nsec);

    /* System realtime at the moment the frame arrived */
    struct timespec sys_at_rx;
    sys_at_rx.tv_sec = real_now.tv_sec;
    sys_at_rx.tv_nsec = real_now.tv_nsec - elapsed_ns;
    while (sys_at_rx.tv_nsec < 0) { sys_at_rx.tv_sec--; sys_at_rx.tv_nsec += 1000000000L; }

    /* Offset = DAB time - system time at reception */
    long offset_us = (ts_dab.tv_sec - sys_at_rx.tv_sec) * 1000000L
                   + (ts_dab.tv_nsec - sys_at_rx.tv_nsec) / 1000L;

    /* Feed SHM refclock if enabled */
    shm_feed(shm_time, ts_dab, sys_at_rx);

    /* In SHM mode, don't touch the clock — chrony handles discipline */
    if (shm_time) {
        fprintf(stderr, "DAB time: %04d-%02d-%02d %02d:%02d:%02d.%03d UTC (offset: %+ld µs → SHM)\n",
            t.year, t.month, t.day, t.hour, t.minutes, t.seconds, t.milliseconds, offset_us);
        return;
    }

    fprintf(stderr, "DAB time: %04d-%02d-%02d %02d:%02d:%02d.%03d UTC\n",
        t.year, t.month, t.day, t.hour, t.minutes, t.seconds, t.milliseconds);
    fprintf(stderr, "System offset: %+ld µs (processing delay: %ld µs)\n",
        offset_us, elapsed_ns / 1000);

    if (step_only || labs(offset_us) > 500000) {
        /* Step: set clock to DAB time + elapsed since reception */
        struct timespec ts_set;
        ts_set.tv_sec = ts_dab.tv_sec;
        ts_set.tv_nsec = ts_dab.tv_nsec + elapsed_ns;
        while (ts_set.tv_nsec >= 1000000000L) { ts_set.tv_sec++; ts_set.tv_nsec -= 1000000000L; }
        if (clock_settime(CLOCK_REALTIME, &ts_set) == 0)
            fprintf(stderr, "Clock stepped by %+ld µs\n", offset_us);
        else
            perror("clock_settime (need root/CAP_SYS_TIME?)");
    } else if (labs(offset_us) > 200) {
        /* Use adjtime-style single-shot slew for small offsets */
        struct timeval delta;
        delta.tv_sec = 0;
        delta.tv_usec = offset_us;
        if (adjtime(&delta, NULL) == 0)
            fprintf(stderr, "Clock slewed by %+ld µs\n", offset_us);
        else
            perror("adjtime");
    } else {
        fprintf(stderr, "Clock within 200µs, no adjustment\n");
    }
}

static void usage(const char* prog)
{
    fprintf(stderr,
        "dab_time_cli - DAB FIG 0/10 time extractor (NTP replacement)\n\n"
        "Usage: %s [options]\n"
        "  -c channel   DAB channel (e.g., 12C). Default: auto-scan\n"
        "  -d host:port Use rtl_tcp instead of local RTL-SDR\n"
        "  -g gain_dB   Manual gain (default: AGC)\n"
        "  -s           Always step clock (no slewing)\n"
        "  -S [unit]    Feed NTP shared memory for chrony/ntpd refclock (default unit: 2)\n"
        "  -1           One-shot: exit after first time update\n"
        "  -D device    RTL-SDR device index (default: 0)\n\n"
        "Examples:\n"
        "  %s -c 12C              # Local RTL-SDR, channel 12C\n"
        "  %s -c 12C -S           # Feed chrony SHM refclock (unit 2)\n"
        "  %s -1                  # Auto-scan, set clock, exit\n"
        "  %s -d 192.168.1.5:1234 -c 12C  # Remote via rtl_tcp\n\n"
        "Chrony refclock config for -S mode:\n"
        "  refclock SHM 2 refid DAB precision 1e-3 delay 0.01\n\n"
        "Requires root or CAP_SYS_TIME to set the system clock.\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char** argv)
{
    std::string channel;
    std::string tcp_host;
    uint16_t tcp_port = 1234;
    bool use_tcp = false;
    bool step_only = false;
    bool one_shot = false;
    bool use_shm = false;
    int shm_unit = 2;
    int device_idx = 0;
    float manual_gain = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i+1 < argc) channel = argv[++i];
        else if (!strcmp(argv[i], "-d") && i+1 < argc) {
            use_tcp = true;
            std::string arg = argv[++i];
            auto colon = arg.find(':');
            if (colon != std::string::npos) {
                tcp_host = arg.substr(0, colon);
                tcp_port = std::stoi(arg.substr(colon+1));
            } else tcp_host = arg;
        }
        else if (!strcmp(argv[i], "-g") && i+1 < argc) manual_gain = atof(argv[++i]);
        else if (!strcmp(argv[i], "-s")) step_only = true;
        else if (!strcmp(argv[i], "-S")) {
            use_shm = true;
            if (i+1 < argc && argv[i+1][0] != '-') shm_unit = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-1")) one_shot = true;
        else if (!strcmp(argv[i], "-D") && i+1 < argc) device_idx = atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }

    if (use_shm) {
        shm_time = shm_init(shm_unit);
        if (!shm_time) {
            fprintf(stderr, "Failed to initialize SHM unit %d\n", shm_unit);
            return 1;
        }
        fprintf(stderr, "SHM refclock enabled on unit %d (key 0x%08x)\n",
                shm_unit, NTP_SHM_KEY + shm_unit);
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    TimeReceiver controller;
    CVirtualInput* input_ptr;
    std::unique_ptr<CRTL_SDR> rtlsdr_input;
    std::unique_ptr<CRTL_TCP_Client> tcp_input;

    if (use_tcp) {
        tcp_input = std::make_unique<CRTL_TCP_Client>(controller);
        tcp_input->setServerAddress(tcp_host);
        tcp_input->setPort(tcp_port);
        input_ptr = tcp_input.get();
    } else {
        rtlsdr_input = std::make_unique<CRTL_SDR>(controller);
        input_ptr = rtlsdr_input.get();
    }

    input_ptr->setAgc(manual_gain < 0);
    if (manual_gain >= 0) {
        /* Find nearest gain step */
        int count = input_ptr->getGainCount();
        int best = 0;
        float best_diff = 999;
        for (int i = 0; i <= count; i++) {
            float g = input_ptr->setGain(i);
            if (fabsf(g - manual_gain) < best_diff) {
                best_diff = fabsf(g - manual_gain);
                best = i;
            }
        }
        input_ptr->setGain(best);
    }

    if (!input_ptr->restart()) {
        std::cerr << "Failed to open input device" << std::endl;
        return 1;
    }

    RadioReceiverOptions rro;
    RadioReceiver rx(controller, *input_ptr, rro);

    /* Main loop */
    std::string active_channel;

    /* Find a channel with time */
    if (!channel.empty()) {
        active_channel = channel;
        Channels ch;
        int freq = ch.getFrequency(active_channel);
        if (freq == 0) { fprintf(stderr, "Unknown channel: %s\n", active_channel.c_str()); return 1; }
        input_ptr->setFrequency(freq);
        input_ptr->reset();
        rx.restart(false);
        fprintf(stderr, "Tuned to %s, waiting for time...\n", active_channel.c_str());
    } else {
        fprintf(stderr, "Scanning DAB Band III...\n");
        for (int i = 0; DAB_CHANNELS[i] && running; i++) {
            fprintf(stderr, "  %s... ", DAB_CHANNELS[i]);
            if (try_channel(*input_ptr, rx, DAB_CHANNELS[i], 10)) {
                fprintf(stderr, "TIME FOUND!\n");
                active_channel = DAB_CHANNELS[i];
                apply_time(received_time, reception_time, step_only);
                if (one_shot) goto done;
                break;
            }
            fprintf(stderr, "no time\n");
        }
        if (active_channel.empty()) {
            fprintf(stderr, "No DAB time source found.\n");
            goto done;
        }
    }

    /* Continuous discipline loop with averaging */
    {
        fprintf(stderr, "Locked to %s, disciplining clock...\n", active_channel.c_str());

        #define AVG_WINDOW 10
        long offset_history[AVG_WINDOW];
        int offset_count = 0;
        int offset_idx = 0;
        int first_step_done = 0;

        while (running) {
            {
                std::unique_lock<std::mutex> lock(time_mutex);
                time_received = false;
                time_cv.wait(lock, [] { return time_received || !running; });
            }
            if (!time_received || !running) continue;

            /* Compute offset */
            struct tm tm_dab = {};
            tm_dab.tm_year = received_time.year - 1900;
            tm_dab.tm_mon = received_time.month - 1;
            tm_dab.tm_mday = received_time.day;
            tm_dab.tm_hour = received_time.hour;
            tm_dab.tm_min = received_time.minutes;
            tm_dab.tm_sec = received_time.seconds;

            struct timespec ts_dab;
            ts_dab.tv_sec = timegm(&tm_dab);
            ts_dab.tv_nsec = (long)received_time.milliseconds * 1000000L;

            struct timespec mono_now, real_now;
            clock_gettime(CLOCK_MONOTONIC, &mono_now);
            clock_gettime(CLOCK_REALTIME, &real_now);

            long elapsed_ns = (mono_now.tv_sec - reception_time.tv_sec) * 1000000000L
                            + (mono_now.tv_nsec - reception_time.tv_nsec);

            struct timespec sys_at_rx;
            sys_at_rx.tv_sec = real_now.tv_sec;
            sys_at_rx.tv_nsec = real_now.tv_nsec - elapsed_ns;
            while (sys_at_rx.tv_nsec < 0) { sys_at_rx.tv_sec--; sys_at_rx.tv_nsec += 1000000000L; }

            long offset_us = (ts_dab.tv_sec - sys_at_rx.tv_sec) * 1000000L
                           + (ts_dab.tv_nsec - sys_at_rx.tv_nsec) / 1000L;

            /* Feed SHM refclock on every sample */
            shm_feed(shm_time, ts_dab, sys_at_rx);

            /* In SHM mode, chrony handles all clock discipline */
            if (shm_time) {
                fprintf(stderr, "DAB time: %02d:%02d:%02d.%03d | offset: %+ld µs → SHM\n",
                    received_time.hour, received_time.minutes, received_time.seconds,
                    received_time.milliseconds, offset_us);
                continue;
            }

            /* First large offset: step immediately */
            if (!first_step_done && labs(offset_us) > 500000) {
                struct timespec ts_set;
                ts_set.tv_sec = ts_dab.tv_sec;
                ts_set.tv_nsec = ts_dab.tv_nsec + elapsed_ns;
                while (ts_set.tv_nsec >= 1000000000L) { ts_set.tv_sec++; ts_set.tv_nsec -= 1000000000L; }
                clock_settime(CLOCK_REALTIME, &ts_set);
                fprintf(stderr, "DAB time: %04d-%02d-%02d %02d:%02d:%02d.%03d UTC\n",
                    received_time.year, received_time.month, received_time.day,
                    received_time.hour, received_time.minutes, received_time.seconds,
                    received_time.milliseconds);
                fprintf(stderr, "Clock stepped by %+ld µs\n", offset_us);
                first_step_done = 1;
                offset_count = 0;
                offset_idx = 0;
                if (one_shot) break;
                continue;
            }
            first_step_done = 1;

            /* Accumulate offset measurements */
            offset_history[offset_idx] = offset_us;
            offset_idx = (offset_idx + 1) % AVG_WINDOW;
            if (offset_count < AVG_WINDOW) offset_count++;

            /* Apply correction only when window is full, then reset */
            if (offset_count >= AVG_WINDOW) {
                /* Compute median */
                long sorted[AVG_WINDOW];
                memcpy(sorted, offset_history, sizeof(sorted));
                int a, b;
                for (a = 0; a < AVG_WINDOW - 1; a++)
                    for (b = a + 1; b < AVG_WINDOW; b++)
                        if (sorted[a] > sorted[b]) { long t = sorted[a]; sorted[a] = sorted[b]; sorted[b] = t; }
                long median_offset = sorted[AVG_WINDOW / 2];

                fprintf(stderr, "DAB time: %02d:%02d:%02d.%03d | median offset: %+ld µs",
                    received_time.hour, received_time.minutes, received_time.seconds,
                    received_time.milliseconds, median_offset);

                if (labs(median_offset) > 100) {
                    struct timeval delta;
                    delta.tv_sec = 0;
                    delta.tv_usec = median_offset;
                    adjtime(&delta, NULL);
                    fprintf(stderr, " → slew\n");
                } else {
                    fprintf(stderr, " → ok\n");
                }

                /* Reset window — wait for slew to complete before next measurement */
                offset_count = 0;
                offset_idx = 0;
            }
        }
    }

done:
    rx.stop();
    return time_received ? 0 : 1;
}
