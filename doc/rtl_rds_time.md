# rtl_rds_time — Internal Design Document

## Overview

`rtl_rds_time` is a single-purpose RDS decoder that extracts the clock-time (CT)
broadcast by FM radio stations via the Radio Data System (RDS). It tunes to an FM
broadcast station, demodulates the FM signal, extracts the 57 kHz RDS subcarrier,
demodulates the BPSK data, and decodes Group 4A to print UTC date/time and local
time offset.

## Standards

- EN 50067 / IEC 62106 — Radio Data System (RDS) specification
- ITU-R BS.643 — System for automatic tuning and other applications in FM radio

## Signal Chain

```
┌─────────────────────────────────────────────────────────────────────┐
│                        RTL2832U Hardware                            │
│  Tuner → ADC → USB (unsigned 8-bit I/Q at 228 kS/s)               │
└────────────────────────────────┬────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 1. DC Offset Avoidance                                             │
│    - Tune capture_rate/4 (57 kHz) above target frequency           │
│    - Apply 90°/sample rotation to shift spectrum down by fs/4      │
│    - Converts uint8 samples to int16 (centered at zero)            │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ Complex I/Q baseband @ 228 kHz
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 2. FM Demodulation (Polar Discriminator)                           │
│    - Computes instantaneous frequency: arg(z[n] × conj(z[n-1]))   │
│    - Output: real-valued composite baseband signal                 │
│    - Contains: mono audio, 19kHz pilot, 38kHz stereo, 57kHz RDS   │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ Real composite baseband @ 228 kHz
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 3. 57 kHz Bandpass Filter                                          │
│    - 2nd-order IIR (biquad), center = 57 kHz, BW ≈ 4 kHz          │
│    - Isolates the RDS subcarrier from audio and stereo content     │
│    - At fs=228kHz, 57kHz is exactly fs/4 → cos(ω₀)=0 (simplifies │
│      the filter: a1 coefficient is zero)                           │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ Narrowband signal around 57 kHz
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 4. Costas Loop (BPSK Carrier Recovery + Demodulation)              │
│    - Generates local 57 kHz oscillator (NCO)                       │
│    - Multiplies input by cos(φ) and -sin(φ) → I and Q arms        │
│    - Error signal: sign(I) × Q (standard BPSK Costas discriminator)│
│    - 2nd-order loop filter (damping=0.707, BW=30 Hz)               │
│    - Tracks carrier frequency drift and phase                      │
│    - I-arm output = demodulated BPSK baseband                      │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ Baseband BPSK signal (~1187.5 baud)
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 5. Clock Recovery (Bit Timing)                                     │
│    - NCO running at nominal 1187.5 Hz (bit rate)                   │
│    - Accumulates (integrates) samples over each bit period         │
│    - At each NCO rollover: makes hard decision (sign of integral)  │
│    - Outputs one bit per 192 input samples (228000/1187.5)         │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ Raw bit stream @ 1187.5 bps
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 6. Differential Decoding                                           │
│    - RDS uses differential BPSK: data = bit[n] XOR bit[n-1]        │
│    - Resolves 180° phase ambiguity inherent in BPSK                │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ Decoded data bits
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 7. Block Synchronization                                           │
│    - RDS frame = 4 blocks × 26 bits (16 data + 10 checkword)       │
│    - Shifts bits into 26-bit register                              │
│    - Computes CRC syndrome using generator polynomial              │
│      G(x) = x¹⁰ + x⁸ + x⁷ + x⁵ + x⁴ + x³ + 1                  │
│    - Compares syndrome against known offset words:                 │
│      Block A: 0x0FC, Block B: 0x198, Block C: 0x168,              │
│      Block C': 0x350, Block D: 0x1B4                               │
│    - Sync acquired when syndrome matches an offset word            │
│    - Sync lost after excessive consecutive CRC failures            │
└────────────────────────────────┬────────────────────────────────────┘
                                 │ 16-bit data words (blocks A,B,C,D)
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│ 8. Group 4A Decoder (Clock-Time)                                   │
│    - Identifies group type from Block B bits 11-15 (type=4, ver=A) │
│    - Extracts Modified Julian Day (MJD) from blocks B and C        │
│    - Extracts hours, minutes, UTC offset from block D              │
│    - Converts MJD to calendar date (year/month/day)                │
│    - Prints UTC time and computed local time                       │
└─────────────────────────────────────────────────────────────────────┘
```

