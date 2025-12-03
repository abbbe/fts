#!/bin/bash

# Connect GDB to running ESP32 master via OpenOCD
# Run this AFTER starting bin/050_openocd_master.sh

. bin/config

source $ESP_IDF_EXPORT

ELF_FILE="$BUILDDIR_MASTER/ftm.elf"

if [ ! -f "$ELF_FILE" ]; then
    echo "ERROR: ELF file not found: $ELF_FILE"
    echo "Build the project first with: bin/040_build_ftms_master.sh"
    exit 1
fi

echo "Connecting GDB to MASTER device on port $GDB_PORT_MASTER..."
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
if [ -f "$BUILDDIR_MASTER/gdbinit/symbols" ]; then
    GDBINIT_ARG="-x $BUILDDIR_MASTER/gdbinit/symbols"
else
    echo "Warning: symbols file not found in build directory"
    GDBINIT_ARG=""
fi

xtensa-esp32s3-elf-gdb $GDBINIT_ARG \
    -ex "set remote hardware-watchpoint-limit 2" \
    -ex "set remote hardware-breakpoint-limit 2" \
    -ex "set remotetimeout 20" \
    -ex "target extended-remote :$GDB_PORT_MASTER" \
    "$ELF_FILE"
