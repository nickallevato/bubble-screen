# How bubble-screen works

```
display ──HTTP long-poll, LAN, device PSK──▶ bubble-screen service ──HTTPS, agent key, GET only──▶ Paperclip API
                                                     ▲
                                                     └──── Paperclip live events (WebSocket, receive-only)
```

## The service (`service/`)

- **`paperclip.py`: reading Paperclip.** `gather()` builds a snapshot from a handful of GETs:
  - the agent's recent runs
  - the live run's detail and log tail
  - the agent's open issues and their open questions
  - pending approvals
  - the company's agents and recent runs, for helpers
  - the comment a finished helper run posted

  `ReadOnlyClient` refuses any method but GET before anything leaves the process.
- **`status.py`: one status.** Reduces the snapshot to a single state, in priority order:
  1. **ask:** an open question or approval newer than 48 h
  2. **error:** the last run failed in the last 30 min, with no newer success
  3. **work:** a run is live and called a tool in the last 15 s
  4. **think:** a run is live otherwise
  5. **sleep:** nothing is happening

  It also produces the title, a picture (calendar, list, cart or mail, from keywords in the title), a detail line, and how many things are waiting on you. Text is folded to the display's 5x7 uppercase ASCII font.
- **`helpers.py`: the other agents.** Every other agent in the company is a "helper": `working` while a run is running, then `done`/`failed` for 120 s after it ends. A finished helper whispers the first line of the comment its run posted, or the issue title. Helpers carry their model (Haiku, Sonnet, Opus) so the display can size them.
- **`live.py`: near-realtime.** Listens on Paperclip's `/companies/{cid}/events/ws`. Any event triggers a re-read 0.3 s later. A poll (3 s while anything runs, 30 s otherwise) is the fallback.
- **`main.py`: serving it.** `GET /api/now` returns the status with a version `v`. With `?v=N&wait=S`, the request is held until the status changes, for up to 25 s. `ok` turns false when the data is more than 90 s old.

## The firmware (`src/`)

- **No UI framework.**
  - A hand-written ILI9341 driver sends 16-row strips over DMA SPI at 80 MHz. Each strip is expanded from the 8-bit palette while the previous one is in flight.
  - Frames are drawn with plain C into a 240x320 8-bit framebuffer: a dithered sky, a lit and shimmering character, motes, and 5x7 text.
- **A fixed 30 fps.** Drawing takes about 13 ms and sending about 16 ms. Even frame steps read as smooth motion; a varying rate reads as jitter.
- **Network on the other core.** A task on core 0 long-polls the service and parses the JSON into a struct behind a spinlock. The renderer only reads that struct, so the network can never stall a frame.
- **Helpers.**
  - A new working helper makes the character toss a work order off that helper's side, and that edge fizzes with a short band of streaks.
  - A finished helper pops in, whispers its result in a typed-out bubble, and the character beams.
  - Helper size follows the model: Sonnet 1.1x, Opus 2.2x, Haiku 0.8x.
- **Touch.** The XPT2046 is bit-banged on its own pins. Rubbing or tapping tickles, holding still pets, and touching elsewhere draws the eyes and motes toward your finger.

## Lessons from the ESP32 (keep these)

- **Memory:** Wi-Fi plus two 75 KB framebuffers don't fit in the ESP32's fragmented heap; the largest free block is about 34 KB once Wi-Fi is up. So there's one framebuffer, allocated in 16-row strips, and the Wi-Fi driver is started before the framebuffer is allocated.
- **Trig:** newlib's `sinf` falls onto a slow path for |x| > ~201. Animation phases like `t*1.3 + 40*k` cross that a couple of minutes after boot, and render time jumped from 13 to 21 ms. Angles are now range-reduced first (`wsin`/`wcos`).
- **Time:** `micros()` wraps every 71.6 minutes, and a float second count loses precision after a day or two. The clock is the 64-bit `esp_timer`, rebased every ~5.5 h together with every stored timestamp. Serial `skew` tests this.
- **Panel quirks:** colour inversion differs between individual CYD panels, so it's a saved setting (serial `invert`), not a build flag.
