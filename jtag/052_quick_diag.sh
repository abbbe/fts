#!/bin/bash

# Quick diagnostic script to get backtrace from all tasks
# Usage: bin/052_quick_diag.sh [master|slave]

. bin/config

DEVICE=${1:-master}

if [ "$DEVICE" = "master" ]; then
    ELF_FILE="$BUILDDIR_MASTER/ftm.elf"
    BUILDDIR="$BUILDDIR_MASTER"
    GDB_PORT=$GDB_PORT_MASTER
    echo "Diagnosing MASTER device on port $GDB_PORT..."
elif [ "$DEVICE" = "slave" ]; then
    ELF_FILE="$BUILDDIR_SLAVE/ftm.elf"
    BUILDDIR="$BUILDDIR_SLAVE"
    GDB_PORT=$GDB_PORT_SLAVE
    echo "Diagnosing SLAVE device on port $GDB_PORT..."
else
    echo "Usage: $0 [master|slave]"
    exit 1
fi

if [ ! -f "$ELF_FILE" ]; then
    echo "ERROR: ELF file not found: $ELF_FILE"
    exit 1
fi

source $ESP_IDF_EXPORT

# Find build-specific gdbinit
if [ -f "$BUILDDIR/gdbinit/gdbinit" ]; then
    GDBINIT_ARG="-x $BUILDDIR/gdbinit/gdbinit"
elif [ -f "$BUILDDIR/gdbinit" ]; then
    GDBINIT_ARG="-x $BUILDDIR/gdbinit"
else
    echo "Warning: gdbinit not found in build directory"
    GDBINIT_ARG=""
fi

# Create GDB command script
TMPFILE=$(mktemp)
cat > $TMPFILE <<'EOF'
set pagination off
set print pretty on

echo \n========================================\n
echo CPU STATE DIAGNOSTICS\n
echo ========================================\n

echo \n--- CPU Registers ---\n
info registers

echo \n--- Program Counter ---\n
x/i $pc

echo \n--- All FreeRTOS Tasks ---\n
info threads

echo \n--- Backtraces for All Tasks ---\n
thread apply all bt

echo \n--- Current Task Details ---\n
bt full

echo \n========================================\n
echo END OF DIAGNOSTICS\n
echo ========================================\n

quit
EOF

echo "Connecting to OpenOCD and collecting diagnostics..."
echo "(Make sure OpenOCD is running: bin/050_openocd_${DEVICE}.sh)"
echo ""

xtensa-esp32s3-elf-gdb -batch \
    $GDBINIT_ARG \
    -x $TMPFILE \
    -ex "target remote :$GDB_PORT" \
    "$ELF_FILE"

rm $TMPFILE
