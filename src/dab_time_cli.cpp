/*
 * dab_time_cli.cpp - DAB time extraction for system clock discipline
 *
 * Extracts FIG 0/10 UTC time with millisecond accuracy from DAB broadcasts
 * and sets the Linux system clock. Lightweight NTP replacement.
 *
 * Supports: direct RTL-SDR device or rtl_tcp remote connection.
 * Auto-scans DAB Band III channels if no channel specified.
 */

#include <iostream>
#include <cstring>
#include <ctime>
#include <csignal>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>
#include <sys/timex.h>

#include "radio-receiver.h"
#include "rtl_sdr.h"
#include "rtl_tcp.h"
#include "channels.h"

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
    } else if (labs(offset_us) > 1000) {
        struct timex tx = {};
        tx.modes = ADJ_OFFSET | ADJ_STATUS;
        tx.offset = offset_us;
        tx.status = STA_PLL;
        if (adjtimex(&tx) >= 0)
            fprintf(stderr, "Clock slewed by %+ld µs\n", offset_us);
        else
            perror("adjtimex");
    } else {
        fprintf(stderr, "Clock within 1ms, no adjustment\n");
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
        "  -1           One-shot: exit after first time update\n"
        "  -D device    RTL-SDR device index (default: 0)\n\n"
        "Examples:\n"
        "  %s -c 12C              # Local RTL-SDR, channel 12C\n"
        "  %s -1                  # Auto-scan, set clock, exit\n"
        "  %s -d 192.168.1.5:1234 -c 12C  # Remote via rtl_tcp\n\n"
        "Requires root or CAP_SYS_TIME to set the system clock.\n",
        prog, prog, prog, prog);
}

int main(int argc, char** argv)
{
    std::string channel;
    std::string tcp_host;
    uint16_t tcp_port = 1234;
    bool use_tcp = false;
    bool step_only = false;
    bool one_shot = false;
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
        else if (!strcmp(argv[i], "-1")) one_shot = true;
        else if (!strcmp(argv[i], "-D") && i+1 < argc) device_idx = atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
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

    /* Continuous discipline loop */
    {
        fprintf(stderr, "Locked to %s, disciplining clock...\n", active_channel.c_str());

        while (running) {
            {
                std::unique_lock<std::mutex> lock(time_mutex);
                time_received = false;
                time_cv.wait(lock, [] { return time_received || !running; });
            }
            if (time_received && running) {
                apply_time(received_time, reception_time, step_only);
                if (one_shot) break;
            }
        }
    }

done:
    rx.stop();
    return time_received ? 0 : 1;
}
