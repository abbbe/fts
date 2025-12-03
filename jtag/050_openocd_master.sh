#!/bin/bash

# Start OpenOCD for ESP32-S3 master device
# Attaches to running device without resetting it
# Reset commands (monitor reset halt/init) are available in GDB if needed

. bin/config

echo "Starting OpenOCD for MASTER device on $PORT_MASTER"
echo "In another terminal, run: bin/051_gdb_master.sh"
echo ""

source $ESP_IDF_EXPORT

# Start OpenOCD for ESP32-S3 (XIAO ESP32-S3 Sense)
# ESP32-S3 has built-in USB JTAG, no external JTAG adapter needed!
# Just connect the USB-C cable that you normally use for programming
echo "Connecting to MASTER device with serial: $SERIAL_MASTER"
echo "Ports: GDB=$GDB_PORT_MASTER, TCL=$TCL_PORT_MASTER, Telnet=$TELNET_PORT_MASTER"
echo "OpenOCD will attach to running device without resetting it"

openocd -s jtag \
    -f esp32s3-base.cfg \
    -c "adapter serial $SERIAL_MASTER" \
    -c "gdb port $GDB_PORT_MASTER" \
    -c "tcl port $TCL_PORT_MASTER" \
    -c "telnet port $TELNET_PORT_MASTER" \
    -c "init"
