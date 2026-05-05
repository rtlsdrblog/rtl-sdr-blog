# rtl_dab_time — Internal Design Document

## Overview

`rtl_dab_time` is a minimal DAB (Digital Audio Broadcasting) receiver that
extracts only the time-of-day information from the FIC (Fast Information Channel)
and uses it to discipline the Linux system clock. It serves as a lightweight
replacement for NTP in environments with DAB reception but no network connectivity.

Key features:
- **Auto-scan**: automatically scans all 32 DAB Band III channels (174-230 MHz)
  to find a signal — no manual frequency configuration needed
- **Clock discipline**: uses `adjtimex()` for gradual slewing or `clock_settime()`
  for immediate step correction
- **Millisecond accuracy**: DAB FIG 0/10 long form provides UTC with ms resolution
- **systemd integration**: ships with service files for drop-in NTP replacement

## Standards

- ETSI EN 300 401 — DAB system specification (Eureka-147)
- §8.1.3.1 — FIG type 0 extension 10 (Date and time)

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│ 0. Channel Scanner (if no -f specified)                            │
│    Scans all 32 DAB Band III blocks (174.928 - 229.072 MHz)       │
│    Dwells 500ms per channel, detects null symbol = DAB present     │
│    Prints status for each channel, locks onto first signal found   │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ Selected frequency
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         RTL2832U @ 2.048 MS/s                       │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ uint8 I/Q → cfloat
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 1. Null Symbol Detection (Frame Sync)                              │
│    Power-dip search: null symbol is ~6dB below data symbols        │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ Frame boundary position
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 2. OFDM Demodulation                                               │
│    a) Strip cyclic prefix (504 samples)                            │
│    b) 2048-point FFT                                               │
│    c) Extract 1536 active carriers                                 │
│    d) DQPSK: phase diff with previous symbol → soft bits           │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ 3072 soft bits per FIC symbol (×3)
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 3. FIC Decoding                                                    │
│    a) Depuncturing (PI_16: insert erasures at punctured positions)  │
│    b) Viterbi decode (rate 1/4, K=7, soft-decision)                │
│    c) Energy dispersal (PRBS descrambling)                         │
│    d) CRC-16 verification per FIB                                  │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ 30 bytes per valid FIB
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 4. FIG Parsing                                                     │
│    Scan FIBs for FIG type 0, extension 10 (Date and Time)          │
│    Extract: MJD, hours, minutes, seconds, milliseconds             │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ struct dab_time
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 5. Clock Discipline                                                │
│    offset < 0.5s → adjtimex() slew (gradual correction)            │
│    offset ≥ 0.5s → clock_settime() step (immediate set)            │
└─────────────────────────────────────────────────────────────────────┘
```

## DAB Mode I Frame Structure

```
│← 96 ms total (196608 samples @ 2.048 MHz) ─────────────────────────│

┌──────┬─────┬─────┬─────┬─────┬─────────────────────────────────────┐
│ NULL │ PRS │FIC 1│FIC 2│FIC 3│  MSC symbols 4-75 (audio, ignored)  │
│2656  │2552 │2552 │2552 │2552 │  72 × 2552 samples                  │
└──────┴─────┴─────┴─────┴─────┴─────────────────────────────────────┘
   │      │     │     │     │
   │      │     └─────┴─────┴── FIC: carries FIG 0/10 (time)
   │      └── Phase Reference Symbol (differential reference)
   └── No signal (frame boundary marker)
