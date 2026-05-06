# dab_time_cli — DAB Time Synchronization (NTP Replacement)

## Overview

`dab_time_cli` extracts UTC time with sub-millisecond accuracy from DAB (Digital
Audio Broadcasting) FIG 0/10 and continuously disciplines the Linux system clock.
It serves as a drop-in NTP replacement for systems with DAB reception but no
network connectivity.

Uses [welle.io](https://github.com/AlbrechtL/welle.io) as the DAB decoding
backend for robust OFDM synchronization and FIC decoding. Audio processing is
excluded — only the OFDM/FIC pipeline is used.

## Performance

- **Accuracy**: ±100 µs typical (after convergence)
- **Resolution**: 1 ms (FIG 0/10 long form)
- **Convergence**: ~10 seconds from cold start
- **Update cycle**: median of 10 measurements every 10 seconds
- **Jitter rejection**: median filter eliminates DAB frame timing noise

## Requirements

### Hardware
- RTL-SDR dongle (RTL2832U + R820T/R820T2)
- DAB antenna (Band III, 174-230 MHz)
- Raspberry Pi 3/4/5 or equivalent (Pi Zero too slow for real-time OFDM)

### Software Dependencies

| Package | Debian/Ubuntu | Purpose |
|---------|--------------|---------|
| libfftw3-dev | `apt install libfftw3-dev` | FFT processing |
| libusb-1.0-0-dev | `apt install libusb-1.0-0-dev` | USB access |
| cmake | `apt install cmake` | Build system |
| g++ | `apt install g++` | C++14 compiler |

Install all at once:
```bash
sudo apt install cmake g++ libfftw3-dev libusb-1.0-0-dev
```

**Note**: Audio codec libraries (mpg123, faad, lame) are NOT required — audio
processing is excluded from the build.

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
DAB time: 2026-05-06 12:14:16.051 UTC
Clock stepped by -1622839 µs
DAB time: 12:14:26.035 | median offset: -33 µs → ok
DAB time: 12:14:36.019 | median offset: -89 µs → ok
DAB time: 12:14:46.003 | median offset: -154 µs → slew
DAB time: 12:14:56.083 | median offset: -44 µs → ok
```

## Clock Discipline Algorithm

1. **Initial sync**: Step clock to DAB time on first FIG 0/10 reception
2. **Measurement**: Timestamp frame arrival with `CLOCK_MONOTONIC`, compute offset
   between DAB time and system time at reception (eliminates processing delay)
3. **Filtering**: Collect 10 offset measurements, compute median (rejects outliers
   from DAB frame timing jitter)
4. **Correction**: If median offset > 100µs, apply `adjtime()` slew
5. **Reset**: Discard window and wait 10 seconds for slew to complete
6. **Repeat**: Steps 2-5 continuously

## systemd Service

### Installation

```bash
sudo cp contrib/systemd/dab-time.service /etc/systemd/system/
# Edit channel: sudo systemctl edit dab-time
sudo timedatectl set-ntp false
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

## Finding Your DAB Channel

Not all DAB ensembles broadcast FIG 0/10 (time). Public broadcasters typically do:
- **Sweden**: SR STOCKHOLM (12C)
- **UK**: BBC National DAB
- **Germany**: ARD ensembles
- **Norway**: NRK
- **Denmark**: DR

Use auto-scan to find a channel with time:
```bash
sudo dab_time_cli -1
```

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
| "SyncOnPhase failed" | Weak signal — check antenna, try `-g 40` |
| Offset oscillates | Normal during first 10s, should stabilize |

## Chrony Refclock Mode (-S)

The `-S` option writes time samples to NTP shared memory (SHM), allowing chrony
to use DAB as a reference clock. This enables the **kernel frequency discipline
loop**, which is critical for applications that call `ntp_adjtime()` to obtain
crystal PPM correction (e.g., rtlsdr-ft8d transmitter).

### Why This Matters

Without chrony, `dab_time_cli` only sets the clock time — it does NOT populate
the kernel's frequency correction (`ntx.freq`). Applications reading
`ntp_adjtime()` will see 0 PPM and transmit at the wrong frequency.

With chrony as intermediary:
```
DAB broadcast → dab_time_cli -S → SHM → chrony → kernel freq discipline
                                                  → ntp_adjtime() returns correct PPM
```

### Quick Setup

```bash
sudo ./contrib/systemd/install-dab-chrony.sh 12C
```

### Manual Setup

1. Run `dab_time_cli` with SHM output:
   ```bash
   sudo dab_time_cli -c 12C -S 2
   ```

2. Configure chrony (`/etc/chrony/conf.d/dab-time.conf`):
   ```
   refclock SHM 2 refid DAB precision 1e-3 delay 0.01 poll 1
   makestep 1.0 3
   rtcsync
   ```

3. Restart chrony:
   ```bash
   sudo systemctl restart chronyd
   ```

4. Verify:
   ```bash
   chronyc sources      # Should show #* DAB
   chronyc tracking     # Shows frequency correction
   ntptime              # Should show status TIME_OK with freq offset
   ```

### Verification for FT8 Transmitter

After chrony converges (typically 30-60 seconds):
```bash
$ ntptime
ntp_gettime() returns code 0 (OK)
  time ... , maximum error ... us, estimated error ... us
ntp_adjtime() returns code 0 (OK)
  modes 0x0000,
  offset 0.000 us, frequency 3.141 ppm, ...
```

The `frequency X.XXX ppm` value is what the FT8 transmitter reads via
`ntp_adjtime()` to correct its RF output frequency.

## License

GPL v2+ (same as rtl-sdr and welle.io)
