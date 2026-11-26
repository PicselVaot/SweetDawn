# SweetDawn commands

Every command accepted by the service, through `sweetdawn-ctl` or over IPC (see [IPC protocol](https://github.com/PicselVaot/SweetDawn/wiki/IPC-protocol) in the wiki).

```bash
sweetdawn-ctl <command> [value]
```

Three kinds of commands:

- **reading**: a group without a value (`sweetdawn-ctl Light`) returns its state as JSON;
- **setting**: a command with a value (`sweetdawn-ctl Light/Brightness 75`);
- **action**: a command without a value (`sweetdawn-ctl Radio/>>`).

Every value is checked before it is sent to the AML. A value out of range is refused with `INVALID_VALUE`. Names containing a space must be quoted: `sweetdawn-ctl "Alarm/1/Tone volume" 5`.

This list comes from the tree defined in [`src/command/aml_commands.c`](../src/command/aml_commands.c). `sweetdawn-ctl commands` returns the same thing as JSON, straight from the service.

## Service

| Command | Value | Effect |
|---|---|---|
| `status` | — | Link state, last known settings, AML clock (`clock`) and alarm state (`alarm_state`), without querying the AML |
| `commands` | — | The full command tree (types, limits, choices) |
| `events` | — | With `sweetdawn-ctl`: shows events live (Ctrl+C to stop). Over IPC, the command is called `subscribe` |

## Settings — device settings

| Command | Value | Effect |
|---|---|---|
| `Settings` | — | **Reading**: `display_brightness`, `display_time`, `time`, `feedback`, `fade` |
| `Settings/Display brightness` | `L`, `H`, `A` | Display brightness: low, high, automatic (no visible effect on the test device, to be confirmed: see [PROTOCOL.md](PROTOCOL.md#display-and-buttons)) |
| `Settings/Display time` | `24`, `12` | Format of the displayed time |
| `Settings/Time` | `auto`, `manual` | Time mode; `auto` also sets the AML clock |
| `Settings/Feedback` | `true`, `false` | Button beep |
| `Settings/Fade` | `true`, `false` | Light fading |
| `Settings/Sync time` | — | **Action**: sets the AML clock to the Raspberry Pi time |
| `Settings/Set time` | `HH:MM`, `HH:MM:SS` or `YYYY-MM-DD HH:MM` | Sets the AML clock to the chosen time; without a date, keeps today's |

**AML clock.** The AML clock cannot be read back: it can only be set. SweetDawn knows it anyway, since it is the one setting it. It remembers the offset from the Raspberry Pi clock (`status` → `clock`). In `auto` mode, the service sets the AML clock on its own: as soon as it connects, then once a day, keeping the offset chosen with `Set time`. In `manual` mode, the time may have been changed with the buttons: it is then unknown, and so is `next`.

In `manual` mode, the time is set with the AML buttons, and the AML then ignores any time setting from outside. `Sync time` and `Set time` therefore switch back to `auto` first.

## Alarm — alarms

`<n>` is `1` or `2`.

| Command | Value | Effect |
|---|---|---|
| `Alarm` | — | **Reading** of both alarms |
| `Alarm/<n>` | — | **Reading**: `enable`, `time`, `days`, `snooze`, `tone`, `tone_volume`, `sunrise`, `sunrise_duration`, `sunrise_brightness`, and the next trigger: `next`, `in`, `in_s` |
| `Alarm/<n>/Enable` | `true`, `false` | Enables or disables the alarm |
| `Alarm/<n>/Time` | `HH:MM` | Alarm time, e.g. `07:30` |
| `Alarm/<n>/Days` | 0 to 127 | Active days (see below) |
| `Alarm/<n>/Snooze` | `1`, `2`, `5`, `10`, `20`, `30` | Snooze duration, in minutes |
| `Alarm/<n>/Tone` | `Buzzer`, `Radio`, `Melody 1`, `Birds`, `Bells`, `Piano`, `Melody 2` | Ringtone |
| `Alarm/<n>/Tone volume` | 1 to 10 | Ringtone volume |
| `Alarm/<n>/Sunrise` | `true`, `false` | Sunrise before the alarm |
| `Alarm/<n>/Sunrise duration` | 2 to 60 | Sunrise duration, in minutes |
| `Alarm/<n>/Sunrise brightness` | 1 to 100 | Brightness reached at the end of the sunrise |
| `Alarm/Stop` | — | **Action**: stops the ringing alarm |
| `Alarm/Snooze` | — | **Action**: rings the ringing alarm again later |

**Next trigger**: `next` is the actual date and time of the next ring (`2026-10-13T07:00:00+02:00`), `in` the readable delay (`11 h 05 min`) and `in_s` that delay in seconds. `next` is `null` if the alarm is disabled, or if the AML clock is not known (see above).

**Days**: add up the values of the days wanted.

| Sun | Mon | Tue | Wed | Thu | Fri | Sat |
|---|---|---|---|---|---|---|
| 1 | 2 | 4 | 8 | 16 | 32 | 64 |

Examples: Monday to Friday = 2 + 4 + 8 + 16 + 32 = **62**; weekend = 1 + 64 = **65**; every day = **127**.

## Light — main lamp

| Command | Value | Effect |
|---|---|---|
| `Light` | — | **Reading**: `enable`, `brightness`, `sunset`, `sunset_duration` |
| `Light/Enable` | `true`, `false` | Turns on or off |
| `Light/Brightness` | 0 to 100 | Brightness |
| `Light/Sunset` | `true`, `false` | Sunset: gradual turning off |
| `Light/Sunset duration` | 1 to 60 | Sunset duration, in minutes |

## Mood — mood light

| Command | Value | Effect |
|---|---|---|
| `Mood` | — | **Reading**: `enable`, `brightness`, `timer`, `timer_duration`, `color`, `scene` |
| `Mood/Enable` | `true`, `false` | Turns on or off |
| `Mood/Color` | `R,G,B` | Colour, each component from 0 to 255, e.g. `255,120,0` |
| `Mood/Brightness` | 0 to 100 | Brightness |
| `Mood/Scene` | `Off`, `Random`, `Rainbow`, `Rainbow Slow`, `Fusion`, `Pulse`, `Wave`, `Chill`, `Action`, `Forest`, `Summer` | Animated scene |
| `Mood/Timer` | `true`, `false` | Turn-off timer |
| `Mood/Timer duration` | 1 to 120 | Timer duration, in minutes |

Good to know:

- **Turning the mood light on always starts the `Rainbow` scene**, even when it is already on. For another scene or a colour, send it **after** `Mood/Enable true`.
- The `Off` scene does not turn the mood light off: it stops the animation and shows the fixed colour (`Mood/Color`).

## Radio — FM radio

| Command | Value | Effect |
|---|---|---|
| `Radio` | — | **Reading**: `enable`, `channel`, `frequency` (MHz), `volume`, `sleep_timer`, `timer_duration` |
| `Radio/Enable` | `true`, `false` | Turns the radio on or off |
| `Radio/>` | — | **Action**: +0.1 MHz; returns the new `frequency` |
| `Radio/<` | — | **Action**: −0.1 MHz |
| `Radio/>>` | — | **Action**: seeks the next station |
| `Radio/<<` | — | **Action**: seeks the previous station |
| `Radio/Channel` | 1 to 10 | Recalls a preset |
| `Radio/Save` | — | **Action**: stores the current frequency in the current preset |
| `Radio/Frequency` | 87.5 to 108 | Tunes directly to a frequency, in MHz; presets do not change |
| `Radio/Presets` | — | **Action**: returns the frequencies of the 10 presets (`presets`) |
| `Radio/Volume` | 1 to 10 | Volume |
| `Radio/Sleep timer` | `true`, `false` | Sleep timer |
| `Radio/Timer duration` | 1 to 60 | Delay before turning off, in minutes |

`Radio/Frequency` goes through the preset list: the frequency is written into the current preset, recalled, then the original list is put back. Presets therefore do not change, but the frequency played no longer matches the preset shown (as after `Radio/>>`).

**Storing a station**: recall the preset to replace (`Radio/Channel 3`), go to the station (`Radio/Frequency`, `Radio/>>`, `Radio/<<`, `Radio/>`, `Radio/<`), then `Radio/Save`.

## Raw — raw frame

| Command | Value | Effect |
|---|---|---|
| `Raw` | hexadecimal bytes: code then data | Sends the frame (the envelope is added), listens for 1.5 s and returns `sent` and `responses` (`code`, `data`, `frame` of every frame received) |

To explore the protocol without stopping the service, e.g. `sweetdawn-ctl Raw "04 01 02"`. Nothing is checked: a badly chosen frame can change any AML setting.

## Media Player — Bluetooth speaker

| Command | Value | Effect |
|---|---|---|
| `Media Player/Enable` | `true`, `false` | Enables or disables the Bluetooth speaker mode |

Reading the state (`Media Player`) is not available yet.

## Commands without acknowledgement

The AML does not confirm some commands: turning on, turning off, brightness, colour, scene, timers, radio volume. For them, `OK` only means the command was sent. Reading the group back shows the effect. Details in [PROTOCOL.md](PROTOCOL.md).
