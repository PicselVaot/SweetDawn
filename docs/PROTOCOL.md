# AML005 Bluetooth protocol

Reference of the exchange between SweetDawn and the AML005, recovered from the original Android app (SunBeam, decompiled), then checked on the device where marked (✔).

In the code, the codes are defined in [`src/aml/protocol.h`](../src/aml/protocol.h) and each command is implemented in [`src/core/control.c`](../src/core/control.c).

## Link

- Classic Bluetooth, RFCOMM, usually channel 1 (see `config/aml.json`).
- **The AML answers nothing until it has received the status request** (`00 16`) ✔. SweetDawn sends it on every connection.
- The AML accepts only one connection at a time ✔: while the service runs, the mobile app and `bin/sweetdawn` cannot connect. If the mobile app connects, it cuts the current link ✔.
- The AML beeps on every new connection ✔.

## Frame format

```
byte :  0  1  2  3  4  5  6  7  8 ...       n-3 n-2 n-1
       FE EF TT LL AB KK IL CC [data]   XX  55  0D  0A
```

| Field   | Role                                                              |
|---------|-------------------------------------------------------------------|
| `FE EF` | start of frame                                                    |
| `TT`    | `0A` request, `0C` response                                       |
| `LL`    | total length of the frame minus 4                                 |
| `AB KK` | `AB AA` request, `AB BB` response                                 |
| `IL`    | inner length: code + data + checksum + `55`                       |
| `CC`    | command or response code                                          |
| `XX`    | checksum: XOR from `IL` up to the last data byte included         |
| `55`    | inner end marker                                                  |
| `0D 0A` | end of frame                                                      |

Example ✔: status `FE EF 0A 09 AB AA 04 00 16 12 55 0D 0A`, response `FE EF 0C 09 AB BB 04 F0 16 E2 55 0D 0A`.

In the tables below, only the **data** is shown.

## Commands

A request that has a response gets a different code (*Response* column). "—": the AML does not answer, the original app expects nothing.

### Device and settings

| Request | Response | Data sent | Data received |
|---|---|---|---|
| `00` status ✔ | `F0` | `16` | authorization: `16` (22) = control granted |
| `12` read settings ✔ | `E2` | — | display, date, time, feedback, fade |
| `02` write settings ✔ | `F2` | display, date, time, feedback, fade | — |
| `01` set the clock | `F1` | s, min, h, weekday (0 = Sun.), day, month, year − 2000, `02` | — |

Setting values:
- `display` (display brightness): 0 = L, 1 = H, 2 = auto;
- `date` (display format): 0 = 24 h, 1 = 12 h;
- `time`: 0 = auto, 1 = manual (the app sets the AML clock when switching to auto). **In manual mode, the AML does not answer the clock setting `01`** ✔;
- `feedback`, `fade`: **inverted**, 0 = enabled, 1 = disabled.

The five settings are always written together.

### Alarms

| Request | Response | Data sent | Data received |
|---|---|---|---|
| `03` read ✔ | `F3` | index \| `80`, `02` | see below |
| `03` write ✔ | `F3` | index, enable, min, h, days, tone, volume, snooze, sunrise, duration, brightness | — |
| `25` state | `C5` | — | sunrise in progress, alarm in progress |
| `28` snooze | `C8` | — | result |
| `29` stop | `C9` | — | result |

- index: `01` = alarm 1 (`Alarm/1`), `07` = alarm 2 (`Alarm/2`). When reading, add `80`.
- Data received when reading: index \| `80`, enable, **minute, hour** (in that order), days, tone, volume, snooze, sunrise, sunrise duration, brightness.
- days: one bit per day, bit 0 = Sunday, bit 1 = Monday … bit 6 = Saturday.
- snooze: index in 1, 2, 5, 10, 20, 30 minutes.
- tone: index (0 = Buzzer, 1 = Radio, 2 and above = melodies).

### Lighting

First byte of the commands: the target, `01` = Light (main lamp), `02` = Mood (mood light).

| Request | Response | Data sent | Data received |
|---|---|---|---|
| `30` read ✔ | `D0` | target | see below |
| `37` turn on | — | target | |
| `35` turn off | — | target | |
| `31` brightness | — | target, 0–100 | |
| `38` timer on | — | target | |
| `36` timer off | — | target | |
| `33` timer duration | — | target, minutes | |
| `32` colour (Mood) | — | R, G, B | |
| `34` scene (Mood) | — | scene index | |