```

We only process the first 5 symbols (null + PRS + 3 FIC). The remaining 72
MSC symbols (carrying audio/data services) are discarded.

## Component Details

### ofdm.c — OFDM Demodulator

**Null Symbol Detection:**
- Computes power in sliding windows across the input buffer
- The null symbol (2656 samples of near-silence) creates a distinctive power dip
- Detection threshold: 6 dB below average power
- This provides coarse frame synchronization

**FFT:**
- Radix-2 DIT (Decimation-In-Time) implementation
- 2048-point complex FFT
- No external library dependency (self-contained)
- Adequate performance for the 4 FFTs per frame we need

**DQPSK Demodulation:**
- DAB encodes data in the phase *difference* between the same carrier in
  consecutive OFDM symbols
- For each carrier k: `data = arg(Z_n[k] × conj(Z_{n-1}[k]))`
- The PRS (symbol 0) serves as the phase reference for FIC symbol 1
- Phase differences map to 2 bits via QPSK constellation
- Output: soft decisions (0-255 scale, 128 = uncertain)

**Carrier Mapping:**
- 1536 active carriers out of 2048 FFT bins
- Negative frequencies: FFT bins [2048-768 .. 2047] → carriers -768..-1
- Positive frequencies: FFT bins [1 .. 768] → carriers 1..768
- DC carrier (bin 0) is unused

### viterbi.c — Convolutional Decoder

**Code Parameters:**
- Rate: 1/4 (4 output bits per input bit)
- Constraint length: K = 7 (64 states)
- Generator polynomials (octal): 133, 171, 145, 133

**Implementation:**
- Soft-decision (8-bit metrics) for ~2 dB gain over hard-decision
- Full traceback (not register-exchange) for correctness
- Precomputed output tables for all state transitions
- Memory: ~O(states × symbols) for traceback storage

**Performance:**
- Processes ~672 symbols per FIB (after depuncturing)
- 3 FIBs per frame = ~2016 Viterbi symbols per 96ms frame
- Well within real-time on any modern CPU

### fic.c — FIC Channel Decoder

**Depuncturing:**
- DAB FIC uses puncturing pattern PI_16 (period 32, keeps 24 of 32 bits)
- Inserts "uncertain" (value 128) soft bits at punctured positions
- Expands the bitstream for the Viterbi decoder

**Energy Dispersal:**
- PRBS generator: x^9 + x^5 + 1, initialized to 0x1FF
- XORed with decoded data to remove the scrambling applied at the transmitter
- Applied after Viterbi decoding, before CRC check

**CRC-16:**
- Polynomial: x^16 + x^12 + x^5 + 1 (CCITT)
- Initial value: 0xFFFF, final XOR: 0xFFFF
- Computed over 30 data bytes, compared against 2 CRC bytes
- Only FIBs passing CRC are used for time extraction

**FIG 0/10 Parsing:**
- Scans each valid FIB for FIG headers
- FIG type 0 (3-bit type field = 0), extension 10 (5-bit ext field = 10)
- Extracts MJD (17 bits), hours (5), minutes (6), seconds (6), ms (10)
- Long form (with seconds/ms) indicated by a flag bit
- MJD converted to calendar date using standard algorithm

### rtl_dab_time.c — Main Program

**Threading Model:**
- Capture thread: libusb async callback fills a shared buffer
- Processing thread: consumes buffer, runs full decode chain
- Mutex + condition variable for synchronization
- Two threads needed because libusb callbacks must return quickly

**Clock Discipline Strategy:**

| Offset | Action | Rationale |
|--------|--------|-----------|
| < 500 ms | `adjtimex()` slew | Gradual correction, no time jumps |
| ≥ 500 ms | `clock_settime()` step | Too large to slew in reasonable time |

The `adjtimex()` call uses `ADJ_OFFSET | ADJ_STATUS` with `STA_PLL` to engage
the kernel's PLL-based clock discipline. The kernel will gradually adjust the
clock frequency to converge on the correct time.

In continuous mode, updates arrive every ~96 ms (every DAB frame that contains
FIG 0/10). This provides excellent discipline — comparable to a GPS PPS signal
for long-term stability.

## Timing Accuracy Analysis

**Sources of error:**

| Source | Magnitude | Notes |
|--------|-----------|-------|
| DAB transmitter clock | < 1 µs | GPS-locked at broadcast sites |
| Propagation delay | ~3 µs/km | Speed of light, typically < 100 µs |
| RTL2832U USB latency | ~1-5 ms | USB bulk transfer scheduling |
| Processing pipeline | < 1 ms | Deterministic DSP chain |
| FIG 0/10 resolution | 1 ms (long) or 1 s (short) | Standard limitation |

**Achievable accuracy:**
- With long-form FIG 0/10: ±5-10 ms (dominated by USB latency jitter)
- With short-form FIG 0/10: ±1 second (format limitation)
- After PLL convergence (minutes): ±1-5 ms steady-state

**Comparison with NTP:**
- NTP over LAN: ±0.1-1 ms
- NTP over WAN: ±1-50 ms
- rtl_dab_time: ±5-10 ms (no network required)

## Usage

```bash
# Auto-scan and set clock once (simplest usage)
sudo rtl_dab_time -1

# Auto-scan, continuous discipline (NTP replacement)
sudo rtl_dab_time

# Manual frequency, one-shot
sudo rtl_dab_time -f 202.928M -1

# Manual frequency, continuous
sudo rtl_dab_time -f 194.064M

# With manual gain and PPM correction
sudo rtl_dab_time -f 225.648M -g 30 -p 52

