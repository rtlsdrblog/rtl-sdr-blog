#!/bin/sh
# Install script for rtl-dab-time systemd service
# Run as root: sudo ./install-service.sh

set -e

BINARY="/usr/local/bin/rtl_dab_time"
SERVICE_DIR="/etc/systemd/system"

# Check binary exists
if [ ! -x "$BINARY" ]; then
    echo "Error: $BINARY not found. Run 'sudo make install' first."
    exit 1
fi

# Create system user
if ! id dab-time >/dev/null 2>&1; then
    echo "Creating system user 'dab-time'..."
    useradd -r -s /usr/sbin/nologin -d /nonexistent -c "DAB Time Sync" dab-time
fi

# Add user to plugdev group for USB access
usermod -aG plugdev dab-time 2>/dev/null || true

# Install service files
echo "Installing systemd service files..."
cp "$(dirname "$0")/rtl-dab-time.service" "$SERVICE_DIR/"
cp "$(dirname "$0")/rtl-dab-time-once.service" "$SERVICE_DIR/"

# Disable conflicting services
echo "Disabling conflicting time sync services..."
systemctl disable --now systemd-timesyncd.service 2>/dev/null || true
systemctl disable --now chronyd.service 2>/dev/null || true
systemctl disable --now ntp.service 2>/dev/null || true
systemctl disable --now ntpd.service 2>/dev/null || true
timedatectl set-ntp false 2>/dev/null || true

# Reload and enable
systemctl daemon-reload

echo ""
echo "Installation complete. Choose a mode:"
echo ""
echo "  Continuous discipline (like ntpd):"
echo "    sudo systemctl enable --now rtl-dab-time.service"
echo ""
echo "  One-shot at boot (like ntpdate):"
echo "    sudo systemctl enable --now rtl-dab-time-once.service"
echo ""
echo "  Both (set at boot + continuous discipline):"
echo "    sudo systemctl enable --now rtl-dab-time-once.service"
echo "    sudo systemctl enable --now rtl-dab-time.service"
echo ""
echo "Check status with:"
echo "    systemctl status rtl-dab-time.service"
echo "    journalctl -u rtl-dab-time.service -f"
echo ""
