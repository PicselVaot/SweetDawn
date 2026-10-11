# SweetDawn — Development Roadmap

## Goal

Turn SweetDawn into a persistent background service that centralizes Bluetooth communication with the AML005, maintains its state in memory, and allows multiple clients to control it.

**Core principle:** a single Bluetooth connection, owned by SweetDawn. The CLI, GUI, and MQTT integration communicate with the service, never directly with the AML005.

## Development Milestones

| Status | Step | Objective | Acceptance Criteria |
|---|---|---|---|
| [ ] | 1. IPC Protocol | Define messages and communication between SweetDawn and its clients | Request, response, error, and event formats are documented |
| [ ] | 2. IPC Server | Implement a local server that accepts multiple clients | Multiple clients can connect and exchange messages without blocking the service |
| [ ] | 3. AML Connection | Integrate Bluetooth connection management into the service | SweetDawn exclusively owns the connection and handles disconnections |
| [ ] | 4. Command Handling | Allow clients to execute AML commands through IPC | The CLI sends a command to the service and receives the corresponding result |
| [ ] | 5. Events and State | Notify connected clients of state changes | In-memory state is updated when relevant frames are received, and clients are notified of changes |
| [ ] | 6. MQTT Integration | Expose commands and device state through MQTT | Incoming commands are handled by SweetDawn, and device state is published to configured topics |
| [ ] | 7. systemd Service | Ensure SweetDawn runs continuously | Automatic startup, graceful shutdown, accessible logs, and configurable restart policy |
| [ ] | 8. Documentation | Document installation, configuration, and usage | README, example configuration, and troubleshooting instructions are up to date |

## Technical Requirements

- **Single connection:** no client communicates directly with the AML005.
- **Centralized state:** SweetDawn maintains the known device state and updates it based on received frames.
- **Correlated responses:** each command response can be associated with its corresponding request.
- **Separate events:** state-change notifications are distinct from command responses.
- **Multiple clients:** a slow or disconnected client must not block other clients or communication with the AML005.
- **Resilience:** the service remains available when the AML005 is temporarily unreachable and attempts to reconnect.
- **Testing:** each milestone includes appropriate tests before proceeding to the next one.

## Implementation Order

The milestones are intentionally sequential. Each milestone must be implemented and tested before work begins on the next.

The first priority is to define the IPC protocol, followed by the IPC server. Bluetooth connection management will then be integrated into the service before migrating CLI commands to the new architecture.

The GUI and MQTT integration will subsequently reuse the same communication mechanism without duplicating device-control logic.

## Notes

- Milestones are checked off only after implementation and validation.
- Features already present in the codebase are not considered complete in this roadmap if their integration into the service is still pending.
- Changes to the architecture must preserve the functionality of existing tests.