# Step-only mode (no slewing)
sudo rtl_dab_time -s
```

Auto-scan output example:
```
Scanning DAB Band III (32 channels)...
  Block 5A (174.928 MHz) ... no signal
  Block 5B (176.640 MHz) ... no signal
  Block 5C (178.352 MHz) ... DAB FOUND!

══════════════════════════════════════════
  DAB signal found: Block 5C (178.352 MHz)
══════════════════════════════════════════

Receiving DAB block 5C (178.352 MHz)
Waiting for FIG 0/10 time data...

DAB time: 2026-05-05 11:24:23.456 UTC
System offset: +1234 µs
Clock slewed by +1234 µs
```

## systemd Integration

Ready-to-use service files are provided in `contrib/systemd/`:

| File | Purpose |
|------|---------|
| `rtl-dab-time.service` | Continuous clock discipline (like ntpd) |
| `rtl-dab-time-once.service` | One-shot clock set at boot (like ntpdate) |
| `install-service.sh` | Automated installer script |

### Quick Install

```bash
sudo contrib/systemd/install-service.sh
sudo systemctl enable --now rtl-dab-time.service
```

The installer:
1. Creates a `dab-time` system user
2. Adds it to the `plugdev` group (USB access)
3. Installs both service files
4. Disables conflicting NTP services
5. Prints instructions for choosing a mode

## Limitations

- **Linux only** — requires `adjtimex()` / `clock_settime()`
- **No coarse frequency offset correction** — relies on the RTL-SDR's oscillator
  being within ±1 carrier spacing (1 kHz) of the true frequency. Use `-p` for
  PPM correction if needed.
- **No fine timing sync** — uses null symbol detection only, not cyclic prefix
  correlation. This limits timing accuracy to ~1 symbol period (1.246 ms).
- **Single multiplex** — monitors one DAB block at a time
- **No channel estimation/equalization** — assumes flat fading (adequate for
  strong signals, may fail in multipath)
- **Requires root** — or `CAP_SYS_TIME` capability to modify system clock

## Replacing NTP with rtl_dab_time

This section explains how to fully replace NTP (ntpd, chronyd, systemd-timesyncd)
with `rtl_dab_time` as the sole time source on a Linux system.

### Prerequisites

- An RTL-SDR dongle connected via USB
- DAB reception at your location (Band III, 174-230 MHz)
- The `rtl_dab_time` binary installed to `/usr/local/bin/` (via `sudo make install`)
- No need to know your local frequency — auto-scan will find it

### Quick Setup (automated)

The fastest path — uses the provided installer script:

```bash
cd rtl-sdr-blog/
sudo make install                          # install binaries
sudo contrib/systemd/install-service.sh    # install service + disable NTP
sudo systemctl enable --now rtl-dab-time.service
```

That's it. The service will auto-scan for a DAB signal and begin disciplining
the clock. Check status with:

```bash
sudo journalctl -u rtl-dab-time.service -f
```

### Manual Setup (step by step)

#### Step 1: Test DAB reception

```bash
# Auto-scan: finds the first available DAB channel
sudo rtl_dab_time -1

# Or test a specific frequency
sudo rtl_dab_time -f 202.928M -1
```

Expected output:
```
Scanning DAB Band III (32 channels)...
  Block 5A (174.928 MHz) ... no signal
  Block 5B (176.640 MHz) ... no signal
  Block 5C (178.352 MHz) ... DAB FOUND!

══════════════════════════════════════════
  DAB signal found: Block 5C (178.352 MHz)
══════════════════════════════════════════

Receiving DAB block 5C (178.352 MHz)
Waiting for FIG 0/10 time data...

DAB time: 2026-05-05 11:24:23.456 UTC
System offset: +1234 µs
Clock stepped by +1234 µs
```

If no signal is found on any channel, check your antenna and try with
manual gain (`-g 40`).

#### Step 2: Disable existing NTP services

```bash
sudo systemctl stop systemd-timesyncd chronyd ntp 2>/dev/null
sudo systemctl disable systemd-timesyncd chronyd ntp 2>/dev/null
timedatectl set-ntp false
```

#### Step 3: Create system user and install service

```bash
# Create unprivileged user
sudo useradd -r -s /usr/sbin/nologin -d /nonexistent dab-time
sudo usermod -aG plugdev dab-time

