#!/bin/bash
# Install DAB time with NTP fallback for chrony.
# DAB is preferred when RTL-SDR is connected; NTP is used when unavailable.
# Includes udev rule for automatic USB hotplug start/stop.
#
# Usage: sudo ./install-dab-chrony-fallback.sh [channel]
#   channel: DAB channel (default: 12C)

set -e

CHANNEL="${1:-12C}"

echo "=== Installing DAB time + NTP fallback (channel: $CHANNEL) ==="

# Check chrony is installed
if ! command -v chronyd &>/dev/null; then
    echo "Installing chrony..."
    apt-get install -y chrony
fi

# Disable systemd-timesyncd (conflicts with chrony)
if systemctl is-active --quiet systemd-timesyncd 2>/dev/null; then
    echo "Disabling systemd-timesyncd (replaced by chrony)..."
    systemctl stop systemd-timesyncd
    systemctl disable systemd-timesyncd
fi
timedatectl set-ntp false 2>/dev/null || true

# Remove any previous DAB-only config that disabled NTP servers
if [ -f /etc/chrony/conf.d/dab-time.conf ]; then
    rm /etc/chrony/conf.d/dab-time.conf
fi
# Restore NTP servers if previously commented out
sed -i 's/^#pool /pool /' /etc/chrony/chrony.conf
sed -i 's/^#server /server /' /etc/chrony/chrony.conf

# Install fallback chrony config (DAB preferred + NTP fallback)
echo "Installing chrony config (DAB preferred, NTP fallback)..."
mkdir -p /etc/chrony/conf.d
cp "$(dirname "$0")/../chrony-dab-fallback.conf" /etc/chrony/conf.d/dab-time.conf

# Install systemd service
echo "Installing dab-time-chrony service (channel: $CHANNEL)..."
sed "s/-c 12C/-c $CHANNEL/" \
    "$(dirname "$0")/dab-time-chrony.service" \
    > /etc/systemd/system/dab-time-chrony.service

# Install udev rule for USB hotplug
echo "Installing udev rule for RTL-SDR hotplug..."
cp "$(dirname "$0")/../99-rtlsdr-dab.rules" /etc/udev/rules.d/
udevadm control --reload-rules

# Stop conflicting services
systemctl stop dab-time 2>/dev/null || true
systemctl disable dab-time 2>/dev/null || true

# Enable (but don't start — udev or manual start will trigger it)
systemctl daemon-reload
systemctl enable dab-time-chrony

# If RTL-SDR is currently connected, start now
if lsusb | grep -q "0bda:283[28]"; then
    echo "RTL-SDR detected, starting dab-time-chrony..."
    systemctl restart dab-time-chrony
    sleep 5
fi

systemctl restart chronyd
sleep 3
chronyc makestep 2>/dev/null || true

echo ""
echo "=== Done ==="
echo "Mode: DAB preferred, NTP fallback"
echo ""
echo "  RTL-SDR plugged in  → DAB time (±100µs)"
echo "  RTL-SDR removed     → NTP fallback (±10ms)"
echo ""
echo "Verify:"
echo "  chronyc sources    # #* DAB when connected, ^* NTP when not"
echo "  ntptime            # frequency X.XXX ppm (always available)"
