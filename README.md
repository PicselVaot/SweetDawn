# SweetDawn

**A C-based control layer for AML005 devices over Bluetooth.**

SweetDawn aims to provide a reliable and extensible way to communicate with AML005 devices, maintain their state, and expose their functionality to other applications.

The project is designed to run as a persistent background service, allowing multiple clients to interact with the same device without managing the Bluetooth connection themselves.

## Features

- **Bluetooth communication** — Connect to an AML005 device over Bluetooth RFCOMM.
- **Protocol handling** — Build, send, receive, and parse AML005 protocol frames.
- **Command system** — Organize device commands in a structured and extensible registry.
- **State management** — Maintain an in-memory representation of the device state.
- **Configuration** — Load device and application settings from JSON.
- **Persistent service** — Keep a single Bluetooth connection alive, refresh the device state periodically, and reconnect automatically with backoff.
- **Local IPC** — Serve multiple clients over a Unix domain socket with newline-delimited JSON.
- **Command-line client** — `sweetdawn-ctl Light/Enable true`, answered in about 100 ms through the open connection.
- **systemd integration** — Start the service at boot and restart it on failure.
- **Home Assistant (MQTT)** — Every function of the device appears automatically in Home Assistant: light, mood light, radio, alarms, settings.
- **Device control** — Read and change settings, alarms, light, mood light, radio and Bluetooth speaker through IPC.
- **Automated tests** — Test protocol handling, configuration, state management, command registration, link supervision, and IPC.

### Planned

- **State events** — Notify connected clients when the device state changes.
- **Graphical interface** — Provide a GUI client alongside the command-line interface.

> SweetDawn is under active development. Features listed under *Planned* are not implemented yet.

## Architecture

The long-term goal is to centralize device communication in a single service:

```text
                  +----------------+
                  |      AML005    |
                  +-------+--------+
                          |
                       Bluetooth
                          |
                  +-------+--------+
                  |    SweetDawn   |
                  |   C service    |
                  |                |
                  |  Device state  |
                  |  Command logic |
                  +-------+--------+
                          |
                     Local IPC
                          |
              +-----------+-----------+
              |           |           |
           CLI client  GUI client  MQTT bridge
```

SweetDawn will own the Bluetooth connection and maintain the authoritative device state. Clients will send commands to the service and receive responses or state updates.

This design avoids having multiple applications independently connect to the same device.

## Requirements

The project targets Linux and is intended to run on a Raspberry Pi.

Build dependencies on Debian-based systems:

```bash
sudo apt update
sudo apt install build-essential pkg-config libbluetooth-dev libjson-c-dev
```

A compatible Bluetooth adapter and an accessible AML005 device are required for live communication tests.

## Build

Clone the repository:

```bash
git clone https://github.com/PicselVaot/SweetDawn.git
cd SweetDawn
```

Compile the project:

```bash
make
```

The build produces the project binaries in the `bin/` directory.

## Tests

Run the automated test suite:

```bash
make test
```

The test suite covers the core components without requiring a live Bluetooth connection.

