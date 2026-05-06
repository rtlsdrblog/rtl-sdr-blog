# rtl-sdr

Turns your Realtek RTL2832 based DVB dongle into a SDR receiver.

For more information see: https://osmocom.org/projects/rtl-sdr/wiki

---

## Modified RTL-SDR Blog Version

25-August-2023: Brought up to date with latest Osmocom upstream, and added some new features like auto-direct sampling.

### Features

1. **VCO PLL current fix** — Improves stability at frequencies above ~1.5 GHz
2. **Direct sampling for rtl_tcp** — Enabled
3. **Force bias tee via EEPROM** — `rtl_eeprom -b 1` to force ON, `rtl_eeprom -b 0` to remove
4. **Offset tuning = bias tee toggle** — Use "offset tuning" button in SDR# to toggle bias tee
5. **R828D RTL-SDR Blog V4 support** — Added
6. **Auto direct sampling** — Automatically switches to direct sampling below 24 MHz (R820T/R860)
7. **DAB time synchronization** — `dab_time_cli` extracts UTC time from DAB broadcasts as NTP replacement *(new)*

### DAB Time Synchronization (NTP Replacement)

`dab_time_cli` uses DAB digital radio broadcasts to synchronize the system clock with ±200 µs accuracy — no network required. Uses [welle.io](https://github.com/AlbrechtL/welle.io) as the DAB decoding backend.

```bash
# Build
sudo apt install libfftw3-dev libusb-1.0-0-dev chrony
git submodule update --init
cd lib/welle.io && git apply ../../patches/welle-io-milliseconds.patch && cd ../..
mkdir build && cd build
cmake -DBUILD_DAB_TIME=ON ..
make dab_time_cli
sudo make install

# Use (standalone — sets clock directly)
sudo dab_time_cli -c 12C      # Continuous discipline
sudo dab_time_cli -c 12C -1   # One-shot: set clock and exit

# Use with chrony (enables kernel frequency discipline for FT8 TX)
sudo dab_time_cli -c 12C -S 2   # Feed chrony via NTP shared memory
```

See [doc/rtl_dab_time.md](doc/rtl_dab_time.md) for full documentation, systemd service setup, and channel list.

### Chrony Integration (for rtlsdr-ft8d / FT8 transmitter)

The `-S` option feeds time samples to chrony via NTP shared memory, enabling the
kernel frequency discipline loop (`ntp_adjtime()`). This is required by the
[rtlsdr-ft8d](https://github.com/Claudio-Sjo/rtlsdr-ft8d) transmitter for
crystal PPM correction.

```bash
# Automated install (disables NTP servers, configures chrony, starts services)
sudo ./contrib/systemd/install-dab-chrony.sh 12C

# Verify
chronyc sources      # Should show #* DAB
ntptime              # Should show frequency X.XXX ppm
```

Chrony config (`/etc/chrony/conf.d/dab-time.conf`):
```
refclock SHM 2 refid DAB precision 1e-3 delay 0.01 poll 1 prefer trust
makestep 0.5 3
rtcsync
```

**Important**: Disable or remove NTP pool/server lines from `/etc/chrony/chrony.conf`
when using DAB as the sole time source, otherwise chrony may reject DAB as a
falseticker.

See [doc/rtl_dab_time.md](doc/rtl_dab_time.md#chrony-refclock-mode--s) for
detailed step-by-step instructions and troubleshooting.

---

> **BIAS TEE NOTE**: Always take care that you do not enable the bias tee when the device is connected to a short circuited antenna unless there is an inline LNA. The circuit is protected with a self-resetting thermal fuse and built-in LDO protection.

> **Note**: Hack 3 (force bias tee) only works if your system is using this driver. Make sure you completely clean previous drivers first.

---

## Installation (Linux)

> **NOTE**: If you previously installed `librtlsdr-dev` via the package manager, remove it first:
> ```bash
> sudo apt purge ^librtlsdr
> sudo rm -rvf /usr/lib/librtlsdr* /usr/include/rtl-sdr* /usr/local/lib/librtlsdr* /usr/local/include/rtl-sdr* /usr/local/include/rtl_* /usr/local/bin/rtl_*
> ```

```bash
sudo apt update
sudo apt install libusb-1.0-0-dev git cmake pkg-config
git clone https://github.com/rtlsdrblog/rtl-sdr-blog
cd rtl-sdr-blog/
mkdir build
cd build
cmake ../ -DINSTALL_UDEV_RULES=ON
make
sudo make install
sudo cp ../rtl-sdr.rules /etc/udev/rules.d/
sudo ldconfig
echo 'blacklist dvb_usb_rtl28xxu' | sudo tee --append /etc/modprobe.d/blacklist-dvb_usb_rtl28xxu.conf
```

## Alternative Debian Package Installation

For systems reliant on Debian packages (FlightRadar24, FlightAware, ADSBExchange):

```bash
sudo apt update
sudo apt install libusb-1.0-0-dev git cmake debhelper
git clone https://github.com/rtlsdrblog/rtl-sdr-blog
cd rtl-sdr-blog
sudo dpkg-buildpackage -b --no-sign
cd ..
sudo dpkg -i librtlsdr0_*.deb
sudo dpkg -i librtlsdr-dev_*.deb
sudo dpkg -i rtl-sdr_*.deb
```

## Installation (macOS)

```bash
brew uninstall rtl-sdr
brew install cmake libusb pkgconfig
git clone https://github.com/rtlsdrblog/rtl-sdr-blog
cd rtl-sdr-blog
mkdir build && cd build
cmake ../
make LIBRARY_PATH=/usr/local/lib
sudo make install
```

## Installation (Windows)

Download the `Release.zip` file from the Releases page. For SDR# extract `rtlsdr.dll` from the `x86` folder. For most other x64 programs, use the `rtlsdr.dll` from the `x64` folder.