## Detailed Stage Descriptions

### 1. RF Capture and DC Avoidance

The RTL2832U has a known DC spike at the center frequency caused by ADC offset
and LO leakage. To avoid this corrupting the signal:

- The dongle is tuned `capture_rate/4 = 57000 Hz` above the target FM station
- A digital frequency shift of -fs/4 is applied by multiplying each sample by
  `e^{-jπn/2}`, which cycles through `{1, -j, -1, j}` every 4 samples
- This is implemented as byte swaps and negations (no multiplies needed)

The 228 kHz sample rate was chosen because:
- It provides bandwidth up to 114 kHz (Nyquist), covering the 57 kHz RDS
  subcarrier with margin
- 57 kHz is exactly fs/4, which simplifies both the DC avoidance shift and
  the bandpass filter design
- It's well within the RTL2832U's supported range

### 2. FM Demodulation

Standard polar discriminator:

```
φ[n] = arg(z[n] × conj(z[n-1]))
     = atan2(Im(z[n]×z*[n-1]), Re(z[n]×z*[n-1]))
```

Where `z[n] = I[n] + jQ[n]`. The output is proportional to the instantaneous
frequency deviation, which for FM is the modulating signal (composite baseband).

The output is scaled to ±32768 representing ±π radians of phase change per sample.

### 3. Bandpass Filter

A 2nd-order IIR bandpass filter isolates the 57 kHz RDS subcarrier:

```
H(z) = b₀(1 - z⁻²) / (1 + a₁z⁻¹ + a₂z⁻²)
```

At fs=228 kHz, the normalized center frequency ω₀ = 2π×57000/228000 = π/2.
Since cos(π/2) = 0, the `a₁` coefficient vanishes, simplifying computation.

The bandwidth of ~4 kHz passes the RDS signal (±2.4 kHz around 57 kHz) while
rejecting the stereo subcarrier at 38 kHz and audio content below 15 kHz.

### 4. Costas Loop

The Costas loop simultaneously recovers the 57 kHz carrier and demodulates
the BPSK signal. It consists of:

**NCO (Numerically Controlled Oscillator):**
- Generates cos(φ) and -sin(φ) at approximately 57 kHz
- Phase φ is adjusted by the loop filter

**Mixer:**
- I-arm: input × cos(φ) → contains the data signal when locked
- Q-arm: input × (-sin(φ)) → contains the error signal when locked

**Phase Detector (BPSK-specific):**
- Error = sign(I) × Q
- For BPSK, this removes the data modulation from the error signal
- Produces a signal proportional to phase error

**Loop Filter (2nd order):**
```
freq += β × error        (frequency integrator)
phase += freq + α × error (phase integrator with proportional path)
```

Parameters:
- Loop bandwidth: 30 Hz (narrow enough to reject noise, wide enough to track
  oscillator drift)
- Damping factor: 0.707 (critically damped)
- α = 2 × ζ × ωn = 2 × 0.707 × (2π×30/228000)
- β = ωn² = (2π×30/228000)²

### 5. Clock Recovery

RDS transmits at exactly 1187.5 bits/second (= 57000/48, locked to the
subcarrier). At 228 kHz sample rate, each bit spans 192 samples.

The clock recovery uses integrate-and-dump:
- Accumulates the demodulated baseband samples
- An NCO at the nominal bit rate triggers sampling decisions
- At each NCO rollover, the sign of the accumulated value determines the bit

This is adequate because the RDS bit rate is precisely locked to the 57 kHz
carrier (which the Costas loop has already recovered), so timing drift is
minimal.

### 6. Differential Decoding

RDS uses differential encoding to resolve the 180° phase ambiguity inherent
in BPSK (the Costas loop can lock in-phase or anti-phase). With differential
encoding:

```
transmitted_phase[n] = transmitted_phase[n-1] XOR data[n]
```

The decoder recovers data by XORing consecutive received bits:
```
data[n] = received[n] XOR received[n-1]
```

This works regardless of whether the Costas loop locked at 0° or 180°.

### 7. Block Synchronization

An RDS group consists of 4 blocks of 26 bits each (104 bits total):

```
Block:  |----A----|----B----|----C----|----D----|
Bits:    16d + 10c  16d + 10c  16d + 10c  16d + 10c
         (d=data, c=check+offset)
```

The 10 check bits are a CRC computed over the 16 data bits, XORed with a
block-specific offset word. This allows the receiver to:

1. Detect transmission errors (CRC mismatch)
2. Identify which block (A/B/C/D) is being received (each has a unique offset)

**Sync acquisition:** The decoder continuously computes the syndrome of the
last 26 received bits. When it matches a known offset word, sync is declared
and the block counter is set accordingly.

**Sync maintenance:** After sync, the decoder expects blocks in sequence
(A→B→C→D→A→...). CRC failures increment a bad-block counter. If errors
exceed good blocks by a threshold, sync is lost and re-acquisition begins.

### 8. Group 4A (Clock-Time) Decoding

Group type is identified from Block B, bits 11-15 (type number) and bit 10
(version: 0=A, 1=B). Group 4A carries:

```
Block B [1:0]   → MJD bits 16:15
Block C [15:1]  → MJD bits 14:0
Block C [0]     → Hours bit 4
Block D [15:12] → Hours bits 3:0
Block D [11:6]  → Minutes (0-59)
Block D [5]     → UTC offset sign (0=+, 1=-)
Block D [4:0]   → UTC offset in half-hours (0-24)
```

**Modified Julian Day (MJD)** is converted to calendar date using the algorithm
from EN 50067 Annex G:

```
y' = int((MJD - 15078.2) / 365.25)
m' = int((MJD - 14956.1 - int(y' × 365.25)) / 30.6001)
day = MJD - 14956 - int(y' × 365.25) - int(m' × 30.6001)
if m' == 14 or m' == 15:
    year = y' + 1901
    month = m' - 13
else:
    year = y' + 1900
    month = m' - 1
```

## Threading Model

Unlike `rtl_fm` which uses 4 threads, `rtl_rds_time` is single-threaded.
All processing runs inside the libusb async callback (`rtlsdr_callback`).
This is feasible because:

- RDS is only 1187.5 bps — negligible CPU load
- 228 kHz sample rate produces ~14 callbacks/second (at 16384 bytes each)
- Each callback processes ~8000 I/Q samples through the entire chain
- Total CPU time per callback is well under 1 ms on any modern hardware

## Timing Characteristics

- **Group 4A transmission rate:** Typically once per minute (station-dependent)
- **Time to first sync:** 2-5 seconds (depends on signal quality)
- **Time to first CT decode:** Up to ~60 seconds after sync
- **Accuracy:** ±1 minute (RDS CT has minute resolution only)

## Parameters

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Sample rate | 228,000 Hz | fs/4 = 57 kHz (RDS carrier) |
| Frequency offset | +57,000 Hz | DC spike avoidance |
| BPF center | 57,000 Hz | RDS subcarrier frequency |
| BPF bandwidth | ~4,000 Hz | Passes ±2.4 kHz RDS signal |
| Costas loop BW | 30 Hz | Tracks drift, rejects noise |
| Costas damping | 0.707 | Critically damped response |
| Bit rate | 1187.5 bps | RDS standard (57000/48) |
| Samples/bit | 192 | 228000/1187.5 |

## Limitations

- No error correction (only error detection via CRC)
- No Group 4A interpolation — waits for next valid transmission
- Single-station operation (no scanning)
- Minute-level resolution only (RDS CT does not carry seconds)
- The 2nd-order bandpass filter has moderate selectivity; strong adjacent
  signals could leak through in extreme cases
- Clock recovery has no timing error feedback (relies on the subcarrier
  being frequency-locked to the bit clock per the RDS standard)