To check communication with a real AML005, run the one-shot CLI (see [Usage](#usage)): it prints every frame sent and received.

## Configuration

SweetDawn reads its devices from `config/aml.json`. Create it from the provided example:

```bash
cp config/aml.example.json config/aml.json
```

Then adjust each entry to your setup:

| Field     | Meaning                                    |
|-----------|--------------------------------------------|
| `name`    | Alias used to select the device            |
| `AML_MAC` | Bluetooth address of the AML005            |
| `AML_CHN` | RFCOMM channel of the AML005               |
| `BLT_MAC` | Bluetooth address of the local adapter     |

When no device name is given on the command line, the first entry is used.

**Important:** `config/aml.json` contains addresses specific to your setup and is ignored by Git. Only `config/aml.example.json`, with placeholder values, is versioned.

## Usage

Both programs must be started from the project root, where `config/` is located.

### One-shot CLI

Connect, read the device status and settings, print every frame in hexadecimal, then exit. Useful to check the hardware or study the protocol:

```bash
./bin/sweetdawn [device_name]
```

The AML005 accepts only one connection at a time: stop the service first (`sudo systemctl stop sweetdawn`), then start it again afterwards.

### Service

The service keeps a single Bluetooth connection to the AML005 open, reconnects automatically, and executes commands sent by clients. Install it as a systemd service (requires sudo):

```bash
make install-service
```

It then starts at boot and restarts after a crash. Useful commands:

```bash
systemctl status sweetdawn          # is it running?
journalctl -u sweetdawn -f          # live log
sudo systemctl restart sweetdawn    # after rebuilding with make
make uninstall-service              # remove it
```

The IPC socket is `/run/sweetdawn/sweetdawn.sock`. To run the service by hand instead, pick another socket path:

```bash
./bin/sweetdawn-service [device_name] /tmp/sweetdawn.sock
```

### Command-line client

`sweetdawn-ctl` sends one command to the running service and prints the answer. `make install-service` links it into `/usr/local/bin`:

```bash
sweetdawn-ctl status
sweetdawn-ctl Light                       # read the light state
sweetdawn-ctl Light/Enable true           # turn the light on
sweetdawn-ctl Radio/>>                    # seek the next station
sweetdawn-ctl Alarm/1/Time 07:30          # set alarm 1
sweetdawn-ctl "Alarm/1/Tone volume" 5     # names with spaces need quotes
sweetdawn-ctl commands                    # every available command
```

**Every available command, with its values and limits, is listed in [docs/COMMANDS.md](docs/COMMANDS.md).**

Exit code: 0 on success, 1 if the command was refused or failed, 2 if the service is unreachable. Set `SWEETDAWN_SOCKET` to use another socket path.

### MQTT bridge (Home Assistant)

`mqtt/sweetdawn_mqtt.py` connects the service to an MQTT broker and announces every function of the AML005 to Home Assistant (MQTT discovery): lights, radio, alarms with their days, settings, and a Bluetooth connectivity sensor. Commands from Home Assistant go through the service like any other client.

Requires `python3-paho-mqtt`. Configure it from the example, then install it as a systemd service:

```bash
cp config/mqtt.example.json config/mqtt.json   # broker address, credentials file
make install-mqtt
journalctl -u sweetdawn-mqtt -f                # live log
```

`config/mqtt.json` points to a credentials file containing `{"user": "...", "passw": "..."}`. Both stay out of Git.

The bridge reads the device state every `poll_seconds` (30 s by default) and one second after each command, since the AML005 does not acknowledge every command. Topics live under `sweetdawn/aml005/`.

### IPC protocol

Each message is a single JSON object terminated by a newline (`\n`), at most 4096 bytes long.

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
| `status` | none | Link state and last known settings, answered instantly without querying the device |
| `commands` | none | The full command tree: names, value types, ranges, choices |
| a group, e.g. `Light` | none | Reads that part of the device: `Settings`, `Alarm`, `Alarm/1`, `Light`, `Mood`, `Radio` |
| a setting, e.g. `Light/Brightness` | one value | Changes it; the value is checked first (type, range, allowed choices) |
| an action, e.g. `Radio/>>` | none | Runs it: `Settings/Sync time`, `Alarm/Stop`, `Alarm/Snooze`, `Radio/<`, `Radio/>`, `Radio/<<`, `Radio/>>` |

Values may be sent as JSON strings, numbers or booleans: `[75]`, `["75"]`, `[true]`. Special formats: alarm `Time` is `"HH:MM"`, `Mood/Color` is `"R,G,B"`, alarm `Days` is a bit mask (bit 0 = Sunday … bit 6 = Saturday).

`status` result:

```json
{"connected": true, "known": true, "age_ms": 842, "authorization": 22,
 "settings": {"display_brightness": "H", "display_time": "24", "time": "auto", "feedback": true, "fade": true}}
```

- `connected` — the Bluetooth link is currently open.
- `known` — the state has been read at least once. The other fields are present only when `known` is true.
- `age_ms` — age of the last state reading (refreshed every 60 s). During an outage the last known values are kept; use this field to judge their freshness.

Some device commands are not acknowledged by the AML005 (see [docs/PROTOCOL.md](docs/PROTOCOL.md)): success then means the command was sent. Read the group back to check the effect.

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

## Project structure

The code is split into layers, one directory each.

```text
src/
├── app/                     # Programs
│   ├── cli.c                #   bin/sweetdawn: one-shot diagnostic
│   ├── service.c            #   bin/sweetdawn-service: persistent service
│   ├── ctl.c                #   bin/sweetdawn-ctl: command-line client
│   └── handlers.c / .h      #   IPC commands of the service
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

test/                        # One test program per module (fake_aml.c: fake device)
mqtt/sweetdawn_mqtt.py       # MQTT bridge to Home Assistant
docs/COMMANDS.md             # Every command, with values and limits
docs/PROTOCOL.md             # AML005 Bluetooth protocol reference
systemd/                     # systemd unit templates (make install-service, install-mqtt)
config/                      # Example configuration (local aml.json is ignored)
```

Data flow in the service:

```text
AML005 <--RFCOMM--> aml/transport <--frames--> core/sweetdawn + core/control <--> app/handlers <--JSON--> ipc/ipc_server <--socket--> clients
```

## Development principles

SweetDawn follows a few core principles:

- **Single ownership:** one service owns the connection to the AML005 device.
- **Separation of concerns:** transport, protocol, commands, state, and client interfaces remain distinct.
- **Testability:** core logic should be testable independently of the physical device whenever possible.
- **Extensibility:** new commands and client interfaces should not require duplicating device communication logic.
- **Privacy:** local device-specific configuration must not be committed to the repository.

## Roadmap

- [x] Bluetooth transport
- [x] Protocol parsing
- [x] JSON configuration
- [x] Command registry
- [x] In-memory device state
- [x] Core automated tests
- [x] Persistent background service
- [x] Automatic Bluetooth reconnection
- [x] Unix domain socket IPC
- [x] Device control commands over IPC
- [ ] State change events
- [x] CLI client communicating with the service
- [x] MQTT integration
- [ ] Graphical client
- [x] systemd service integration

## License

SweetDawn is distributed under the **GNU General Public License v3.0 (GPL-3.0)**. See [`LICENSE`](LICENSE) for details.