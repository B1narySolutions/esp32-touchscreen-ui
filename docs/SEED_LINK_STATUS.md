# Seed link status

What is verified where, as of 2026-10-02. "Hardware" means the real touchscreen board; the
real Daisy Seed3 has **not** been connected yet (its firmware needs the receiver first, see
SEED_INTEGRATION_GUIDE.md). Two simulated Seeds stand in for it on the PC:
`tools/mock_seed.py` (Python, with fault injection) and `tests/host/seed_sim.c` (the Seed's own
drop-in receiver code, `slp_receiver.c`).

## Verified on the touchscreen hardware

| Item | Result |
|---|---|
| Console move to native USB | Works (COM "USB Serial Device", flashing and `DIAG` over it). Root cause found and fixed: firmware drove EXIO5 (USB_SEL) high = CAN, so the port never enumerated. |
| Display stability with the link running | No `lvgl_stuck`, no RGB restarts in any session after the observer fix; drag frames 32-36 ms (baseline 35.3 ms). |
| Internal RAM | heap_min about 49 KB (baseline 58.7 KB). The IRAM UART ISR (required for RX during PSRAM contention) costs ~5 KB; everything else new lives in PSRAM. |
| rig_state | Every UI action routed through it; one log line per change burst; UI behaviour unchanged. |
| Saved rig migration | v1 rig from older firmware migrated and restored on the board (Plexi/Mesa to JCM800 G5). |
| Physical pot on GPIO6 | Master volume follows over the full range; median filter rejects contact dropouts; idle pot causes no changes. |
| I2C scan at boot | 0x24 IO expander, 0x5D GT911 (no encoders fitted yet). |
| microSD | FAT32/MBR card mounts; no card and wrong format are reported, never formatted. |
| `.nam` conversion on the ESP32 | CRC32 of the three amps.json models on the device equals the Python reference: Fender 0066a5ae, Vox 3949448f, Marshall f737e066. Bad files rejected with clear reasons (wrong sample rate, A1 model, not JSON). Cache plus scan index: a boot with an unchanged card scans 6 files in 250 ms. |
| SD Library UI | Lists accepted and rejected files at full width; selecting uploads and shows progress then "Running on the Seed". |

## Verified against the simulated Seed only (mock_seed.py / seed_sim)

| Item | Result |
|---|---|
| Handshake | ~5 ms to link up; one snapshot per handshake. |
| Snapshots | Confirmed complete by the mock; on HELLO, preset load (exactly one), every 5 s. |
| Seed reboot | New boot_id gets a complete snapshot within 6 ms; selected SD profile re-uploaded within 0.1 s. |
| Fault injection | 5 % corrupted frames + 0.1 % dropped bytes: link never dropped; every damaged snapshot repaired in 0.71-1.14 s (limit 5 s). |
| Coalescing | Slider drag: 25 SET_PARAMS frames/s peak (cap 100), never one per touch sample. |
| Mute | Sent at once as its own frame, both ways. |
| Ping | 0.79 ms round trip through the CH343 at 1 Mbaud. |
| Model upload | 7484 bytes in 83-86 ms, CRC verified by the receiver every time. |
| UART at 1 Mbaud with the display busy | 0 overruns during drags, fault tests and snapshots; 1 overrun seen once, at the moment the mock process reopened the port (not reproduced since). |
| 30-minute soak | PASS: 360 snapshots all complete, 0 decode errors both ways, 0 UART overruns, no link drops, no resets, no display stalls or RGB restarts, heap_min flat at 47.8 KB over 1,766 DIAG lines. |
| The Seed's receiver code against the real ESP32 | `seed_sim` (slp_receiver.c on the PC): link up in 3 ms, 132 of 133 snapshots confirmed (the last was in flight), mute both ways, coalesced slider drags, ping 1.6-1.9 ms, SD profile upload received with CRC check and selected (`3949448f`, weights held). 0 link errors, 0 UART overruns. |

## Host-tested only

- Protocol library: 51,022 checks (COBS/CRC vectors, every message, 2 M-byte decoder fuzz, coalescer), MSVC `/W4 /WX` + AddressSanitizer.
- `.nam` converter: byte-identical to `convert_a2.py` on the three real models; 47-case differential test agrees on every accept/reject (including rounding ties, `True == 1`, duplicate keys, BOM, overflow).
- Seed receiver (`slp_receiver.c`): handshake, hooks, snapshot confirmation, out-of-order and repeated chunks, CRC failure, range and size limits, link loss with state kept, noise.
- Python protocol (`tools/slp.py`) matches the C encoder byte for byte on all 17 examples.

## Not tested yet

- **The real Seed** (needs the receiver integrated; see LAB_DAY_CHECKLIST.md): electrical link
  over the H3 to J4 cable, real STATUS and METERS, model loading on the Seed, audio behaviour of
  mute/volume, Seed DSP load with the receiver in its main loop.
- **STEMMA QT encoders**: driver written (seesaw registers per Adafruit's library), untested;
  rotation direction and the 1 ms read delay need checking on hardware. Touch latency with
  knobs fitted not measured (the pot doesn't use I2C).
- Tap-to-frame latency comparison needs ~20 chip taps (only 6 so far).
- Web Serial preview reading `DIAG` over the native USB port (should work: it's a CDC port).
- SD card hot-swap: the card is scanned at boot and by "Rescan SD card"; there is no card-detect pin.

## Known limitations

- The SD scan trusts `/nam/.cache/index.tsv` for files whose name, size and modification time
  are unchanged (boot scan of 6 files: 250 ms, was 3.8 s). A file replaced by different content
  with the same size and timestamp would be missed until it is renamed or touched.
- **Startup hang if the native USB port is opened right after a reset** (measured with
  `tools/reset_test.py`). With flash and PSRAM at 120 MHz (the current, display-tested setting),
  a PC opening the "USB" port within about a second of a reset stops startup at "Enter psram
  timing tuning": 33 of 33 times, whatever the reset method. Opening it 1.5 s later: 0 of 40.
  With flash and PSRAM at 80 MHz: 0 of 10 even when opened immediately, so the 120 MHz timing
  tuning (experimental in ESP-IDF) is the sensitive part. The repo's tools now wait 1.5 s after
  the port reappears. Watch for it with `idf.py flash monitor` and Web Serial reconnects; RESET
  recovers it. Possible fix: 80 MHz PSRAM, but that costs PSRAM bandwidth, which the display
  depends on, so it needs a display soak with someone watching before adopting it.
- A rescan while an upload is running can fail that upload once (it retries after 5 s).
- Chip taps rebuild the whole effect panel (90-390 ms tap-to-frame, same as before this work).
