# rtl_dab_time — DAB Time Extraction Tool

## Overview

`dab_time_cli` extracts UTC time with millisecond accuracy from DAB (Digital
Audio Broadcasting) FIG 0/10 and sets the Linux system clock. It serves as a
lightweight NTP replacement for systems with DAB reception but no network.

The tool uses [welle.io](https://github.com/AlbrechtL/welle.io) as its DAB
decoding backend, providing robust OFDM synchronization, frequency correction,
and full FIC decoding.

## Features

- **Millisecond accuracy**: extracts the 10-bit milliseconds field from FIG 0/10 long form
- **rtl_tcp support**: connects to a remote RTL-SDR via rtl_tcp protocol
- **Software AGC**: automatic gain control matching welle.io's algorithm
- **Clock discipline**: steps the system clock or reports offset
- **Minimal**: only decodes FIC for time — no audio processing needed

## Requirements

- RTL-SDR dongle (local or via rtl_tcp)
- DAB ensemble broadcasting FIG 0/10 (most public broadcasters do)
- Linux with `clock_settime` capability (root or CAP_SYS_TIME)

### Build Dependencies

- fftw3f (single-precision FFT)
- libmpg123-dev
- libfaad-dev
- libmp3lame-dev
- pthreads

## Building

```bash
# Clone with submodule
git clone --recursive https://github.com/Claudio-Sjo/rtl-sdr-blog.git
cd rtl-sdr-blog

# Or if already cloned:
git submodule update --init

# Apply milliseconds patch to welle.io
cd lib/welle.io
git apply ../../patches/welle-io-milliseconds.patch
cd ../..

# Build
mkdir build && cd build
cmake -DBUILD_DAB_TIME=ON ..
make dab_time_cli
```

## Usage

```
dab_time_cli [-c channel] [-d host:port] [-s]
```

### Options

| Option | Description |
|--------|-------------|
| `-c channel` | DAB channel (e.g., 5A, 10B, 12C). Required. |
| `-d host:port` | rtl_tcp server address (default: 127.0.0.1:1234) |
| `-s` | Step clock immediately (default: only step if offset > 0.5s) |

### Examples

```bash
# Connect to local rtl_tcp, channel 12C
dab_time_cli -c 12C

# Connect to remote rtl_tcp
dab_time_cli -c 12C -d 192.168.1.100:1234

# Step clock immediately
sudo dab_time_cli -c 12C -d 192.168.1.100:1234 -s
```

### Output

```
Ensemble: SR STOCKHOLM
DAB time: 2026-05-06 06:18:40.051 UTC (offset: -2797575 µs)
Clock stepped
```

## Shell Script Alternative

For systems without C++ build tools, `contrib/rtl_dab_time.sh` provides
the same functionality using `welle-cli` as an external process:

```bash
./contrib/rtl_dab_time.sh -c 12C -d 192.168.1.100:1234 -1
```

## How It Works

1. Connects to RTL-SDR via rtl_tcp protocol
2. welle.io backend performs:
   - OFDM synchronization (null detection, PRS correlation, fine frequency correction)
   - DQPSK demodulation with frequency deinterleaving
   - FIC decoding (convolutional decode, energy dispersal, CRC check)
3. FIG 0/10 parser extracts MJD date + UTC time + milliseconds
4. System clock is stepped or offset is reported

## DAB Channels (Band III)

| Block | Freq (MHz) | Block | Freq (MHz) | Block | Freq (MHz) | Block | Freq (MHz) |
|-------|-----------|-------|-----------|-------|-----------|-------|-----------|
| 5A | 174.928 | 7A | 188.928 | 9A | 202.928 | 11A | 216.928 |
| 5B | 176.640 | 7B | 190.640 | 9B | 204.640 | 11B | 218.640 |
| 5C | 178.352 | 7C | 192.352 | 9C | 206.352 | 11C | 220.352 |
| 5D | 180.064 | 7D | 194.064 | 9D | 208.064 | 11D | 222.064 |
| 6A | 181.936 | 8A | 195.936 | 10A | 209.936 | 12A | 223.936 |
| 6B | 183.648 | 8B | 197.648 | 10B | 211.648 | 12B | 225.648 |
| 6C | 185.360 | 8C | 199.360 | 10C | 213.360 | 12C | 227.360 |
| 6D | 187.072 | 8D | 201.072 | 10D | 215.072 | 12D | 229.072 |

## Notes

- Not all DAB ensembles broadcast FIG 0/10. Public broadcasters (BBC, SR, NRK, DR)
  typically do. Commercial multiplexes often don't.
- The tool requires ~2-5 seconds to sync and extract time on a good signal.
- Accuracy is limited by DAB frame timing (~24ms frame period) and network
  latency when using rtl_tcp remotely.
