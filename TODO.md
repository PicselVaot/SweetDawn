# SweetDawn — Development Roadmap

## Goal

Turn SweetDawn into a persistent background service that centralizes Bluetooth communication with the AML005, maintains its state in memory, and allows multiple clients to control it.

**Core principle:** a single Bluetooth connection, owned by SweetDawn. The CLI, GUI, and MQTT integration communicate with the service, never directly with the AML005.

## Development Milestones

| Status | Step | Objective | Acceptance Criteria |
|---|---|---|---|
| [x] | 1. IPC Protocol | Define messages and communication between SweetDawn and its clients | Request, response, error, and event formats are documented |
| [x] | 2. IPC Server | Implement a local server that accepts multiple clients | Multiple clients can connect and exchange messages without blocking the service |
| [x] | 3. AML Connection | Integrate Bluetooth connection management into the service | SweetDawn exclusively owns the connection and handles disconnections |
| [x] | 4. Command Handling | Allow clients to execute AML commands through IPC | The CLI sends a command to the service and receives the corresponding result |
| [x] | 5. Events and State | Notify connected clients of state changes | In-memory state is updated when relevant frames are received, and clients are notified of changes |
| [x] | 6. MQTT Integration | Expose commands and device state through MQTT | Incoming commands are handled by SweetDawn, and device state is published to configured topics |
| [ ] | 7. systemd Service | Ensure SweetDawn runs continuously | Automatic startup, graceful shutdown, accessible logs, and configurable restart policy |
| [x] | 8. Documentation | Document installation, configuration, and usage | README, example configuration, and troubleshooting instructions are up to date |

### Status notes

- **Step 1:** request, response, error and event formats are implemented and documented in the README (IPC protocol).
- **Step 4:** every AML command is reachable through IPC and tested against a simulated device. Verified on the device: all reads, acknowledged writes (settings, alarms, clock), light on/off, radio on/off, mood scenes and color. Not yet checked for their visible effect: volumes, timers, sunset, Bluetooth speaker.
- **Step 5:** unsolicited frames (`C6` sunrise, `C7` ringing, `CA` light changed, `EB`–`EE` timers) are dispatched by the service and pushed to subscribed clients as `alarm_state`, `light_changed` and `timer_ended` events. Snooze and stop send nothing: the alarm state is read every 10 s while an alarm is active. Verified on the device: ringing, snooze (tap), light changes by gesture. Limitation: a stop with the device button looks like a snooze until the snooze duration ends.
- **Step 6:** the MQTT bridge subscribes to the service events (alarm state and light changes are live) and still polls every 30 s as a fallback.
- **Step 7:** both services (`sweetdawn`, `sweetdawn-mqtt`) are installed, start at boot, restart on failure, and log to the journal. Remaining before checking it off: confirm they come back after a reboot of the Raspberry Pi.
- **Step 8:** README rewritten with installation from scratch, usage, MQTT, IPC reference and troubleshooting; every command in docs/COMMANDS.md; protocol in docs/PROTOCOL.md.

## Beyond the milestones

- **Bluetooth speaker state:** the read command is known (`17`) but not exposed (`Media Player` group read).
- **Alarm with no day selected:** the next trigger assumes it rings once, at the next occurrence of its time. To confirm on the device.
- **Unknown codes:** `05`, `06` and `11` answer with one byte of unknown meaning (docs/PROTOCOL.md).
- **AML clock after a service restart:** an offset chosen with `Settings/Set time` is not saved; after a restart the service sets the AML005 back to the Raspberry Pi time.
- **Graphical client:** possibly unnecessary now that Home Assistant covers it.

## Technical Requirements

- **Single connection:** no client communicates directly with the AML005.
- **Centralized state:** SweetDawn maintains the known device state and updates it based on received frames.
- **Correlated responses:** each command response can be associated with its corresponding request.
- **Separate events:** state-change notifications are distinct from command responses.
- **Multiple clients:** a slow or disconnected client must not block other clients or communication with the AML005.
- **Resilience:** the service remains available when the AML005 is temporarily unreachable and attempts to reconnect.
- **Testing:** each milestone includes appropriate tests before proceeding to the next one.

## Notes

- Milestones are checked off only after implementation and validation.
- Changes to the architecture must preserve the functionality of existing tests.
