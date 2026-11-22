# bubble-screen

A tiny desk display that shows what your [Paperclip](https://github.com/paperclipai/paperclip) AI agent is doing,
as a friendly animated pixel character. It runs on a $15 ESP32 "Cheap Yellow Display".

- **States you can read across the room.** The character rests, thinks, works (building a little picture of the job:
  a calendar, list, cart or envelope), hops and waves when the agent needs you, and gets flustered when it's stuck.
  A strip along the bottom shows the agent's name, the state, how long it's been, and the current task.
- **Helpers.** When other agents in the company work for it, the character tosses them a work order off the side of
  the screen and that edge fizzes while they're busy. When they finish, they pop in and whisper the result in its
  ear. Helpers are sized by model: an Opus helper is twice the size of a Sonnet one.
- **Near-realtime.** Paperclip's live event feed nudges the service, and the display long-polls, so changes show up
  in about a second.
- **Tickle it.** Rub or tap the character to tickle it, hold still to pet it, or touch anywhere else and its eyes
  follow your finger.
- **Demo mode.** With no service configured, it runs a demo starring **BUBBLES** and a couple of BUBBLES helpers.

## Hardware

An **ESP32-2432S028R** ("CYD": 2.8" 240×320 ILI9341, XPT2046 touch, CH340 USB), standing upright.

## Firmware

```bash
pio run -t upload            # plugged into this machine
pio device monitor           # 115200 baud
```

`scripts/flash-remote.sh <ssh-host>` builds here and flashes a board plugged into another machine.

Configure it over serial; every setting is saved on the board:

```
set ssid <wifi name>
set pass <wifi password>
set host <service ip>
set port 8000
set psk <DEVICE_PSK>
live
```

| Command | Effect |
|---|---|
| `live` / `demo` | Choose the source (saved). With no service configured, it runs the demo. |
| `sleep` `think` `work` `ask` `error` | Pin a state, for testing. |
| `helpers` | Play the helper demo once. |
| `invert` | Flip panel colour inversion (some CYD panels need it). |
| `flip` | Rotate 180°. |
| `skew <s>` | Jump the clock ahead (tests the clock rebase). |

## Service

The `service/` folder holds a small FastAPI app that reads one agent from Paperclip, using that agent's own API key
and only ever GETs, and serves the display. See [`service/deploy/README.md`](service/deploy/README.md).

```bash
cd service && python -m venv .venv && .venv/bin/pip install -r requirements.txt -r requirements-dev.txt
.venv/bin/python -m pytest -q
```

## How it works

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) covers the data flow, the state rules, and the ESP32 lessons learned.

## License

Copyright (C) 2026 Nick Allevato <nick.allevato@gmail.com>

bubble-screen is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License, version 3, as published by the Free Software Foundation. It comes with ABSOLUTELY NO WARRANTY. See
[LICENSE](LICENSE) for the full text.
