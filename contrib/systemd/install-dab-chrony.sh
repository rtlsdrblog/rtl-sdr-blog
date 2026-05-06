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
fi
timedatectl set-ntp false 2>/dev/null || true

# Comment out NTP pool/server lines to prevent chrony from rejecting DAB
echo "Disabling NTP servers in chrony.conf (DAB will be sole source)..."
sed -i 's/^pool /#pool /' /etc/chrony/chrony.conf
sed -i 's/^server /#server /' /etc/chrony/chrony.conf

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

# Reload and enable
systemctl daemon-reload
systemctl enable dab-time-chrony

# Start dab_time_cli first, let it lock, then start chrony
echo "Starting dab-time-chrony..."
systemctl restart dab-time-chrony
echo "Waiting for DAB lock (5 seconds)..."
sleep 5

echo "Restarting chrony..."
systemctl restart chronyd

# Force step if clock is far off
sleep 3
chronyc makestep 2>/dev/null || true

echo ""
echo "=== Done ==="
echo "DAB time → SHM unit 2 → chrony → kernel frequency discipline"
echo ""
echo "Verify with:"
echo "  chronyc sources    # Should show #* DAB"
echo "  chronyc tracking   # Should show frequency correction"
echo "  ntptime            # Should show TIME_OK and freq offset"
echo ""
echo "If chronyc shows '#? DAB', wait 30 seconds for convergence."
echo "If offset is large, run: chronyc makestep"
