#!/bin/bash

# Setup USB permissions for ESP32-S3 USB JTAG
# This fixes the LIBUSB_ERROR_ACCESS error

echo "Setting up USB permissions for ESP32-S3 JTAG debugging..."
echo ""

# Create udev rules file
RULES_FILE="/etc/udev/rules.d/60-esp32s3-jtag.rules"

echo "Creating udev rules..."
sudo tee $RULES_FILE > /dev/null <<'EOF'
# ESP32-S3 USB JTAG/serial debug unit
ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", MODE="0666", GROUP="plugdev"
EOF

echo "Reloading udev rules..."
sudo udevadm control --reload-rules
sudo udevadm trigger

echo ""
echo "Done! USB permissions configured."
echo ""
echo "You may need to:"
echo "1. Unplug and replug your ESP32-S3 devices"
echo "2. Or run: sudo udevadm trigger"
echo ""
echo "Then try running bin/050_openocd_master.sh again"