# Install service files (shipped in contrib/systemd/)
sudo cp contrib/systemd/rtl-dab-time.service /etc/systemd/system/
sudo cp contrib/systemd/rtl-dab-time-once.service /etc/systemd/system/
sudo systemctl daemon-reload
```

#### Step 4: Choose operating mode

**Option A — Continuous discipline (recommended, like ntpd):**

```bash
sudo systemctl enable --now rtl-dab-time.service
```

The service auto-scans on startup, locks onto a DAB channel, and continuously
disciplines the clock via `adjtimex()`. If the signal is lost, it restarts
and re-scans.

**Option B — One-shot at boot (like ntpdate):**

```bash
sudo systemctl enable --now rtl-dab-time-once.service
```

Sets the clock once during early boot, then exits. Useful for systems that
only need approximate time at startup.

**Option C — Both (belt and suspenders):**

```bash
sudo systemctl enable --now rtl-dab-time-once.service
sudo systemctl enable --now rtl-dab-time.service
```

Sets clock immediately at boot (step), then continuously disciplines (slew).

#### Step 5: Verify

```bash
# Watch live updates
sudo journalctl -u rtl-dab-time.service -f

# Check kernel clock status
adjtimex --print | grep -E "status|offset|freq"

# timedatectl will show "NTP service: n/a" (expected)
timedatectl
```

The kernel's `STA_PLL` flag will be set, and the offset should converge toward
zero over several minutes.

#### Step 6 (optional): Pin to a specific channel

If you know your best DAB channel, edit the service to skip the scan:

```bash
sudo systemctl edit rtl-dab-time.service
```

Add:
```ini
[Service]
ExecStart=
ExecStart=/usr/local/bin/rtl_dab_time -f 178.352M
```

This saves ~16 seconds of scan time on startup.

### Service File Details

The shipped `rtl-dab-time.service` includes full systemd hardening:

- `User=dab-time` — runs unprivileged
- `AmbientCapabilities=CAP_SYS_TIME` — only capability needed
- `ProtectSystem=strict` — read-only filesystem
- `PrivateTmp=yes` — isolated /tmp
- `MemoryDenyWriteExecute=yes` — W^X enforcement
- `DeviceAllow=/dev/bus/usb/* rw` — USB access only
- `Conflicts=systemd-timesyncd.service chronyd.service ntp.service` — prevents
  running alongside other time sync

### Comparison: rtl_dab_time vs NTP vs chrony

| Feature | ntpd/chrony | rtl_dab_time |
|---------|-------------|--------------|
| Network required | Yes | No |
| Configuration needed | Server list | None (auto-scan) |
| Accuracy | 0.1-50 ms | 5-10 ms |
| Time to first sync | 5-30 seconds | 2-20 seconds (scan + decode) |
| Update interval | 64-1024 seconds | 96 ms |
| Hardware required | None (network) | RTL-SDR dongle (~$10) |
| Works offline | No | Yes |
| Works underground | No (unless network) | If DAB signal penetrates |
| Leap second aware | Yes | Yes (LSI bit in FIG 0/10) |
| Authentication | NTS/symmetric key | Inherent (broadcast) |
| Spoofing resistance | Low (network) | Moderate (requires RF) |

### When to use rtl_dab_time instead of NTP

- **Air-gapped systems** — no network connectivity by design
- **Embedded/IoT** — devices with DAB reception but no reliable internet
- **Network-hostile environments** — where NTP is blocked or unreliable
- **Rapid initial sync** — DAB provides time within seconds of boot
- **Zero-configuration** — auto-scan means no setup beyond plugging in the dongle
- **Redundancy** — as a secondary time source alongside NTP

### Troubleshooting

**"No DAB signal found on any channel"**
- Check antenna connection (needs a VHF antenna, not the stock whip)
- Increase gain: edit service to add `-g 40`
- Check if the RTL-SDR is detected: `rtl_test -t`
- Verify the `dab-time` user has USB access: `groups dab-time` should show `plugdev`

**"adjtimex failed"**
- Service needs `CAP_SYS_TIME` — verify with `systemctl show rtl-dab-time -p AmbientCapabilities`
- Or test manually with `sudo`

**Clock jumps instead of slewing**
- Normal on first start if system clock is far off (>0.5s)
- Subsequent corrections will slew smoothly
- Use `-s` flag to always step (not recommended for production)

**Large offset that doesn't converge**
- Check PPM error of your RTL-SDR: `rtl_test -p`
- Apply correction: edit service to add `-p <ppm_value>`
- Large PPM error can cause OFDM sync failures

**Service keeps restarting**
- Check `journalctl -u rtl-dab-time.service` for the specific error
- Most common: USB device not accessible (permissions) or no DAB coverage
