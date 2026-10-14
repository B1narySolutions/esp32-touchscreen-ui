# Seed link status

What is verified where, as of 2026-10-13. "Hardware" means the real touchscreen board. The real
Daisy Seed3 ran the link on 2026-10-13 with a test build (its firmware plus `seed3-link.patch`); the
Seed repo itself doesn't have the receiver yet. Two simulated Seeds stand in for it on the PC:
`tools/mock_seed.py` (Python, with fault injection) and `tests/host/seed_sim.c` (the Seed's own
drop-in receiver code, `slp_receiver.c`).

## Verified with the real Seed (2026-10-13)

Bare Seed wired to H3 (D13, D14, DGND), SW1 on UART2, Seed running `realtime-nam-seed3` c3abe82 plus
`seed3-link.patch`, both consoles read with `tools/lab_day.py`.

| Item | Result |
|---|---|
| Handshake | Seed HELLO `fw "nam-a2", 48000 Hz / 48, 3 built-in models` (not MOCK), link up within about 1 s |
| Link health | 0 decode errors and 0 UART overruns over the whole session, including touchscreen resets, a Seed reset and cable pulls |
| Snapshots | All confirmed; with one wire off (touchscreen to Seed) the missing confirmations triggered resyncs every second, as designed |
| Amp models | 5 of 5 card taps made the Seed print `>>> ... loaded` |
| Mute | The Seed's OUT meter went to silence (-120 dBFS) while muted and back to its noise floor (about -91 dBFS) |
| Telemetry | DSP load 57.2 % on the touchscreen, 57 % on the Seed's own console; meters show the real noise floor and react to touching the input pins |
| Ping | 1.95 ms round trip |
| Recovery | Seed reset: new HELLO and full resend at once. Cable pull: "link lost" / "link back" on both boards |
| Seed audio | 0 overruns; DSP 57 % with the link vs 56 % without (about 3 us per block) |
| libDaisy lock-up | Without the patch's error-interrupt fix, the touchscreen's boot message locked the Seed's main loop (no console, no USB). Fixed in the patch and verified |

## Verified on the touchscreen hardware

| Item | Result |
|---|---|
| Console move to native USB | Works (COM "USB Serial Device", flashing and `DIAG` over it). Root cause found and fixed: firmware drove EXIO5 (USB_SEL) high = CAN, so the port never enumerated. |
| Display stability with the link running | No `lvgl_stuck`, no RGB restarts in any session after the observer fix; drag frames 32-36 ms (baseline 35.3 ms). |
| Internal RAM | heap_min about 49 KB (baseline 58.7 KB). The IRAM UART ISR (required for RX during PSRAM contention) costs ~5 KB; everything else new lives in PSRAM. |
| rig_state | Every UI action routed through it; one log line per change burst; UI behaviour unchanged. |
| Saved rig migration | v1 rig from older firmware migrated and restored on the board (Plexi/Mesa to JCM800 G5). |
| Touch latency with the link and knobs running | 165.4 ms average tap-to-frame over 25 effect-box taps (baseline 164.6 ms over 23: +0.5 %, limit 10 %); worst 200 ms (baseline 393 ms). |
| Physical pot on GPIO6 | Master volume follows over the full range; median filter rejects contact dropouts; idle pot causes no changes. |
| I2C scan at boot | 0x24 IO expander, 0x5D GT911 (no encoders fitted yet). |
| microSD | FAT32/MBR card mounts; no card and wrong format are reported, never formatted. |
| `.nam` conversion on the ESP32 | CRC32 of the three amps.json models on the device equals the Python reference: Fender 0066a5ae, Vox 3949448f, Marshall f737e066. Bad files rejected with clear reasons (wrong sample rate, A1 model, not JSON). Cache plus scan index: a boot with an unchanged card scans 6 files in 250 ms. |
| SD Library UI | Lists accepted and rejected files at full width; selecting uploads and shows progress then "Running on the Seed". |
| macOS build and flash (2026-10-13) | ESP-IDF 5.5.5 on a Mac, flashed over `/dev/cu.usbmodem*`. No Seed: `seed` conn false, rx 0, err 0; heap_min 49.0 KB; Test Mode "Not connected". Opening the native port resets the board (`rst` "usb"); it booted cleanly every time. |
| Seed without STATUS (2026-10-13) | Against a PC stand-in that sends HELLO but no STATUS: Test Mode shows "- (no STATUS from the Seed yet)" instead of 0.0 %, DIAG `stv` false. On a Seed reboot the previous boot's DSP load, model and counters are cleared until its first STATUS. 0 link errors, 0 UART overruns. |

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
| macOS, 2026-10-13 | `seed_sim` (now also builds on macOS/Linux) against the touchscreen for 21 min: 252 of 252 snapshots confirmed, no link drops, 2 decode errors and 1 UART overrun only at the moment the port opened (none after), heap_min flat at 49.0 KB, no display stalls. Amp bypass from the touchscreen arrived as SELECT_MODEL bypass; SD profile f737e066 uploaded and CRC-verified by the receiver. Reset counters followed by a Seed reboot now gives the new boot's counts (was a wrap to ~4.29e9). |
| The Seed's receiver code against the real ESP32 | `seed_sim` (slp_receiver.c on the PC): link up in 3 ms, 132 of 133 snapshots confirmed (the last was in flight), mute both ways, coalesced slider drags, ping 1.6-1.9 ms, SD profile upload received with CRC check and selected (`3949448f`, weights held). 0 link errors, 0 UART overruns. |

## Host-tested only

- Protocol library on macOS too (2026-10-13, clang ASan/UBSan, `tools/run_host_tests.sh`): 51,022 checks, 0 failures; `tools/test_slp_py.py` 17 of 17.
- Protocol library: 51,022 checks (COBS/CRC vectors, every message, 2 M-byte decoder fuzz, coalescer), MSVC `/W4 /WX` + AddressSanitizer.
- `.nam` converter: byte-identical to `convert_a2.py` on the three real models; 47-case differential test agrees on every accept/reject (including rounding ties, `True == 1`, duplicate keys, BOM, overflow).
- Seed receiver (`slp_receiver.c`): handshake, hooks, snapshot confirmation, out-of-order and repeated chunks, CRC failure, range and size limits, link loss with state kept, noise.
- Python protocol (`tools/slp.py`) matches the C encoder byte for byte on all 17 examples.
- Seed integration patch ([seed3-link.patch](seed3-link.patch), 2026-10-13): compiles and links into
  `realtime-nam-seed3` `230eb74` against libDaisy `cc146d5` (Arm GNU 15.3.rel2), no new warnings, flash
  103,428 B to 111,636 B of 128 KB. Not run on a Seed.

## Not tested yet

- **With the real Seed, still open:** listening to mute and MASTER with headphones (only the output
  meter was checked), meters while playing a guitar, SD profile upload (the Seed side isn't written:
  phase 2), the carrier's J4 cable (tested on a bare Seed), and the Seed repo integrating the patch.
- **STEMMA QT encoders**: driver written (seesaw registers per Adafruit's library), untested;
  rotation direction and the 1 ms read delay need checking on hardware. Touch latency with
  knobs fitted not measured (the pot doesn't use I2C).
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
  recovers it. 80 MHz was tried and **rejected**: the screen rippled under heavy redraws (and
  the frames measured averaged 118 ms against ~35 ms at 120 MHz). The 120 MHz setting stays; the
  rule is "don't open the native USB port within ~1.5 s of a reset".
- A rescan while an upload is running can fail that upload once (it retries after 5 s).
- Chip taps rebuild the whole effect panel (90-390 ms tap-to-frame, same as before this work).
