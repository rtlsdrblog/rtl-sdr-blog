#!/bin/bash
# Install DAB time as a chrony refclock source.
# This enables kernel frequency discipline (ntp_adjtime) for applications
# like rtlsdr-ft8d that need PPM correction from the kernel.
#
# Usage: sudo ./install-dab-chrony.sh [channel]
#   channel: DAB channel (default: 12C)

set -e

CHANNEL="${1:-12C}"

echo "=== Installing DAB time chrony refclock (channel: $CHANNEL) ==="

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
    timedatectl set-ntp false
fi

# Install chrony config
echo "Installing chrony DAB refclock config..."
mkdir -p /etc/chrony/conf.d
cp "$(dirname "$0")/../chrony-dab.conf" /etc/chrony/conf.d/dab-time.conf

# Install systemd service with correct channel
echo "Installing dab-time-chrony service (channel: $CHANNEL)..."
sed "s/-c 12C/-c $CHANNEL/" \
    "$(dirname "$0")/dab-time-chrony.service" \
    > /etc/systemd/system/dab-time-chrony.service

# Stop conflicting dab-time service if running
systemctl stop dab-time 2>/dev/null || true
systemctl disable dab-time 2>/dev/null || true

# Enable and start
systemctl daemon-reload
systemctl enable dab-time-chrony
systemctl restart dab-time-chrony
systemctl restart chronyd

echo ""
echo "=== Done ==="
echo "DAB time → SHM unit 2 → chrony → kernel frequency discipline"
echo ""
echo "Verify with:"
echo "  chronyc sources    # Should show DAB refclock"
echo "  chronyc tracking   # Should show frequency correction"
echo "  ntptime            # Should show TIME_OK and freq offset"
