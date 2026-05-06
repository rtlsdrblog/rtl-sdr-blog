# dab_time_cli — DAB Time Synchronization (NTP Replacement)

## Overview

`dab_time_cli` extracts UTC time with millisecond accuracy from DAB (Digital
Audio Broadcasting) FIG 0/10 and continuously disciplines the Linux system clock.
It serves as a drop-in NTP replacement for systems with DAB reception but no
network connectivity.

Uses [welle.io](https://github.com/AlbrechtL/welle.io) as the DAB decoding
backend for robust OFDM synchronization and FIC decoding.

## Performance

- **Accuracy**: ±200 µs typical (after initial sync)
- **Resolution**: 1 ms (FIG 0/10 long form)
- **Sync time**: ~5-10 seconds from cold start
- **Update rate**: every 1 second

## Requirements

### Hardware
- RTL-SDR dongle (RTL2832U + R820T/R820T2)
- DAB antenna (Band III, 174-230 MHz)

### Software Dependencies

| Package | Debian/Ubuntu | Purpose |
|---------|--------------|---------|
| libfftw3-dev | `apt install libfftw3-dev` | FFT processing |
| libusb-1.0-0-dev | `apt install libusb-1.0-0-dev` | USB access |
| libmpg123-dev | `apt install libmpg123-dev` | Audio codec (welle.io dep) |
| libfaad-dev | `apt install libfaad-dev` | AAC decoder (welle.io dep) |
| libmp3lame-dev | `apt install libmp3lame-dev` | MP3 encoder (welle.io dep) |
| cmake | `apt install cmake` | Build system |
| g++ | `apt install g++` | C++14 compiler |

Install all at once:
```bash
sudo apt install cmake g++ libfftw3-dev libusb-1.0-0-dev libmpg123-dev libfaad-dev libmp3lame-dev
```

## Building

```bash
git clone --recursive https://github.com/Claudio-Sjo/rtl-sdr-blog.git
cd rtl-sdr-blog

# Apply milliseconds patch to welle.io
cd lib/welle.io
git apply ../../patches/welle-io-milliseconds.patch
cd ../..

# Build
mkdir build && cd build
cmake -DBUILD_DAB_TIME=ON ..
make dab_time_cli

# Install
sudo make install
# Or manually:
sudo cp dab_time_cli /usr/local/bin/
```

## Usage

```
dab_time_cli [options]

Options:
  -c channel    DAB channel (e.g., 5A, 10B, 12C). Default: auto-scan
  -d host:port  Use rtl_tcp instead of local RTL-SDR
  -g gain_dB    Manual gain (default: software AGC)
  -s            Always step clock (no slewing)
  -1            One-shot: set clock and exit
  -D index      RTL-SDR device index (default: 0)
```

### Examples

```bash
# Continuous discipline on channel 12C (local RTL-SDR)
sudo dab_time_cli -c 12C

# One-shot: set clock and exit
sudo dab_time_cli -c 12C -1

# Auto-scan all channels
sudo dab_time_cli -1

# Via remote rtl_tcp
dab_time_cli -c 12C -d 192.168.1.100:1234
```

### Output

```
Tuned to 12C, waiting for time...
Locked to 12C, disciplining clock...
DAB time: 2026-05-06 07:09:39.091 UTC
System offset: -1622839 µs
Clock stepped by -1622839 µs
DAB time: 2026-05-06 07:09:40.051 UTC
System offset: +236 µs
Clock within 1ms, no adjustment
DAB time: 2026-05-06 07:09:41.011 UTC
System offset: +181 µs
Clock within 1ms, no adjustment
```

## systemd Service

### Installation

```bash
sudo contrib/systemd/install-dab-time.sh
```

This will:
1. Disable systemd-timesyncd (NTP)
2. Install the dab-time service
3. Enable it for automatic start

### Manual Setup

```bash
# Edit channel in service file
sudo cp contrib/systemd/dab-time.service /etc/systemd/system/
sudo systemctl edit dab-time.service  # Change -c 12C to your channel

# Disable NTP
sudo timedatectl set-ntp false

# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable --now dab-time
```

### Service Commands

```bash
systemctl start dab-time       # Start
systemctl stop dab-time        # Stop
systemctl status dab-time      # Status
journalctl -u dab-time -f      # Follow logs
```

### One-Shot at Boot

For systems that only need time set once at boot:
```bash
sudo cp contrib/systemd/dab-time-once.service /etc/systemd/system/
sudo systemctl enable dab-time-once
```

## Finding Your DAB Channel

Use auto-scan to find which channel broadcasts time in your area:
```bash
sudo dab_time_cli -1
```

Or check your country's DAB channel allocations. Public broadcasters
(BBC, SR, NRK, DR, ARD/ZDF) typically broadcast FIG 0/10 time.

## DAB Band III Channels

| Block | MHz | Block | MHz | Block | MHz | Block | MHz |
|-------|---------|-------|---------|-------|---------|-------|---------|
| 5A | 174.928 | 7A | 188.928 | 9A | 202.928 | 11A | 216.928 |
| 5B | 176.640 | 7B | 190.640 | 9B | 204.640 | 11B | 218.640 |
| 5C | 178.352 | 7C | 192.352 | 9C | 206.352 | 11C | 220.352 |
| 5D | 180.064 | 7D | 194.064 | 9D | 208.064 | 11D | 222.064 |
| 6A | 181.936 | 8A | 195.936 | 10A | 209.936 | 12A | 223.936 |
| 6B | 183.648 | 8B | 197.648 | 10B | 211.648 | 12B | 225.648 |
| 6C | 185.360 | 8C | 199.360 | 10C | 213.360 | 12C | 227.360 |
| 6D | 187.072 | 8D | 201.072 | 10D | 215.072 | 12D | 229.072 |

## Troubleshooting

| Problem | Solution |
|---------|----------|
| "No devices found" | Check USB connection, run `lsusb` |
| "clock_settime: Operation not permitted" | Run with `sudo` or add `CAP_SYS_TIME` |
| No time after 60s | Ensemble may not broadcast FIG 0/10, try another channel |
| Large constant offset | Normal on first sync, will step then converge |
| "Lost coarse sync" | Weak signal — check antenna, try higher gain (`-g 40`) |

## How It Works

1. Opens RTL-SDR device (or connects to rtl_tcp)
2. Software AGC optimizes gain level
3. welle.io OFDM processor syncs to DAB frame structure
4. FIC decoder extracts FIG 0/10 (date/time with milliseconds)
5. Clock discipline:
   - Offset > 0.5s → `clock_settime()` step
   - Offset 1ms-0.5s → `adjtimex()` slew
   - Offset < 1ms → no action

## License

GPL v2+ (same as rtl-sdr and welle.io)
