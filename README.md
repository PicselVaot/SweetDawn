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
- **Automated tests** — Test protocol handling, configuration, state management, and command registration.

### Planned

- **Persistent daemon** — Keep a single Bluetooth connection alive and manage reconnections.
- **Local IPC** — Allow clients to communicate with the service through a Unix domain socket.
- **MQTT integration** — Publish device state and accept commands from a home automation system.
- **Graphical interface** — Provide a GUI client alongside the command-line interface.

> SweetDawn is under active development. The daemon architecture, IPC, MQTT integration, and graphical interface are planned features, not yet fully implemented.

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

To run the AML communication test explicitly:

```bash
make test-aml
```

This test requires a reachable AML005 device and a working Bluetooth configuration.

## Configuration

SweetDawn uses JSON configuration files.

Create your local device configuration from the provided example, when available, and adjust it to match your Bluetooth adapter and AML005 device.

**Important:** Device configuration may contain Bluetooth addresses specific to your setup. Keep personal configuration files out of version control. Use an example configuration with placeholder values when sharing the project.

## Project structure

```text
src/
├── main.c                  # Application entry point
├── sweetdawn.c / .h        # Application lifecycle and coordination
├── aml.c / aml.h            # Bluetooth transport
├── protocol.c / protocol.h  # AML005 protocol handling
├── state.c / state.h        # Device state
├── config.c / config.h      # Configuration management
└── command/
    ├── command.c / .h       # Command abstractions
    ├── registry.c / .h      # Command registry
    └── aml_commands.c / .h  # AML005 commands

test/                        # Automated tests
config/                      # Configuration examples and local settings
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
- [ ] Persistent background service
- [ ] Automatic Bluetooth reconnection
- [ ] Unix domain socket IPC
- [ ] CLI client communicating with the service
- [ ] MQTT integration
- [ ] Graphical client
- [ ] systemd service integration

## License

SweetDawn is distributed under the **GNU General Public License v3.0 (GPL-3.0)**. See [`LICENSE`](LICENSE) for details.