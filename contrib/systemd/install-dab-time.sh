#!/bin/bash
# Install dab-time systemd service
# Run as root: sudo ./install-service.sh

set -e

BINARY="/usr/local/bin/dab_time_cli"
SERVICE_DIR="/etc/systemd/system"

if [ ! -f "$BINARY" ]; then
    echo "Error: $BINARY not found. Run 'make install' first."
    exit 1
fi

# Disable NTP/timesyncd
echo "Disabling systemd-timesyncd..."
timedatectl set-ntp false 2>/dev/null || true
systemctl disable --now systemd-timesyncd 2>/dev/null || true

# Install service files
echo "Installing dab-time service..."
cp "$(dirname "$0")/dab-time.service" "$SERVICE_DIR/"
cp "$(dirname "$0")/dab-time-once.service" "$SERVICE_DIR/"

# Reload and enable
systemctl daemon-reload
systemctl enable dab-time.service

echo ""
echo "Installed. Edit /etc/systemd/system/dab-time.service to set your DAB channel."
echo ""
echo "Commands:"
echo "  systemctl start dab-time      # Start continuous discipline"
echo "  systemctl status dab-time     # Check status"
echo "  journalctl -u dab-time -f     # Follow logs"
echo ""
echo "For one-shot at boot:"
echo "  systemctl enable dab-time-once.service"
