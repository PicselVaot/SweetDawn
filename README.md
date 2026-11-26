# SweetDawn

**Control the AML005 sunrise alarm clock from Linux: a C service over Bluetooth, a command-line client, and Home Assistant integration.**

SweetDawn runs on a Raspberry Pi as a background service. It keeps a single Bluetooth connection to the AML005 open, tracks its state, and lets several clients control it at the same time: the `sweetdawn-ctl` command-line client, Home Assistant through MQTT, or any program speaking its JSON protocol.

## Features

- **Full device control** — Light, mood light (color and scenes), FM radio, both alarms (time, days, tone, sunrise), display settings and Bluetooth speaker.
- **Persistent service** — One Bluetooth connection, kept alive and re-established automatically when the AML005 comes back.
- **Clock and next alarm** — Keeps the AML005 clock on time and tells when each alarm will ring next (`in 11 h 05 min`).
- **Live events** — Alarm ringing, sunrise starting, snooze, light changed by a gesture or a button, timers ending: pushed to clients as they happen.
- **Command-line client** — `sweetdawn-ctl Light/Enable true`, answered in about 100 ms through the open connection.
- **Home Assistant** — Every function appears automatically through MQTT discovery, including "next alarm" timestamps usable in automations.
- **Validated commands** — Every value is checked (type, range, allowed choices) before anything is sent to the device.
- **Documented protocol** — The AML005 Bluetooth protocol, recovered from the original Android app and checked on the device: [docs/PROTOCOL.md](docs/PROTOCOL.md).
- **Tested** — Protocol, configuration, state, commands, link supervision, IPC server, and end-to-end command handling against a simulated device.
- **systemd services** — Start at boot, restart on failure, logs in the journal.

### Planned

- **Graphical client**.

## Architecture

```text
                    +-----------+
                    |  AML005   |
                    +-----+-----+
                          | Bluetooth RFCOMM
                    +-----+-----+
                    | SweetDawn |  owns the connection, tracks state,
                    |  service  |  validates and runs commands
                    +-----+-----+
                          | Unix socket, JSON
         +----------------+----------------+
         |                |                |
  sweetdawn-ctl      MQTT bridge      your program
                          |
                   Home Assistant
```

Only the service talks to the AML005, which accepts a single connection at a time. Clients never touch Bluetooth.

## Installation

The project targets Linux and is meant to run on a Raspberry Pi with a Bluetooth adapter.

**1. Dependencies** (Debian / Raspberry Pi OS):

```bash
sudo apt update
sudo apt install build-essential pkg-config libbluetooth-dev libjson-c-dev
sudo apt install python3-paho-mqtt     # only for Home Assistant
```

**2. Build and test:**

```bash
git clone https://github.com/PicselVaot/SweetDawn.git
cd SweetDawn
make
make test
```

The tests use a simulated device: no Bluetooth needed.

**3. Configure your device** from the example:

```bash
cp config/aml.example.json config/aml.json
```

| Field     | Meaning                                    |
|-----------|--------------------------------------------|
| `name`    | Alias used to select the device            |
| `AML_MAC` | Bluetooth address of the AML005            |
| `AML_CHN` | RFCOMM channel of the AML005 (usually 1)   |
| `BLT_MAC` | Bluetooth address of the local adapter (`hciconfig`) |

When no device name is given, the first entry is used. `config/aml.json` contains addresses specific to your setup and is ignored by Git.

**4. Check the hardware** with the one-shot diagnostic, which prints every frame sent and received:

```bash
./bin/sweetdawn
```

**5. Install the service** (requires sudo):

```bash
make install-service
sweetdawn-ctl status
```

