/*
 * dab_time_cli.cpp - Minimal DAB time extractor using welle.io backend
 * Extracts FIG 0/10 UTC time with second accuracy and sets system clock.
 *
 * Build (from rtl-sdr-blog root):
 *   g++ -O2 -std=c++14 -o build/dab_time_cli src/dab_time_cli.cpp \
 *     $(find /tmp/welle.io/build-cli/CMakeFiles/welle-cli.dir/src/{backend,input,various} -name "*.o") \
 *     -I/tmp/welle.io/src -I/tmp/welle.io/src/backend -I/tmp/welle.io/src/various -I/tmp/welle.io/src/input \
 *     -lfftw3 -lpthread -lm -lmpg123 -lfaad -lmp3lame
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

#include "radio-receiver.h"
#include "rtl_tcp.h"
#include "channels.h"

static std::atomic<bool> running{true};
static std::mutex time_mutex;
static std::condition_variable time_cv;
static bool time_received = false;
static dab_date_time_t received_time;

class TimeReceiver : public RadioControllerInterface {
public:
    void onSNR(float) override {}
    void onFrequencyCorrectorChange(int, int) override {}
    void onSyncChange(char) override {}
    void onSignalPresence(bool s) override {
        if (s) std::cerr << "Signal found" << std::endl;
    }
    void onServiceDetected(uint32_t) override {}
    void onNewEnsemble(uint16_t) override {}
    void onSetEnsembleLabel(DabLabel& label) override {
        std::cerr << "Ensemble: " << label.utf8_label() << std::endl;
    }
    void onDateTimeUpdate(const dab_date_time_t& dt) override {
        std::lock_guard<std::mutex> lock(time_mutex);
        received_time = dt;
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
            std::cerr << text << text2 << std::endl;
    }
};

static void sighandler(int) {
    running = false;
    time_cv.notify_all();
}

int main(int argc, char** argv)
{
    std::string channel = "12C";
    std::string host = "127.0.0.1";
    uint16_t port = 1234;
    bool step_only = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i+1 < argc) channel = argv[++i];
        else if (!strcmp(argv[i], "-d") && i+1 < argc) {
            std::string arg = argv[++i];
            auto colon = arg.find(':');
            if (colon != std::string::npos) {
                host = arg.substr(0, colon);
                port = std::stoi(arg.substr(colon+1));
            } else host = arg;
        }
        else if (!strcmp(argv[i], "-s")) step_only = true;
        else { std::cerr << "Usage: " << argv[0] << " [-c channel] [-d host:port] [-s]" << std::endl; return 1; }
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    TimeReceiver controller;
    CRTL_TCP_Client input(controller);
    input.setServerAddress(host);
    input.setPort(port);

    Channels ch;
    int freq = ch.getFrequency(channel);
    if (freq == 0) { std::cerr << "Unknown channel: " << channel << std::endl; return 1; }

    RadioReceiverOptions rro;
    RadioReceiver rx(controller, input, rro);

    input.setFrequency(freq);
    input.setAgc(true);
    if (!input.restart()) {
        std::cerr << "Failed to connect to " << host << ":" << port << std::endl;
        return 1;
    }
    rx.restart(false);

    std::cerr << "Receiving " << channel << " (" << freq/1000 << " kHz), waiting for time..." << std::endl;

    /* Wait for time (up to 60s) */
    {
        std::unique_lock<std::mutex> lock(time_mutex);
        time_cv.wait_for(lock, std::chrono::seconds(60), [] { return time_received || !running; });
    }

    rx.stop();
    // input stopped by destructor

    if (!time_received) {
        std::cerr << "No time received" << std::endl;
        return 1;
    }

    /* Apply time */
    struct tm tm_dab = {};
    tm_dab.tm_year = received_time.year - 1900;
    tm_dab.tm_mon = received_time.month - 1;
    tm_dab.tm_mday = received_time.day;
    tm_dab.tm_hour = received_time.hour;
    tm_dab.tm_min = received_time.minutes;
    tm_dab.tm_sec = received_time.seconds;

    struct timespec ts_dab, ts_now;
    ts_dab.tv_sec = timegm(&tm_dab);
    ts_dab.tv_nsec = 0;

    clock_gettime(CLOCK_REALTIME, &ts_now);
    long offset_us = (ts_dab.tv_sec - ts_now.tv_sec) * 1000000L
                   + (ts_dab.tv_nsec - ts_now.tv_nsec) / 1000L;

    fprintf(stderr, "DAB time: %04d-%02d-%02d %02d:%02d:%02d UTC (offset: %+ld µs)\n",
        received_time.year, received_time.month, received_time.day,
        received_time.hour, received_time.minutes, received_time.seconds, offset_us);

    if (step_only || labs(offset_us) > 500000) {
        if (clock_settime(CLOCK_REALTIME, &ts_dab) == 0)
            fprintf(stderr, "Clock stepped\n");
        else
            perror("clock_settime (need root?)");
    } else {
        fprintf(stderr, "Offset < 0.5s, no step needed\n");
    }

    return 0;
}