- Read response ✔: target, enable, brightness, timer on, duration. For Mood only, it goes on with R, G, B, scene (9 bytes in all, 5 for Light).
- For Light, the timer is the "sunset" (gradual turning off).
- Turning Mood on (`37 02`) restarts scene 2 (Rainbow), even when it was already on ✔. The original app takes this into account.
- Scene 0 (Off) = fixed colour, no animation.
- Scenes: 0 Off, 1 Random, 2 Rainbow, 3 Rainbow Slow, 4 Fusion, 5 Pulse, 6 Wave, 7 Chill, 8 Action, 9 Forest, 10 Summer.

### FM radio

| Request | Response | Data sent | Data received |
|---|---|---|---|
| `07` read ✔ | `F7` | `02` | enable, preset (1–10), frequency (2 bytes), volume, timer on, duration |
| `08` on/off | `F8` | 1 / 0 | |
| `0A` change the frequency | `FA` | seek (0 fine step, 1 seek), direction (0 up, 1 down) | echo of the 2 bytes, frequency (2 bytes) |
| `09` preset | `F9` | number 1–10 | |
| `0B` volume | — | 1–10 | |
| `0C` timer on | — | 1 / 0 | |
| `0D` timer duration | — | minutes | |
| `04` read the presets ✔ | `F4` | `01 02` | direction, on, 10 frequencies, volume, timer on, duration |
| `04` write the presets ✔ | — | `00`, on, 10 frequencies, volume, timer on, duration | |
| `0E` store preset | `FE` | — | |

Frequency: 2-byte integer, most significant byte first, in tenths of MHz (`03 F7` = 1015 = 101.5 MHz).

**Choosing a frequency** ✔ (found on 26/11/2026, not in the original app): `04` with `00` as first byte **writes** the 10 presets (same layout as the read response, no answer from the AML), then `09 <n>` plays preset n. SweetDawn uses it for `Radio/Frequency`, then puts the original list back.

- The "on" byte of the `04` write turns the radio on or off: put the current state in it.
- `04` with `01` (or `81`) as first byte stays a read, whatever data follows.
- `09` only reads its first byte; `09 00` goes to a hidden preset 0 (frequency `01 00` = 25.6 MHz, invalid).
- `08` followed by extra data turns the radio off.
- `20` and `07` with a full state as data (valid frequency included) only read; `0A` ignores modes other than 0 and 1.

### Bluetooth speaker (Media Player)

| Request | Response | Data sent | Data received |
|---|---|---|---|
| `10` enable | `E0` | — | type |
| `24` disable | — | — | |
| `0F` A2DP state | `FF` | — | type, address (12 characters) |
| `17` read | `E7` | — | volume, timer on, duration |
| `14` volume | — | volume | |
| `15` timer on | `E5` | 1 / 0 | success |
| `16` timer duration | — | minutes | |

## Request / response codes

The response code follows from the request code:

| Requests | Response |
|---|---|
| `0X` | `FX` |
| `1X` | `EX` |
| `2X` | `CX` |
| `3X` | `DX` |

Spontaneous messages follow the same rule: `C7` (alarm) is the response to `27`, `EB` the one to `1B`, etc.

## Codes found by exploration

On 12/10/2026, every code the app does not use, from `00` to `7F`, was sent without data to the device. After each one, the full state was read back. Only these answered ✔:

| Request | Response | Data received | Interpretation |
|---|---|---|---|
| `05` | `F5` | `01` | unknown |
| `06` | `F6` | `00` | unknown |
| `11` | `E1` | `01` | unknown (it is not the clock) |
| `20` | `C0` | as `07` | radio state |
| `21` | `C1` | as `17` | music state; **changed the music timer duration** (1 → 10) |
| `22` | `C2` | as `30 01` | Light state |
| `23` | `C3` | as `30 02`, but first byte `01` | Mood state |

No other answer, and no other change of state, between `00` and `7F`.

The unknown codes from `00` to `3B` were then tried again with 1 to 6 `00` bytes as parameter: the answers are strictly identical to those without a parameter, and no other code answers. `01` without data (empty clock setting) is only acknowledged (`F1`, no data).

Still on 12/10/2026, in the dark with the lights off, each unknown code from `00` to `3F` (and the `display` field of the settings) received a `0A` byte, then `64`, then `FF`, 8 s apart, while watching the display: no visible effect. `05`, `06` and `11` always return the same value, whatever the parameter: they are reads. The `display` field accepts and keeps a value outside 0–2 (read back as is), with no visible effect on the display.

**No known command reads back the AML clock, or sets the display brightness other than with L / H / A.**

On 26/11/2026, with the service running and `Raw`, more probes found nothing new; the state was read back after each one:

- **Selector variants of the known reads**: alarm read `03` answers only for indexes `81` and `87` (tried `80` to `8F`); light read `30` only for targets `01` and `02` (tried `00` to `05`); the preset list `04` only looks at its first byte (`01` reads, `00` writes, anything else gets no answer); `07`, `12` and `17` ignore their data.
- **Codes `80` to `FF` without data**: no answer, no change of state. With the probes of 12/10/2026, all 256 codes have now been tried without data.
- **Write attempts** following the `04` pattern (direction byte `00`, then a full state): `20`, `21`, `22`, `23` only read; `30 00 …` gets no answer; `05`, `06` and `11` with a value keep returning `01`, `00`, `01`.

## Display and buttons

Observed on 26/11/2026:

- **The device buttons are ignored while a Bluetooth connection is open** ✔. Disconnected, ▲ / ▼ change the volume, Mode turns the radio off and 🔔 beeps; connected, nothing happens. While the SweetDawn service runs, it keeps the connection open, so the buttons are inactive.
- **The display brightness setting (`display`) has no visible effect** — *to be confirmed*. Every value tried, `00` to `09`, `0A`, `10`, `20`, `40`, `64`, `7F`, `80`, `C8` and `FF`, is accepted and read back, and keeps across a power cut, but the display did not change, whether the value was written while running or present at power-on. A phone flashlight did not change it either, so no ambient light sensor seemed active. The test device is old and its display is faint; it once went completely dark and only came back after unplugging the device: **this unit may be defective**, and another AML005 may behave differently.
- That evening the display went completely dark and came back after unplugging the device. To find a cause, every probe of the evening was replayed while watching the display (display setting values, the write attempts, codes `80` to `FF`, the reads): none of them changed it. The cause is unknown; an intermittent fault of this unit is the most likely.
- **The clock setting `01` accepts impossible times** ✔ and shows them as they are: minutes `50` (80) display `01:80`. Handy to show a number on the device during a probe (set `h:min` to it before each request); SweetDawn's own `Settings/Set time` refuses such values, so use `Raw` (`01 00 <min> <h> <weekday> <day> <month> <year-2000> 02`), then `Settings/Sync time`.

## Spontaneous messages

Sent by the AML without a request, once the handshake is done. SweetDawn turns them into events for its clients (`src/app/handlers.c`, [events](https://github.com/PicselVaot/SweetDawn/wiki/IPC-protocol#events) in the wiki):

| Code | Data | Event |
|---|---|---|
| `C6` | alarm number | sunrise starts ✔ |
| `C7` | alarm number | the alarm rings ✔ |
| `CA` | — | the lighting changed: read it again with `30` ✔ |
| `EB` | ? | Light timer ended |
| `EC` | ? | Mood timer ended |
| `ED` | ? | radio timer ended |
| `EE` | ? | music timer ended |

Observed on the device on 12/10/2026:

- `C6 02` at 19:42:00 and `C7 02` at 19:44:00 for alarm 2 set at 19:44 with a 2-min sunrise: the data is the alarm number (1 or 2), not its protocol index.
- `CA` arrives on every lighting change made on the device, including by the **gesture sensor** (a movement in front of the AML changes the light), often in bursts. The original app answers by reading the lamp again.
- Tapping the AML while it rings triggers a **snooze**, but **sends no message**: the ringing stops and the sunrise starts again from zero for the snooze duration. Only the state read `25` reveals it.
- Alarm state (`25` → `C5`, two bytes) ✔:
  1. number of the alarm that **holds the lamp** (sunrise, then ringing; 0 = none);
  2. number of the alarm that **rings** (0 = none).

  During the snooze: `02 00`; after `Alarm/Stop`: `00 00`. While alarm 1 rings: `01 01`.
- During the sunrise, the **lamp brightness** (read `30 01`) rises from 0 to the alarm brightness: 4, 14, 24… 100 in 2 minutes ✔. The lamp stays "off" (`enable` = 0).
- **Overlapping alarms** (alarm 1 at 20:47, alarm 2 at 20:48, 2-min sunrises) ✔:
  - only one sunrise at a time: the one of alarm 2, which should have started during that of alarm 1, is ignored (no `C6 02`);
  - when alarm 2 rings (`C7 02`), it takes over: the state goes from `01 01` to `01 02`, and alarm 1 does not ring again (still nothing 7 minutes later, for a 5-min snooze);
  - an alarm can ring for more than 6 minutes without stopping on its own.
- Stop (`29` → `C9`): answers `01` when an alarm was active ✔.