**6. Optional: Home Assistant** — see [MQTT bridge](#mqtt-bridge-home-assistant).

## Usage

### Command-line client

`sweetdawn-ctl` sends one command to the running service and prints the answer. It works from any directory.

```bash
sweetdawn-ctl status                      # link, settings, AML clock
sweetdawn-ctl Light                       # read the light state
sweetdawn-ctl Light/Enable true           # turn the light on
sweetdawn-ctl Mood/Scene Wave             # mood light scene
sweetdawn-ctl Radio/>>                    # seek the next station
sweetdawn-ctl Alarm/1                     # alarm 1, with its next trigger
sweetdawn-ctl Alarm/1/Time 07:30          # set alarm 1
sweetdawn-ctl "Alarm/1/Tone volume" 5     # names with spaces need quotes
sweetdawn-ctl "Settings/Set time" 07:45   # set the AML clock
sweetdawn-ctl commands                    # every available command
sweetdawn-ctl events                      # follow events live (Ctrl+C to stop)
```

**Every command, with its values and limits, is listed in [docs/COMMANDS.md](docs/COMMANDS.md).**

Exit code: 0 on success, 1 if the command was refused or failed, 2 if the service is unreachable. Set `SWEETDAWN_SOCKET` to use another socket path.

### Service

```bash
systemctl status sweetdawn          # is it running?
journalctl -u sweetdawn -f          # live log
sudo systemctl restart sweetdawn    # after rebuilding with make
make uninstall-service              # remove it
```

The service reads the device state every 60 s, which also detects a dead link. While the AML005 is off or out of range it keeps trying to reconnect, waiting longer after each failure (up to 30 s).

**AML005 clock.** The device clock cannot be read back, but SweetDawn sets it, so it knows it: it remembers the offset from the Raspberry Pi clock. In `auto` time mode, the service sets the AML005 clock when it connects and once a day, which also follows daylight saving changes. In `manual` mode, the clock may have been changed with the device buttons, so it is reported as unknown.

The IPC socket is `/run/sweetdawn/sweetdawn.sock`. To run the service by hand, from the project root, pick another socket path:

```bash
./bin/sweetdawn-service [device_name] /tmp/sweetdawn.sock
SWEETDAWN_SOCKET=/tmp/sweetdawn.sock sweetdawn-ctl status
```

### One-shot diagnostic

`./bin/sweetdawn [device_name]` connects directly, reads the status and settings, prints every frame in hexadecimal, then exits. Useful to check the hardware or study the protocol. Run it from the project root, with the service stopped (`sudo systemctl stop sweetdawn`): the AML005 accepts only one connection.

### MQTT bridge (Home Assistant)

`mqtt/sweetdawn_mqtt.py` connects the service to an MQTT broker and announces the AML005 to Home Assistant through MQTT discovery:

Entity names start with their group, so related entities stay together in Home Assistant lists (shown as "AML005 Alarm 1 time", for example):

| Group | Entities |
|---|---|
| Alarm | `Alarm state`, `Alarm next`, `Alarm stop`, `Alarm snooze` |
| Alarm 1, Alarm 2 | `Alarm 1` (on/off), `Alarm 1 state`, `Alarm 1 next`, `Alarm 1 time`, `Alarm 1 day 1 Monday` … `day 7 Sunday`, `Alarm 1 snooze`, `Alarm 1 tone`, `Alarm 1 tone volume`, `Alarm 1 sunrise`, `Alarm 1 sunrise duration`, `Alarm 1 sunrise brightness` |
| Light | `Light` (brightness), `Light sunset`, `Light sunset duration`, `Light fade` |
| Mood | `Mood` (brightness, color, scenes as effects), `Mood timer`, `Mood timer duration` |
| Radio | `Radio`, `Radio volume`, `Radio preset`, `Radio frequency`, `Radio seek next` / `previous`, `Radio tune up` / `down`, `Radio sleep timer`, `Radio sleep timer duration` |
| Clock | `Clock` (shows the AML time and sets another one), `Clock mode`, `Clock sync`, `Clock offset` |
| Display | `Display brightness`, `Display time format` |
| Others | `Sound button beep`, `Speaker`, `Bluetooth connection` |

- `… state` sensors: `off`, `sunrise` (sunrise, or snooze waiting to ring again) or `on` (ringing). `Alarm state` covers both alarms, which can overlap. Updated live: while an alarm is active, the service reads its state every second.
- `… next` sensors: timestamp of the next ring; Home Assistant shows "in 11 hours" and can use it in automations.

Commands from Home Assistant go through the service like any other client. Install it:

```bash
cp config/mqtt.example.json config/mqtt.json   # broker address, credentials file
make install-mqtt
journalctl -u sweetdawn-mqtt -f                # live log
```

`config/mqtt.json` points to a credentials file containing `{"user": "...", "passw": "..."}`. Both stay out of Git.

The bridge subscribes to the service events, so alarms and light changes made on the device (buttons, gesture sensor) show up in Home Assistant immediately. It also reads the device state every `poll_seconds` (30 s by default) and one second after each command, since the AML005 does not acknowledge every command. Topics live under `sweetdawn/aml005/`.

### Infrared remote (optional)

`ir/sweetdawn_ir.py` lets an infrared remote control the AML005. Like the MQTT bridge, it is a client of the service and never touches Bluetooth. Everything that depends on the remote is in `config/ir.json`: where to read it, and for each key its code and its action.

| Input (`"input"` → `"type"`) | Receiver | Protocols |
|---|---|---|
| `audio` | demodulated IR receiver (TSOP-like) plugged into a microphone input, e.g. a USB sound card | NEC, decoded by `ir/ir_input.py` (needs `python3-numpy`) |
| `lirc` | receiver handled by the kernel: GPIO (`dtoverlay=gpio-ir`), USB MCE receivers... (`/dev/lirc0`) | whatever the kernel decodes: NEC, RC5, RC6, Sony, JVC, Sanyo, Sharp... (enable them with `ir-keytable -p all`) |
| `evdev` | receiver that shows up as a keyboard (FLIRC, many USB dongles) | any: keys arrive as `key:<code>` |

Codes use the kernel's format (`nec:0x14`, `rc5:0x1e0c`), so a remote keeps its codes whichever receiver reads it. `config/ir.example.json` is a 44-key LED strip remote read through a USB sound card.

```bash
cp config/ir.example.json config/ir.json        # set the input, then:
python3 ir/ir_learn.py listen                   # show the code of each key
python3 ir/ir_learn.py learn                    # name a key, press it: its code goes into config/ir.json
python3 ir/sweetdawn_ir.py --dry-run --press "☼+"   # try a key's action without sending anything
make install-ir                                 # start at boot (journalctl -u sweetdawn-ir -f)
make test-ir                                    # decoder tests, including real noisy recordings
```

Actions (the key's `"action"`, with its parameters):

| Action | Effect |
|---|---|
| `command` (`command`) | any SweetDawn action, e.g. `Alarm/Stop`, `Alarm/Snooze`, `Radio/>>`, `Radio/Save` |
| `light_on`, `light_off`, `light_step` (`step`) | the active light (lamp or mood light): on, off, brightness ± `step` |
| `light_mode`, `mood_mode`, `light_preset` (`percent`) | switch to the lamp or the mood light; lamp at a fixed brightness |
| `scene` (`scene`), `scene_step` (`step`) | a mood scene, or the next / previous one |
| `music_power`, `music_switch` | toggle the active music source; switch between radio and Bluetooth speaker |
| `volume` (`step`), `radio_preset` (`channel`), `radio_preset_step` (`step`) | volume; recall a preset (turns the radio on) |
| `timer` (`minutes`) | sunset / sleep timer on the active light and music; beyond the AML maximum, the service waits and then starts the last 60 minutes on the AML |
| `none` | nothing |

`"repeat": true` repeats the action while the key is held. Switching the active light or music turns off the one in use and turns on the new one. The service remembers the active light and music in `~/.local/state/sweetdawn-ir.json`.

### IPC protocol

Each message is a single JSON object on one line, terminated by `\n`. Requests are at most 4096 bytes. A client may also subscribe to events, which then arrive on the same connection.

Request:

```json
{"type": "request", "id": 1, "command": "Light/Brightness", "args": [75]}
```

`id` is an integer or a string chosen by the client; it is copied into the response.

Successful response (`result` is `{}` for commands that only change something):

```json
{"type": "response", "id": 1, "ok": true, "result": {}}
```

Error response:

```json
{"type": "response", "id": 1, "ok": false, "error": {"code": "INVALID_VALUE", "message": "..."}}
```

#### Commands

The full list of commands is in [docs/COMMANDS.md](docs/COMMANDS.md).

| Command | Args | Effect |
|---|---|---|
| `status` | none | Link state, last known settings and AML clock, answered instantly without querying the device |
| `commands` | none | The full command tree: names, value types, ranges, choices |
| `subscribe` | none | Receive events on this connection from now on (see below) |
| a group, e.g. `Light` | none | Reads that part of the device: `Settings`, `Alarm`, `Alarm/1`, `Alarm/2`, `Light`, `Mood`, `Radio` |
| a setting, e.g. `Light/Brightness` | one value | Changes it; the value is checked first (type, range, allowed choices) |
| an action, e.g. `Radio/>>` | none | Runs it: `Settings/Sync time`, `Alarm/Stop`, `Alarm/Snooze`, `Radio/<`, `Radio/>`, `Radio/<<`, `Radio/>>`, `Radio/Save` |

Values may be sent as JSON strings, numbers or booleans: `[75]`, `["75"]`, `[true]`. Special formats: alarm `Time` is `"HH:MM"`, `Mood/Color` is `"R,G,B"`, alarm `Days` is a bit mask (bit 0 = Sunday … bit 6 = Saturday), `Settings/Set time` is `"HH:MM"`, `"HH:MM:SS"` or `"YYYY-MM-DD HH:MM"`.

`status` result:

```json
{"connected": true, "known": true, "age_ms": 842, "authorization": 22,
 "settings": {"display_brightness": "H", "display_time": "24", "time": "auto", "feedback": true, "fade": true},
 "clock": {"known": true, "time": "2026-10-12T18:27:38+02:00", "offset_s": 0},
 "alarm_state": {"state": "idle", "alarm": null, "alarms": {"1": "idle", "2": "idle"}}}
```

- `connected` — the Bluetooth link is currently open.
- `known` — the state has been read at least once. `age_ms`, `authorization` and `settings` are present only when `known` is true.
- `age_ms` — age of the last state reading. During an outage the last known values are kept; use this field to judge their freshness.
- `clock` — the AML005 clock as set by SweetDawn, and its offset from the system clock; `known` is false in manual time mode or before the first setting.
- `alarm_state` — what the alarm is doing now (see `alarm_state` events below).

An alarm read (`Alarm/1`) also returns its next trigger: `next` (ISO 8601, or `null` if the alarm is off or the clock unknown), `in` (`"11 h 05 min"`) and `in_s` (seconds).

Some device commands are not acknowledged by the AML005 (see [docs/PROTOCOL.md](docs/PROTOCOL.md)): success then means the command was sent. Read the group back to check the effect.

#### Events

After `subscribe`, the service pushes events on the same connection, between responses:

```json
{"type": "event", "event": "alarm_state", "data": {"state": "ringing", "alarm": 2, "alarms": {"1": "idle", "2": "ringing"}}}
{"type": "event", "event": "light_changed", "data": {"light": {...}, "mood": {...}}}
{"type": "event", "event": "timer_ended", "data": {"timer": "radio"}}
{"type": "event", "event": "changed", "data": {"command": "Light/Brightness", "value": "30", "result": {}}}
{"type": "event", "event": "link", "data": {"connected": true}}
```

| Event | When | Data |
|---|---|---|
| `alarm_state` | An alarm state changes | `alarms`: the state of each alarm (`idle`, `sunrise`, `ringing` or `snoozed`); `state` and `alarm`: the most advanced one (ringing, then snoozed, then sunrise), `alarm` being `null` when both are idle |
| `light_changed` | The light or mood light changed on the device (button, gesture sensor, timer) | Both states, as returned by `Light` and `Mood` |
| `timer_ended` | A sleep timer ended | `timer`: `light`, `mood`, `radio` or `music` |
| `changed` | A client (CLI, MQTT bridge, web page...) successfully changed a setting or ran an action | `command`; `value` as text (`null` for an action); `result`: the response sent to that client |
| `link` | The Bluetooth link to the AML opened or was lost | `connected` |

Both alarms are tracked separately, since they can overlap (one ringing during the other's sunrise). The AML005 reports the sunrise and the ringing itself, but nothing when it is tapped (snooze) or stopped with its button. While an alarm is active, the service reads its state every second. When the ringing stops, the state becomes `snoozed` for the alarm's snooze duration: if it does not ring again, it was stopped, and the state returns to `idle`. A stop sent through SweetDawn (`Alarm/Stop`) is known, and gives `idle` at once.

`changed` lets every client follow changes made by the others without reading the device again. It only says the command was accepted: for commands the AML005 does not acknowledge, reading the group remains the only way to see its real state.

The web page receives the same events through `GET /events` (Server-Sent Events), one `data:` line per event.

#### Error codes

| Code | Meaning |
|---|---|
| `INVALID_JSON`, `INVALID_REQUEST`, `UNSUPPORTED_TYPE` | The message itself is malformed |
| `UNKNOWN_COMMAND` | No such command (see `commands`) |
| `INVALID_ARGUMENTS` | Wrong number of args |
| `INVALID_VALUE` | Value out of range, not an allowed choice, or wrong format |
| `COMMAND_NOT_IMPLEMENTED` | The command exists in the tree but cannot be run yet |
| `STATE_UNKNOWN` | Settings not read yet; retry once connected |
| `AML_DISCONNECTED` | The AML005 is not connected right now |
| `AML_TIMEOUT` | The AML005 did not answer |
| `AML_PROTOCOL_ERROR` | Unexpected answer from the AML005 |
| `AML_ERROR`, `COMMAND_FAILED`, `INTERNAL_ERROR` | Other failures |

Quick test from a shell:

```bash
echo '{"type":"request","id":1,"command":"Light","args":[]}' | nc -U -q1 /run/sweetdawn/sweetdawn.sock
```

## Troubleshooting

**`Service injoignable sur /run/sweetdawn/sweetdawn.sock`** — The service is not running. Check `systemctl status sweetdawn` and `journalctl -u sweetdawn -n 50`.

**`AML_DISCONNECTED`, or `connected: false` in `status`** — The service cannot reach the AML005. Check that the device is powered and within range. It only accepts one connection: close the original mobile app, and stop the service before using `./bin/sweetdawn`. The journal shows each attempt (`Connexion à l'AML005...`).

**`connect AML005: Device or resource busy`** (one-shot diagnostic) — Another program holds the connection, usually the service: `sudo systemctl stop sweetdawn` first.

**`AML_TIMEOUT` on every command right after connecting** — The AML005 ignores everything until it receives the status request; the service sends it on each connection. If it persists, restart the service.

**The clock cannot be set, or `clock.known` is false** — In `manual` time mode the AML005 ignores clock updates from outside. `Settings/Set time` and `Settings/Sync time` switch back to `auto` first; `sweetdawn-ctl Settings/Time auto` does it too.

**The mood light always shows Rainbow** — Turning the mood light on always restarts the Rainbow scene, even when it is already on. Send the scene or color after `Mood/Enable true`. The MQTT bridge already does this.

**The alarm state shows `snoozed` although it was stopped** — Stopped with the device button, the AML005 looks exactly like a snoozed alarm. The state returns to `idle` at the end of the snooze duration. Stopping through SweetDawn (`Alarm/Stop`, or the Home Assistant button) is recognized at once.

**A command returns OK but nothing changes** — Some commands are not acknowledged by the AML005 (see [docs/PROTOCOL.md](docs/PROTOCOL.md)). Read the group back (`sweetdawn-ctl Light`) to see the actual state.

**Home Assistant shows no AML005 entities** — Check `journalctl -u sweetdawn-mqtt -n 50`: a `Connexion MQTT refusée` line points to `config/mqtt.json` or the credentials file. Entities show as unavailable while the AML005 is disconnected.

**Service started but `bind` fails with "Address already in use"** — Another instance is running. A socket left over by a crash is replaced automatically; a socket still in use, or a regular file at that path, is never removed.

## Project structure

The code is split into layers, one directory each.

```text
src/
├── app/                     # Programs
│   ├── cli.c                #   bin/sweetdawn: one-shot diagnostic
│   ├── service.c            #   bin/sweetdawn-service: persistent service
│   ├── ctl.c                #   bin/sweetdawn-ctl: command-line client
│   └── handlers.c / .h      #   IPC commands, events, AML clock, next alarm
├── ipc/                     # Talking to clients
│   ├── ipc_server.c / .h    #   Unix domain socket server
│   └── ipc_protocol.c / .h  #   JSON message parsing and responses
├── core/                    # Application logic
│   ├── sweetdawn.c / .h     #   Link supervision, refresh, reconnection
│   ├── control.c / .h       #   One function per device operation
│   ├── state.c / .h         #   Last known device state
│   └── config.c / .h        #   config/aml.json loading
├── command/                 # Command tree (Settings, Alarm, Light, ...)
│   ├── command.c / .h       #   Tree node, value validation
│   ├── registry.c / .h      #   Root and lookup by path
│   └── aml_commands.c / .h  #   The AML005 commands
└── aml/                     # Talking to the AML005
    ├── transport.c / .h     #   Bluetooth RFCOMM connection
    └── protocol.c / .h      #   Frame format, building and parsing

test/                        # One test program per module (fake_aml.c: simulated device)
mqtt/sweetdawn_mqtt.py       # MQTT bridge to Home Assistant
ir/                          # Infrared remote: service, receivers (audio, lirc, evdev), learn tool, tests
docs/COMMANDS.md             # Every command, with values and limits
docs/PROTOCOL.md             # AML005 Bluetooth protocol reference
systemd/                     # systemd unit templates (make install-service, install-mqtt, install-ir)
config/                      # Example configuration (local files are ignored)
```

Data flow in the service:

```text
AML005 <--RFCOMM--> aml/transport <--frames--> core/sweetdawn + core/control <--> app/handlers <--JSON--> ipc/ipc_server <--socket--> clients
```

## Development principles

- **Single ownership:** one service owns the connection to the AML005 device.
- **Separation of concerns:** transport, protocol, commands, state, and client interfaces remain distinct.
- **Testability:** core logic should be testable independently of the physical device whenever possible.
- **Extensibility:** new commands and client interfaces should not require duplicating device communication logic.
- **Privacy:** local device-specific configuration must not be committed to the repository.

## Roadmap

- [x] Bluetooth transport and protocol
- [x] Persistent service with automatic reconnection
- [x] Unix domain socket IPC
- [x] Full device control over IPC
- [x] Command-line client
- [x] AML clock tracking and next alarm
- [x] MQTT integration (Home Assistant)
- [x] systemd services
- [x] Live state change events
- [ ] Graphical client

See [TODO.md](TODO.md) for details.

## License

SweetDawn is distributed under the **GNU General Public License v3.0 (GPL-3.0)**. See [`LICENSE`](LICENSE) for details.
