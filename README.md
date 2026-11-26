# SweetDawn

**Control the AML005 sunrise alarm clock from Linux, over Bluetooth.**

## Why

The AML005 is a sunrise alarm clock: a lamp that brightens slowly before the alarm, a coloured mood light, an FM radio and a Bluetooth speaker. Out of the box, it is driven by its buttons and by an Android app, over Bluetooth, one phone at a time.

SweetDawn takes the phone out of the loop. It runs on a Raspberry Pi next to the device and makes the AML005 part of the home: set the alarm from Home Assistant, turn the light on from a browser, change the radio station with an old infrared remote, react in automations when the alarm rings.

There was no documentation of the device: its Bluetooth protocol was recovered from the original app and checked on the device, command by command. It is documented in [docs/PROTOCOL.md](docs/PROTOCOL.md), including things the app itself never did, such as tuning the radio to a given frequency.

## What it does

- **Every function of the device**: light, mood light (colour and scenes), FM radio, both alarms with their sunrise, display settings, Bluetooth speaker.
- **A service that stays connected**: one Bluetooth connection, kept open and re-established on its own; the AML005 clock is kept on time.
- **Many ways to control it, at the same time**: a command-line client, a web page, Home Assistant (MQTT discovery), an infrared remote, or your own program.
- **Live**: alarm ringing, sunrise, snooze, a light changed with a button or the gesture sensor… every client knows at once.
- **Careful**: every value is checked before anything reaches the device; tests run against a simulated AML005.

## How it works

```text
                       AML005
                         │ Bluetooth (RFCOMM)
               ┌─────────┴─────────┐
               │ SweetDawn service │  owns the connection, tracks the state,
               │   (C, systemd)    │  validates and runs commands
               └─────────┬─────────┘
                         │ JSON over a Unix socket (and HTTP)
   ┌─────────────┬───────┴──────┬───────────────┬────────────────┐
sweetdawn-ctl  web page    MQTT bridge    infrared remote   your program
                                │
                         Home Assistant
```

The AML005 accepts a single connection: only the service talks to it, and every client goes through the service.

## Getting started

```bash
git clone https://github.com/PicselVaot/SweetDawn.git && cd SweetDawn
make && make test
cp config/aml.example.json config/aml.json     # the Bluetooth address of your AML005
make install-service
sweetdawn-ctl status
```

The full step-by-step guide is in the **[wiki](https://github.com/PicselVaot/SweetDawn/wiki)**.

## Documentation

| | |
|---|---|
| [Installation](https://github.com/PicselVaot/SweetDawn/wiki/Installation) | from a fresh Raspberry Pi to the running service |
| [Command line and service](https://github.com/PicselVaot/SweetDawn/wiki/Command-line-and-service) | `sweetdawn-ctl`, the service, the AML clock |
| [Home Assistant](https://github.com/PicselVaot/SweetDawn/wiki/Home-Assistant) | the MQTT bridge and its entities |
| [Web page](https://github.com/PicselVaot/SweetDawn/wiki/Web-page) | the built-in control page |
| [Infrared remote](https://github.com/PicselVaot/SweetDawn/wiki/Infrared-remote) | use any remote: receivers, learning keys, actions |
| [Radio](https://github.com/PicselVaot/SweetDawn/wiki/Radio) | direct tuning and presets |
| [IPC protocol](https://github.com/PicselVaot/SweetDawn/wiki/IPC-protocol) | write your own client |
| [Troubleshooting](https://github.com/PicselVaot/SweetDawn/wiki/Troubleshooting) | when something does not work |
| [Development](https://github.com/PicselVaot/SweetDawn/wiki/Development) | code structure, tests, exploring the protocol |
| [docs/COMMANDS.md](docs/COMMANDS.md) | every command, with its values and limits |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | the AML005 Bluetooth protocol |

## License

SweetDawn is free software under the **GNU General Public License v3.0**: you may use, study, modify and share it; if you distribute it, modified or not, you must do so under the same license, with its source code. See [LICENSE](LICENSE).
