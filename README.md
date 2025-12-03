Fine Time Sync is a library to build synchronised, high-precision timing network using off-the-shelf ESP32 boards, using nothing but its built in Wi-Fi Fine Timing Measurement (FTM) system.

The **technical implementation details** are in https://github.com/abbbe/fts/blob/main/docs/fts-presa-20251203.pdf. Feel free to ask questions on Reddit (see link below) or open an issue here on Github.

A small **demo** can be seen on Reddit https://www.reddit.com/r/embedded/comments/1pbp0az/. You can reproduce it following the instructions below.

The code uses native ESP-IDF. I would like to make easy-to use library for Arduino IDE, but see https://github.com/abbbe/fts/issues/2 (any help is appreciated).

# Supported Hardware

- Developed on S3, uses MCPWM timer to drive digital output from hardware
- Should work without modifications on other chips with FTM and MCPWM (S2, C6)
- Should work on C2 and C3 using with GPTimer instead of MCPWM
- Will not work at all on chips without FTM (classic ESP32, ESP32 H2)

# Hardware Setup

- Master: Seeed XIAO ESP32-S3 Sense
  - has a built in yellow LED
  - solder pin headers for oscilloscope probe: GND and GPIO7
- Slaves: Waveshare ESP32-S3 Touch Screen 1.47 + external LED
  - solder pin headers for oscilloscope probe: GND and GPIO7
  - solder pin header for external led (with a resistor): 3V3, GPIO41

# Quick Start

## Install ESP-IDF Toolchain and its dependencies

See https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html#installation.
If you use non-standard installation directory, adjust ESP_IDF_EXPORT in the config, see below.

## Clone FTS repo

```bash
git clone https://github.com/abbbe/fts
cd fts
```

There is a useful `bin/idf` wrapper script.

If you want to monitor multiple devices at once -- install tmux.

You must edit `bin/config.local` to set serial ports for your ESP32 boards, see below.

## Set serial port paths

### MacOS

On my MacBook serial ports paths depend on which USB socket I plug the board into, but otherwise are stable:
```
# ESP-IDF setup
ESP_IDF_EXPORT=~/esp/esp-idf/export.sh

# --- map the dev boards to roles ---
SERIAL_PORT_MASTER=/dev/cu.usbmodem11101
SERIAL_PORT_SLAVE_1=/dev/cu.usbmodem11201
SERIAL_PORT_SLAVE_2=/dev/cu.usbmodem11301
```

The same approach might work for some Linux distros.

### Linux and WSL

Stable paths to serial ports can be formed using USB device serial numbers and /dev/serial/by-id/ prefix.
Copy bin/config.local.linux over bin/config.local and edit it, should be self-explanatory.

## Build and run

```
bin/idf clean build flash
```

If you have attached LEDs to your boards -- they should start blinking in sync.

If you have connected an oscilloscope to GND and GPIO7 of any two boards -- you should see synchronized 2kHz pulses.

If you have tmux (and hopefully know how to handle it), you can watch output of all devices at once:
```
bin/idf monitor all
```

Otherwise just run monitoring command in 3 separate terminals:
```
bin/idf monitor master
bin/idf monitor slave1
bin/idf monitor slave2
```

## The wrapper script

The wrapper script is quite flexible. You can specify any set of actions: clean, build, flash, monitor. And any set of targets: master, slave, slave1, slave2, all, all-slaves.  Targets 'slave' and 'slave1' are synonyms.

```bash
# Build all firmware
bin/idf clean build 

# Flash specific devices
bin/idf flash master
bin/idf flash slave1
bin/idf flash slave2

# Monitor all devices (opens tmux with multiple panes)
bin/idf monitor all

# Build, flash, and monitor in one command
bin/idf build flash monitor all
```

# Troubleshooting

## Small (few microseconds) fixed phase error

Master and slave initialize wifi slighly differently, so there will be a discrepancy. There should be no significant offset between the slaves after, however.

To fix this must estimate the error and adjust the compensation parameter:
```
components/dtr/include/dtr.h:#define DTR_COMPENSATION_NS -200
```

## Large (hundreds of microseconds) fixed phase error

Make sure the slave start no later then 2 minutes after the master. See https://github.com/abbbe/fts/issues/1.

## Variable (hundreds of nanoseconds) phase error

Check logs on slave's serial console, specifically watch number of successful FTM reports:
```
I (7148) crm: Regression: samples=122 (+63), R²=1.000, std=1.689 ns, ppm_lr_m1=8.323975161, ppm_rl_m1=-8.323905873
```

+63 (out of 64) is very good. If you only get a handful - your mileage will vary.

FTM might fail due to low signal strength, dirty power, or ground loops. Note it is very easy to make yourself a ground loop by plugging usig cheap USB-based scope or logic analyser. Try running your setup from batteries (or laptops running from batteries).

## Serial port problems (WSL)

Windows 11 to WSL Ubuntu serial passthrough can be glitchy. Reboot is likely to fix the issue, but you can also try restart the USB passthrough subsystem.

- unplug USB cables
- kill the passthrough script / close PowerShell
- in Windows: kill usbipd.exe tasks
- in Ubuntu WSL: `modprobe -r vhci-hcd`

If modprobe -r hangs, you have to reboot the PC.

Start it all back:
- in Windows: start 'USBIP Device Host' service
- in Ubuntu WSL: `modprobe vhci-hcd`
- plug in USB cables

In Ubuntu WSL run `ls -l /dev/serial/by-id/` - you should see 3 ports.

## SOS, I am stuck in tmux!!!

Read some tmux manual. If tmux is too confusing -- monitor individual devices in separate terminals.

## Port is busy

Chances are you still have an idf monitor running under tmux. The following might help:
```
tmux attach
Ctrl-B : kill-session
```