# Lab day checklist: touchscreen + Seed3

Everything on the touchscreen side is ready and tested against a simulated Seed. This list
takes you from "both boards on the bench" to "the touchscreen controls the real Seed", with what
you should see at each step. If a step's result differs, stop there: the notes say what to check.

## Before you go

- [ ] Touchscreen firmware built from `feature/seed-link` and flashed (`idf.py -p COMx flash`).
- [ ] Seed firmware with the receiver integrated (see [SEED_INTEGRATION_GUIDE.md](SEED_INTEGRATION_GUIDE.md)),
      built and flashed by the Seed owner. Until then, steps 1 to 4 still work and step 5 shows
      "Not connected", which is the correct, honest result.
- [ ] A 3-wire jumper cable (or a 4-wire one with pin 4 cut/removed), two USB-C cables, the
      microSD card with your `.nam` files in `/nam`.
- [ ] On the PC: ESP-IDF 5.5.5, and this repo.

## 1. Cable (power off)

| ESP32 board header H3 "UART2" | Seed carrier J4 "DISPLAY_UART" |
|---|---|
| pin 2 GND | pin 1 GND |
| pin 3 RX | pin 2 (Seed TX, D13) |
| pin 4 TX | pin 3 (Seed RX, D14) |
| pin 1 3V3: **leave empty** | pin 4 +5V: **leave empty** |

> **Never a straight 4-wire cable: it joins the carrier's +5 V to the ESP32's 3.3 V rail.**

## 2. Switch and ports

- [ ] ESP32 board switch **SW1 on UART2** (routes the link to H3). On UART1 the link goes to the
      USB TO UART port instead (that's the simulated-Seed setup).
- [ ] USB cable in the ESP32 board's **"USB"** Type-C port (native USB): this is the console, the
      `DIAG` line, the preview's Web Serial, and flashing. It shows up as "USB Serial Device (COMx)".
- [ ] The "USB TO UART" port isn't needed for the Seed link (with SW1 on UART2 it isn't connected
      to anything useful).
- [ ] Seed powered from its carrier (J3 5V_IN) or its own USB, as usual.

## 3. Boot the touchscreen and read its console

```
python tools/capture_log.py --port COMx --wait 15 --seconds 20 --reset --out logs/lab_boot.txt
```

Expect, in order:
- `seed_link: started on UART0 1000000 baud, TX 43 / RX 44`
- `knobs: I2C scan (GPIO8/9): ... 0x24 IO expander, 0x5D GT911 touch` (+ encoders if fitted)
- `sd: mounted /sdcard` and `nam_store: N profile(s) ready`
- One `DIAG {...}` line per second.

If the screen stays dark and the console prints nothing after a reset, press the board's RESET
button. (Known cause: a PC opening the "USB" port within about a second of a reset, e.g.
`idf.py flash monitor` or a Web Serial reconnect. `capture_log.py` waits 1.5 s to avoid it.)

## 4. The Seed's own USB console

The Seed still prints its 1 Hz status line on its USB serial (`make monitor`). Note its DSP load
before connecting anything (about 55 % with A2-Lite).

## 5. Link up

Within a second of both running, the touchscreen console should show:
- `seed_link: Seed HELLO: boot xxxxxxxx, fw "...", 48000 Hz / 48, 3 built-in models` (**not** "MOCK Seed")
- `seed_link: snapshot #1 sent (reason 1, 5 frames)`

On the screen: Menu (Presets drawer) > Test Mode > the SEED3 panel says **Connected** (green dot,
no amber banner), with the Seed's firmware version, 48000 Hz / 48, DSP load, and model name.

If it stays "Not connected":
- Swap the two signal wires (TX/RX crossed is the usual fault); check SW1 is on UART2.
- Check both sides use 1,000,000 baud.
- The touchscreen's `DIAG` line shows `"seed":{"tx":N,"rx":M,"err":E}`: `rx` growing with `err` also
  growing means wrong baud or noise; `rx` stuck at 0 means no bytes arrive (wiring, switch).

## 6. Things to check, in order of importance

| Check | How | Expect |
|---|---|---|
| Mute | Tap MUTE on the touchscreen while playing | Silence at once; un-mute restores the same level |
| Master volume | Drag MASTER | Output level follows smoothly (no zipper noise) |
| Amp models | AMP chip, tap each built-in card | Seed switches amps (its console prints `>>> ... loaded`); the touchscreen's status line says "Running on the Seed" |
| Ping | Test Mode > Ping SEED | Round-trip time in ms (expect ~1 ms) |
| Meters | Play the guitar | IN/OUT meters on the main screen move; Test Mode shows real dBFS values, no "MOCK" tag |
| SD model | AMP > SD Library > pick a profile | "Uploading to the Seed: N%" then "Running on the Seed"; the Seed's console shows the new model |
| Seed reboot | Reset the Seed | Touchscreen logs a new Seed HELLO, re-sends everything, re-uploads the SD model if one is selected |
| Link loss | Pull the cable for 5 s, plug back | "link lost" then "link back"; the Seed keeps sounding the same while unplugged |
| Presets | Load a preset | One snapshot (`reason 2`); Seed follows |

## 7. Measure and record

- [ ] Touchscreen: 2 minutes of `DIAG` while dragging sliders:
      `python tools/capture_log.py --port COMx --seconds 120 --out logs/lab_drag.txt` (compare with
      the baseline in SEED_LINK_PLAN.md: frame ~35 ms, heap_min ~49 KB, no lvgl_stuck / rgb_restarts).
- [ ] Seed: DSP load and overruns from its console with the link active vs. without (the receiver
      runs in the main loop, so audio load should be unchanged).
- [ ] Link errors: `"seed":{"err":...,"ovr":...}` should stay at or near 0.
- [ ] Write the results into SEED_LINK_STATUS.md (move items from "mock only" to "hardware").

## 8. Going back to the simulated Seed

SW1 to UART1, then on the PC: `python tools/mock_seed.py --port COMy` (the USB TO UART port), or
the Seed's own receiver code: `build/host/seed_sim.exe COMy 60`. Run `tools/link_check.py
reboot|faults|drag` for the scripted checks.
