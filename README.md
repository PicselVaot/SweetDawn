# AML005 Ambient System (Linux)
A Bluetooth-controlled ambient light system driven by a USB camera.

This project captures video from a V4L2 camera, extracts ambient colors, and sends them to an AML005 Bluetooth-controlled device. It also includes a helper tool to synchronize the device clock.

## Features
* USB camera capture via V4L2
* Real-time ambient color extraction
* Bluetooth RFCOMM communication (AML005 protocol)
* Clock synchronization tool
* Systemd services for automation
* Graceful degradation (no camera / no device support)

## Architecture
```
Camera (V4L2)
↓
Frame capture (YUYV)
↓
Color analysis (border sampling)
↓
Smoothing filter
↓
AML005 Bluetooth commands
↓
Ambient light output
```

## Dependencies
Required packages (Debian / Ubuntu / Linux):

```bash
sudo apt update
sudo apt install -y build-essential libbluetooth-dev bluez v4l-utils
```
Optional (for calibration tool):
```bash
sudo apt install -y ffmpeg
```

## Build
```bash
make
```

## Install
```bash
sudo make install PREFIX=/usr/local
```
## Configuration
Create or edit:

/etc/aml005.conf

Example:
```conf
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
RECONNECT_ATTEMPTS=0
LOG_EVERY=120
```

## Reference frame system
The ambient system uses a reference frame for color normalization.

Generate reference frame:
```bash
ffmpeg -f v4l2 -input_format yuyv422 -video_size 1280x720
-i /dev/video2 -frames:v 1 -f rawvideo /etc/aml005-reference.yuyv
```
This file is required for ambient mode.

## Runtime behavior
Normal mode:
* Camera available
* Bluetooth device reachable
* Ambient colors actively updated

---

### Graceful degradation:
#### No camera:
* System enters idle mode
* Bluetooth connection is closed
* System waits for input source
#### No Bluetooth device:
* Camera continues running
* Colors computed but not sent
* System retries connection periodically
#### Invalid configuration:
* Fallback to safe defaults
* Logs warnings and continues if possible

## Calibration mode (recommended future improvement)
If reference frame is missing:
1. Detect missing reference
2. Enter calibration mode
3. Capture frame automatically
4. Save reference
5. Resume normal operation

## Systemd services
```bash
sudo systemctl daemon-reload
sudo systemctl enable --now aml005-ambient.service
sudo systemctl enable --now aml005-sync-clock.service
sudo systemctl enable --now aml005-sync-clock.timer
```

## Tools
Clock sync:
```bash
./aml005_sync_clock -c /etc/aml005.conf -v
```
Ambient daemon:
```bash
./aml005_ambient -c /etc/aml005.conf
```

## Known limitations
* Requires YUYV camera support
* No automatic format fallback yet
* Reference frame is system-dependent
* Bluetooth reconnect logic is basic

## Roadmap
* Add --calibrate mode (remove FFmpeg dependency)
* Automatic camera format fallback
* Robust Bluetooth reconnection
* Safe headless mode
* Config validation system
* Logging levels

## Philosophy
* Never crash on missing hardware
* Degrade gracefully
* Prefer recovery over failure
* Works on any Linux system (not Raspberry-specific)

## License
This project is licensed under the GNU General Public License v3 (GPLv3).

Unless otherwise stated, all files in this repository are distributed under GPLv3.

See the full license text in the LICENSE file.

Official license: https://www.gnu.org/licenses/gpl-3.0.html