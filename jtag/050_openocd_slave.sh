#!/bin/bash

# Start OpenOCD for ESP32-S3 slave device
# Attaches to running device without resetting it
# Reset commands (monitor reset halt/init) are available in GDB if needed
#
# Usage: bin/050_openocd_slave.sh [1|2]
#   Default: slave 1

. bin/config

# Parse slave number (default to 1)
SLAVE_NUM=${1:-1}

if [[ "$SLAVE_NUM" != "1" && "$SLAVE_NUM" != "2" ]]; then
    echo "Error: Slave number must be 1 or 2"
    echo "Usage: $0 [1|2]"
    echo "  Default: 1"
    exit 1
fi

# Select serial and ports based on slave number
if [[ "$SLAVE_NUM" == "1" ]]; then
    SERIAL=$SERIAL_SLAVE_1
    GDB_PORT=$GDB_PORT_SLAVE_1
    TCL_PORT=$TCL_PORT_SLAVE_1
    TELNET_PORT=$TELNET_PORT_SLAVE_1
else
    SERIAL=$SERIAL_SLAVE_2
    GDB_PORT=$GDB_PORT_SLAVE_2
    TCL_PORT=$TCL_PORT_SLAVE_2
    TELNET_PORT=$TELNET_PORT_SLAVE_2
fi

echo "Starting OpenOCD for SLAVE $SLAVE_NUM device"
echo "In another terminal, run: bin/051_gdb_slave.sh $SLAVE_NUM"
echo ""

source $ESP_IDF_EXPORT

# Start OpenOCD for ESP32-S3 (XIAO ESP32-S3 Sense)
# ESP32-S3 has built-in USB JTAG, no external JTAG adapter needed!
# Just connect the USB-C cable that you normally use for programming
echo "Connecting to SLAVE $SLAVE_NUM device with serial: $SERIAL"
echo "Ports: GDB=$GDB_PORT, TCL=$TCL_PORT, Telnet=$TELNET_PORT"
echo "OpenOCD will attach to running device without resetting it"

openocd -s jtag \
    -f esp32s3-base.cfg \
    -c "adapter serial $SERIAL" \
    -c "gdb port $GDB_PORT" \
    -c "tcl port $TCL_PORT" \
    -c "telnet port $TELNET_PORT" \
    -c "init"
