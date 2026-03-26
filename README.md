# AML005 for Raspberry Pi Zero W

This project contains:

- `include/aml005.h` and `src/aml005.c`: a small C API for AML005 over Bluetooth RFCOMM.
- `src/ambient_daemon.c`: a daemon that captures a USB camera with V4L2, extracts an ambient color from the frame borders, and sends it to the AML005 mood light.
- `src/sync_clock.c`: a small helper that reconnects to the AML005 and updates its clock using the local system time.
- `systemd/*.service` and `systemd/*.timer`: boot + daily midnight automation.

## Dependencies

On Raspberry Pi OS / Debian-like systems:

```bash
sudo apt update
sudo apt install -y build-essential libbluetooth-dev bluez
```

## Build

```bash
make
```

## Install

```bash
sudo make install PREFIX=/usr/local
sudo nano /etc/aml005.conf
```

Example config:

```ini
AML005_MAC=AA:BB:CC:DD:EE:FF
AML005_CHANNEL=1
AML005_TIMEOUT_MS=2000
CAMERA=/dev/video0
WIDTH=640
HEIGHT=480
FPS=15
STRIDE=4
BORDER_RATIO=0.15
COLOR_SMOOTH=0.45
LOG_EVERY=120
```

## Enable services

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now aml005-sync-clock.service
sudo systemctl enable --now aml005-sync-clock.timer
sudo systemctl enable --now aml005-ambient.service
```

## Notes

- The camera code currently expects `YUYV` from V4L2. Many USB webcams support it, but not all. If your camera only exposes MJPEG, this version will refuse to start.
- The AML005 clock is synced:
  - at boot, through `aml005-sync-clock.service`
  - every midnight, through `aml005-sync-clock.timer`
- The ambient daemon also performs one clock sync at startup, before enabling the mood light.
