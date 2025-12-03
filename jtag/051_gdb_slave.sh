#!/bin/bash

# Connect GDB to running ESP32 slave via OpenOCD
# Run this AFTER starting bin/050_openocd_slave.sh
#
# Usage: bin/051_gdb_slave.sh [1|2]
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

# Select GDB port based on slave number
if [[ "$SLAVE_NUM" == "1" ]]; then
    GDB_PORT=$GDB_PORT_SLAVE_1
else
    GDB_PORT=$GDB_PORT_SLAVE_2
fi

source $ESP_IDF_EXPORT

ELF_FILE="$BUILDDIR_SLAVE/ftm.elf"

if [ ! -f "$ELF_FILE" ]; then
    echo "ERROR: ELF file not found: $ELF_FILE"
    echo "Build the project first with: bin/043_idf build"
    exit 1
fi

echo "Connecting GDB to SLAVE $SLAVE_NUM device on port $GDB_PORT..."
echo ""
echo "Useful GDB commands:"
echo "  info threads          - Show all FreeRTOS tasks"
echo "  bt                    - Show backtrace of current thread"
echo "  thread N              - Switch to thread N"
echo "  bt full               - Show backtrace with local variables"
echo "  info registers        - Show CPU registers"
echo "  x/32x 0x3ffb0000      - Examine memory (example address)"
echo "  monitor reset halt    - Reset and halt the CPU"
echo "  continue              - Resume execution"
echo ""

# Use build-specific symbols but skip auto-connect (which resets)
# We'll manually connect to preserve device state
if [ -f "$BUILDDIR_SLAVE/gdbinit/symbols" ]; then
    GDBINIT_ARG="-x $BUILDDIR_SLAVE/gdbinit/symbols"
else
    echo "Warning: symbols file not found in build directory"
    GDBINIT_ARG=""
fi

xtensa-esp32s3-elf-gdb $GDBINIT_ARG \
    -ex "set remote hardware-watchpoint-limit 2" \
    -ex "set remote hardware-breakpoint-limit 2" \
    -ex "set remotetimeout 20" \
    -ex "target extended-remote :$GDB_PORT" \
    "$ELF_FILE"
