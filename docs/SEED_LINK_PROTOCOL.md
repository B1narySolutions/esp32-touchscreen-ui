# Seed link protocol, version 1

How the ESP32-S3 touchscreen (the "UI") and the Daisy Seed3 running NAM (the "Seed") talk.
The reference implementation is `firmware/main/seed_link_proto/` (pure C99, no allocation,
no OS); the Seed firmware can compile those files unchanged. `tools/slp.py` is the same protocol
in Python, used by the mock Seed (`tools/mock_seed.py`). Every hex example below was printed by
`tests/host/examples.c` from the real encoder (`tools/run_host_tests.ps1 -Examples`).

Design rule: the ESP32 does the heavy lifting (JSON, validation, repacking, CRCs, coalescing,
retries). The Seed only receives small fixed-format frames that it parses in its main loop
without allocating.

## 1. Physical layer

| | |
|---|---|
| Interface | UART, 8N1, no flow control, 3.3 V logic |
| Baud | 1,000,000 (configurable on the ESP32 in menuconfig; both ends must match) |
| ESP32 side | UART0, GPIO43 TX / GPIO44 RX, on header H3 "UART2" (SW1 set to UART2) |
| Seed side | USART1, D13 TX (PB6) / D14 RX (PB7), carrier header J4 "DISPLAY_UART" |
| Byte order | Little-endian everywhere |

Cable: ESP32 H3 pin 2 (GND) to J4 pin 1 (GND), H3 pin 3 (ESP RX) to J4 pin 2 (Seed TX), H3 pin 4
(ESP TX) to J4 pin 3 (Seed RX). **J4 pin 4 is +5 V and H3 pin 1 is the ESP32's 3.3 V rail: never
connect them.** A straight 4-wire cable is wrong.

## 2. Framing

Each frame on the wire is `0x00, COBS(frame), 0x00`. COBS ([Consistent Overhead Byte
Stuffing](https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing)) guarantees the
encoded data never contains 0x00, so 0x00 always marks a frame boundary. The leading 0x00
terminates any junk that preceded the frame (for example the text the ESP32's ROM prints on
UART0 at every reset), so junk never costs a good frame. Empty frames (two 0x00 in a row) are
ignored.

Decoded frame:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `ver` | 1. Frames with another version are counted and dropped. |
| 1 | 1 | `type` | Message type, section 4. |
| 2 | 1 | `seq` | Sender's frame counter, +1 per frame, wraps at 255. Used to match ACKs. |
| 3 | 1 | `flags` | Bit 0 `ACK_REQ`: receiver must answer with ACK. Other bits 0. |
| 4 | 2 | `len` | Payload length, 0 to 256. |
| 6 | len | `payload` | |
| 6+len | 2 | `crc16` | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR) over bytes 0 to 5+len. Check value of "123456789" = 0x29B1. |

Maximum encoded frame: 264 decoded bytes, 268 on the wire with both delimiters.

A receiver drops, and counts as a link error, any frame that fails COBS decoding, is shorter
than 8 bytes, has a `len` that disagrees with the frame size, fails the CRC, has the wrong
version, or is longer than the maximum. It then waits for the next 0x00. Unknown `type`
values with a valid CRC are ignored (forward compatibility).

## 3. Link behaviour

**Handshake.** Each side sends HELLO every 500 ms until it hears a HELLO from the peer. After
that it answers a peer HELLO with its own HELLO only when the peer's `boot_id` is new (first
contact, or the peer rebooted). This settles in at most two HELLOs each way, with no ping-pong.

**Reboot detection.** `boot_id` is random per boot. A HELLO with a different `boot_id` means
the peer rebooted and lost its state.

**Heartbeat.** Each side sends HEARTBEAT once per second. A side that has received no valid
frame for 3 s declares the link lost (the ESP32 then shows "Not connected" and empties the
meters) and goes back to sending HELLO every 500 ms.

**State, ESP32 to Seed.** All state messages are idempotent and unacknowledged: the latest
value wins, and a lost frame is repaired by the next change or the next snapshot.

**Snapshots.** A snapshot is the complete state: `SNAPSHOT_BEGIN`, then `SET_CHAIN`,
`SET_FX_STATE`, `SELECT_MODEL`, `SET_MASTER` and as many `SET_PARAMS` as needed for every
knob, then `SNAPSHOT_END` with the number of state frames sent in between. The ESP32 sends one:
- when it receives a HELLO from a Seed it isn't in sync with: a new `boot_id`, or while the link was down (reason 1),
- after a preset load (reason 2) or a saved rig restored at boot (reason 3), exactly once,
- every 5 s as background self-healing (reason 4),
- when the link comes back after a loss (reason 5).

The Seed should apply the frames as they arrive (each is valid on its own), and use
`SNAPSHOT_END` only to confirm completeness: if the count matches, report the snapshot id in
STATUS `applied_snapshot_id`. If the ESP32 sees that id lagging behind its latest snapshot for
more than 0.7 s, a frame was lost and it sends the snapshot again (reason 5, at most once per
second), so a lost frame is repaired within about a second instead of at the next 5 s
snapshot. A Seed that never reports an applied id (always 0) simply gets the 5 s self-healing.

**Rate control.** The ESP32 coalesces parameter changes and sends at most one `SET_PARAMS`
every 10 ms, containing only the parameters whose value changed since the last frame. A full
slider drag is therefore at most 100 small frames per second, never one per touch sample. The
first change after a quiet period is sent at once.

**Mute is never delayed.** A mute or un-mute is sent immediately as `SET_MASTER`, bypassing
coalescing. Master volume and mute are in every snapshot.

**On link loss the Seed keeps its last state.** It must not jump to defaults, mute, or change
the model. When the ESP32 returns it sends a snapshot.

**Parsing on the Seed.** Receive with DMA into a ring buffer (libDaisy
`UartHandler::DmaListenStart` with an idle-line callback), feed bytes to the decoder in the
main loop, never in the audio callback. Changing the amp model must stop audio the way
`ApplyPendingAmpCommand` does today.

## 4. Messages

Type values, direction, and payload size:

| Type | Name | Direction | Payload bytes |
|---|---|---|---|
| 0x01 | HELLO | both | 29 + 25 per built-in model |
| 0x02 | HEARTBEAT | both | 12 |
| 0x03 | PING | both | 8 |
| 0x04 | PONG | both | 8 |
| 0x05 | ACK | both | 3 |
| 0x06 | LOG | both | 1 to 256, UTF-8 text, not terminated |
| 0x10 | SET_PARAMS | UI to Seed | 1 + 4 per parameter (1 to 63 parameters) |
| 0x11 | SET_FX_STATE | UI to Seed | 1 + 3 per effect (1 to 12) |
| 0x12 | SET_CHAIN | UI to Seed | 1 + 1 per effect (1 to 11) |
| 0x13 | SET_MASTER | UI to Seed | 2 |
| 0x14 | SELECT_MODEL | UI to Seed | 6 |
| 0x15 | SNAPSHOT_BEGIN | UI to Seed | 3 |
| 0x16 | SNAPSHOT_END | UI to Seed | 3 |
| 0x20 | STATUS | Seed to UI | 17 |
| 0x21 | METERS | Seed to UI | 8 |
| 0x30 | MODEL_BEGIN | UI to Seed | 43, ACK_REQ |
| 0x31 | MODEL_CHUNK | UI to Seed | 6 to 197, ACK_REQ |
| 0x32 | MODEL_COMMIT | UI to Seed | 6 |
| 0x33 | MODEL_RESULT | Seed to UI | 6 |
| 0x34 | MODEL_ABORT | UI to Seed | 6 |

Text fields are fixed width, NUL-padded, and not NUL-terminated when the text fills the field.

### HELLO (0x01)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | protocol version (1) |
| 1 | 1 | role: 1 = UI (ESP32), 2 = Seed, 3 = mock Seed (`tools/mock_seed.py`; everything it reports is simulated) |
| 2 | 4 | boot_id, random per boot |
| 6 | 16 | firmware version, text |
| 22 | 4 | sample rate in Hz (0 from the UI) |
| 26 | 2 | audio block size in samples (0 from the UI) |
| 28 | 1 | n = number of built-in models (0 to 8; 0 from the UI) |
| 29 | 25 x n | per model: id (1 byte, the Seed's `AmpId`), name (24 bytes, text) |

The Seed lists its built-in amps in `AmpId` order. The ESP32 shows exactly these names in its
AMP picker, so the list on screen always matches what the Seed can run.

### HEARTBEAT (0x02)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | uptime in ms |
| 4 | 4 | valid frames this side has received since boot |
| 8 | 4 | frames this side has dropped since boot |

### PING (0x03) and PONG (0x04)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | token |
| 4 | 4 | sender's timestamp in microseconds |

The receiver of a PING answers with a PONG carrying the identical payload. The ESP32 uses this
for the round-trip time in Test Mode ("Ping SEED").

### ACK (0x05)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | type of the frame being acknowledged |
| 1 | 1 | seq of the frame being acknowledged |
| 2 | 1 | status: 0 OK, 1 wrong state, 2 CRC mismatch, 3 out of range, 4 no memory, 5 busy, 6 rejected, 7 timeout |

Sent only for frames with `ACK_REQ` set (the MODEL_BEGIN and MODEL_CHUNK frames).

### SET_PARAMS (0x10)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | n, 1 to 63 |
| 1 | 4 x n | per parameter: id (u16), value (i16) |

Values are in UI units: every knob is 0 to 100. The ids and their meanings are in section 5;
map them by meaning, not by position.

### SET_FX_STATE (0x11)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | n, 1 to 12 |
| 1 | 3 x n | per effect: effect id (section 5), on (1) or bypassed (0), model index |

The model index is the UI's choice for that effect (for example drive 0 = Tube Screamer); the
names are in `firmware/main/ui/ui_effects_data.c`. For the AMP the authoritative model is
`SELECT_MODEL`. CAB is always on.

### SET_CHAIN (0x12)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | n, 1 to 11 |
| 1 | n | effect ids in processing order, each at most once; CAB (12) never appears and is always last |

Effects not in the chain are not in the signal path at all (different from bypassed).

### SET_MASTER (0x13)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | volume 0 to 100 |
| 1 | 1 | muted: 1 = hard mute, 0 = not muted |

Muting keeps the volume value, so un-muting restores the same level.

### SELECT_MODEL (0x14)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | kind: 1 = built-in, 2 = uploaded, 3 = amp bypass |
| 1 | 1 | built-in id (`AmpId`) when kind = 1, else 0 |
| 2 | 4 | CRC32 of the uploaded weights when kind = 2, else 0 |

If the Seed doesn't hold an uploaded model with that CRC32 (it rebooted), it keeps the current
model and sets `MODEL_FAILED` in STATUS; the ESP32 then uploads it again and re-selects it.

### SNAPSHOT_BEGIN (0x15) and SNAPSHOT_END (0x16)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | snapshot id (increments per snapshot) |
| 2 | 1 | BEGIN: reason (1 peer HELLO, 2 preset, 3 boot, 4 periodic, 5 link back). END: number of state frames between BEGIN and END |

### STATUS (0x20), about 2 Hz

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | DSP load average, 0.1 % of the audio block budget |
| 2 | 2 | DSP load peak, same unit |
| 4 | 4 | audio callback overruns since boot |
| 8 | 1 | active model kind (as SELECT_MODEL) |
| 9 | 1 | active built-in id |
| 10 | 4 | active uploaded model CRC32 |
| 14 | 1 | flags: bit 0 bypass, bit 1 model failed to load, bit 2 audio running |
| 15 | 2 | last snapshot id applied completely |

### METERS (0x21), about 30 Hz

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | input peak since the previous METERS, centi-dBFS (i16; -600 = -6.00 dBFS; -12000 = silence) |
| 2 | 2 | output peak, same unit |
| 4 | 4 | output clip count since boot |

These drive the IN/OUT meters on the main screen. The ESP32 shows nothing (empty meters) when
the link is down; it never invents levels.

### Model upload: MODEL_BEGIN (0x30), MODEL_CHUNK (0x31), MODEL_COMMIT (0x32), MODEL_RESULT (0x33), MODEL_ABORT (0x34)

MODEL_BEGIN, with ACK_REQ:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | transfer id |
| 1 | 2 | weight count (1871 for A2-Lite) |
| 3 | 4 | byte count = weight count x 4 |
| 7 | 4 | CRC32 (IEEE, as zlib `crc32`) of the packed weights |
| 11 | 32 | model name, text |

MODEL_CHUNK, with ACK_REQ:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | transfer id |
| 1 | 4 | byte offset |
| 5 | 1 to 192 | data |

MODEL_COMMIT, MODEL_RESULT, MODEL_ABORT:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | transfer id |
| 1 | 1 | status (MODEL_RESULT and MODEL_ABORT; 0 in COMMIT), codes as ACK |
| 2 | 4 | MODEL_RESULT: CRC32 the Seed computed over what it received; else 0 |

The weights are little-endian float32 in the engine's packed order (exactly what
`realtime-nam-seed3/scripts/convert_a2.py` `pack()` produces: input projection, then per layer
the conv taps as [tap][in][out] followed by the layer tail, then the head taps as [tap][in],
head bias, head scale). The ESP32 converts and validates `.nam` files, so the Seed never parses
JSON.

Sequence:
1. UI sends MODEL_BEGIN; the Seed allocates (or reuses) a 7484-byte RAM buffer for it and ACKs.
2. UI sends MODEL_CHUNKs with at most 4 un-ACKed at a time. Each chunk is ACKed. A chunk not
   ACKed within 150 ms is sent again (with a new seq; chunks are idempotent by offset). After 5
   failed tries of one chunk the UI sends MODEL_ABORT and reports the failure.
3. UI sends MODEL_COMMIT. The Seed checks the CRC32 and answers MODEL_RESULT (status 0 = stored,
   2 = CRC mismatch). Storing does not switch models.
4. UI sends SELECT_MODEL kind 2 with the CRC32. The Seed stops audio, builds the model on its
   RAM copy (the engine keeps pointers to the weights, so the copy must stay alive while the
   model is in use), restarts audio, and reports the result in STATUS.

A 7484-byte model is 39 chunks; at 1 Mbaud the transfer takes well under a second.

## 5. Parameter ids

Ids never change once assigned; a removed knob's id is retired, not reused. The registry is
`tools/param_ids.json`; `tools/gen_params.py` checks it against the UI at every firmware build
and regenerates the table below, `firmware/main/rig/param_map_gen.h` (for the ESP32) and
`firmware/main/seed_link_proto/slp_params.h` (named constants for the Seed).

<!-- BEGIN PARAM TABLE (tools/gen_params.py) -->

Effect wire ids: gate = 1, comp = 2, eq = 3, drive = 4, amp = 5, octave = 6, chorus = 7, phaser = 8, tremolo = 9, delay = 10, reverb = 11, cab = 12.

| Id | Key | Range | Meaning | On the Seed |
|---|---|---|---|---|
| `0x0001` | `master.volume` | 0 to 100 | Master output volume. Sent as SET_MASTER; listed here for completeness. | not yet implemented |
| `0x0002` | `master.mute` | 0 or 1 | Hard mute of the output (1 = muted). Sent as SET_MASTER. | not yet implemented |
| `0x0100` | `gate.thresh` | 0 to 100 | Noise gate threshold: below this level the gate closes. | not yet implemented |
| `0x0101` | `gate.attack` | 0 to 100 | Noise gate attack: how fast it opens. | not yet implemented |
| `0x0102` | `gate.release` | 0 to 100 | Noise gate release: how fast it closes. | not yet implemented |
| `0x0103` | `gate.range` | 0 to 100 | Noise gate range: how much it attenuates when closed. | not yet implemented |
| `0x0200` | `comp.thresh` | 0 to 100 | Compressor threshold. | not yet implemented |
| `0x0201` | `comp.ratio` | 0 to 100 | Compressor ratio. | not yet implemented |
| `0x0202` | `comp.attack` | 0 to 100 | Compressor attack time. | not yet implemented |
| `0x0203` | `comp.release` | 0 to 100 | Compressor release time. | not yet implemented |
| `0x0204` | `comp.gain` | 0 to 100 | Compressor make-up gain. | not yet implemented |
| `0x0300` | `eq.low` | 0 to 100 | EQ low band cut/boost (50 = flat). | not yet implemented |
| `0x0301` | `eq.mid` | 0 to 100 | EQ mid band cut/boost (50 = flat). | not yet implemented |
| `0x0302` | `eq.high` | 0 to 100 | EQ high band cut/boost (50 = flat). | not yet implemented |
| `0x0303` | `eq.level` | 0 to 100 | EQ output level. | not yet implemented |
| `0x0400` | `drive.gain` | 0 to 100 | Drive pedal gain (amount of clipping). | not yet implemented |
| `0x0401` | `drive.tone` | 0 to 100 | Drive pedal tone (dark to bright). | not yet implemented |
| `0x0402` | `drive.level` | 0 to 100 | Drive pedal output level. | not yet implemented |
| `0x0500` | `amp.gain` | 0 to 100 | Amp input gain into the NAM model (drive). | not yet implemented |
| `0x0501` | `amp.bass` | 0 to 100 | Amp tone stack bass (50 = flat). | not yet implemented |
| `0x0502` | `amp.mid` | 0 to 100 | Amp tone stack middle (50 = flat). | not yet implemented |
| `0x0503` | `amp.treble` | 0 to 100 | Amp tone stack treble (50 = flat). | not yet implemented |
| `0x0504` | `amp.level` | 0 to 100 | Amp output level after the model. | not yet implemented |
| `0x0600` | `octave.mix` | 0 to 100 | Octave effect wet/dry mix. | not yet implemented |
| `0x0601` | `octave.octUp` | 0 to 100 | Level of the octave-up voice. | not yet implemented |
| `0x0602` | `octave.octDown` | 0 to 100 | Level of the octave-down voice. | not yet implemented |
| `0x0700` | `chorus.rate` | 0 to 100 | Chorus LFO rate. | not yet implemented |
| `0x0701` | `chorus.depth` | 0 to 100 | Chorus modulation depth. | not yet implemented |
| `0x0702` | `chorus.mix` | 0 to 100 | Chorus wet/dry mix. | not yet implemented |
| `0x0800` | `phaser.rate` | 0 to 100 | Phaser LFO rate. | not yet implemented |
| `0x0801` | `phaser.depth` | 0 to 100 | Phaser sweep depth. | not yet implemented |
| `0x0802` | `phaser.mix` | 0 to 100 | Phaser wet/dry mix. | not yet implemented |
| `0x0900` | `tremolo.rate` | 0 to 100 | Tremolo rate. | not yet implemented |
| `0x0901` | `tremolo.depth` | 0 to 100 | Tremolo depth. | not yet implemented |
| `0x0A00` | `delay.time` | 0 to 100 | Delay time. | not yet implemented |
| `0x0A01` | `delay.feedback` | 0 to 100 | Delay feedback (number of repeats). | not yet implemented |
| `0x0A02` | `delay.tone` | 0 to 100 | Delay repeat tone (dark to bright). | not yet implemented |
| `0x0A03` | `delay.mix` | 0 to 100 | Delay wet/dry mix. | not yet implemented |
| `0x0B00` | `reverb.decay` | 0 to 100 | Reverb decay time. | not yet implemented |
| `0x0B01` | `reverb.predelay` | 0 to 100 | Reverb pre-delay. | not yet implemented |
| `0x0B02` | `reverb.tone` | 0 to 100 | Reverb tone (dark to bright). | not yet implemented |
| `0x0B03` | `reverb.mix` | 0 to 100 | Reverb wet/dry mix. | not yet implemented |
| `0x0C00` | `cab.lowcut` | 0 to 100 | Cabinet low cut (high-pass) frequency. | not yet implemented |
| `0x0C01` | `cab.highcut` | 0 to 100 | Cabinet high cut (low-pass) frequency. | not yet implemented |
| `0x0C02` | `cab.level` | 0 to 100 | Cabinet output level. | not yet implemented |

<!-- END PARAM TABLE -->

## 6. Examples (real encoder output)

Each block shows the payload and the full wire bytes including both 0x00 delimiters.

#### HELLO from the Seed (seq 0): role 2, boot_id 0x1A2B3C4D, 48000 Hz, 48-sample blocks, 3 built-in amps
```
payload 01 02 4D 3C 2B 1A 6E 61 6D 2D 61 32 2D 31 2E 30 00 00 00 00 00 00 80 BB 00 00 30 00 03 01 46 65 6E 64 65 72 20 54 77 69 6E 36 35 00 00 00 00 00 00 00 00 00 00 00 02 56 6F 78 20 41 43 33 30 20 43 68 69 6D 65 79 00 00 00 00 00 00 00 00 00 03 4D 61 72 73 68 61 6C 6C 20 4A 43 4D 38 30 30 20 47 35 00 00 00 00 00 00
wire    00 03 01 01 01 02 68 11 01 02 4D 3C 2B 1A 6E 61 6D 2D 61 32 2D 31 2E 30 01 01 01 01 01 03 80 BB 01 02 30 10 03 01 46 65 6E 64 65 72 20 54 77 69 6E 36 35 01 01 01 01 01 01 01 01 01 01 11 02 56 6F 78 20 41 43 33 30 20 43 68 69 6D 65 79 01 01 01 01 01 01 01 01 14 03 4D 61 72 73 68 61 6C 6C 20 4A 43 4D 38 30 30 20 47 35 01 01 01 01 01 03 BB EF 00
```

#### HELLO from the ESP32 (seq 0): role 1, no built-ins
```
payload 01 01 EE FF C0 00 66 62 64 35 64 39 39 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
wire    00 03 01 01 01 02 1D 06 01 01 EE FF C0 08 66 62 64 35 64 39 39 01 01 01 01 01 01 01 01 01 01 01 01 01 01 01 03 B4 13 00
```

#### HEARTBEAT (seq 17): uptime 61 s, 1234 frames received, 2 dropped
```
payload 48 EE 00 00 D2 04 00 00 02 00 00 00
wire    00 04 01 02 11 02 0C 03 48 EE 01 03 D2 04 01 02 02 01 01 03 C4 6E 00
```

#### PING (seq 18): token 42, t_send_us 123456789 (PONG echoes the same payload)
```
payload 2A 00 00 00 15 CD 5B 07
wire    00 04 01 03 12 02 08 02 2A 01 01 07 15 CD 5B 07 AD ED 00
```

#### SET_PARAMS (seq 19): amp.gain = 58, amp.treble = 66
```
payload 02 00 05 3A 00 03 05 42 00
wire    00 04 01 10 13 02 09 02 02 03 05 3A 04 03 05 42 03 D2 88 00
```

#### SET_FX_STATE (seq 20): drive on (model 0), amp on (model 2), reverb bypassed
```
payload 03 04 01 00 05 01 02 0B 00 00
wire    00 04 01 11 14 02 0A 04 03 04 01 05 05 01 02 0B 01 03 72 9A 00
```

#### SET_CHAIN (seq 21): gate, comp, drive, amp, chorus, delay, reverb (then CAB)
```
payload 07 01 02 04 05 07 0A 0B
wire    00 04 01 12 15 02 08 0B 07 01 02 04 05 07 0A 0B 89 27 00
```

#### SET_MASTER (seq 22): volume 63, muted
```
payload 3F 01
wire    00 04 01 13 16 02 02 05 3F 01 93 E6 00
```

#### SELECT_MODEL (seq 23): built-in amp 3 (Marshall JCM800 G5)
```
payload 01 03 00 00 00 00
wire    00 04 01 14 17 02 06 03 01 03 01 01 01 03 23 20 00
```

#### SNAPSHOT_BEGIN (seq 24): snapshot 5, reason 1 (peer HELLO)
```
payload 05 00 01
wire    00 04 01 15 18 02 03 02 05 04 01 8E 37 00
```

#### SNAPSHOT_END (seq 31): snapshot 5, 6 state frames sent in between
```
payload 05 00 06
wire    00 04 01 16 1F 02 03 02 05 04 06 58 96 00
```

#### STATUS (seq 40): DSP 55.3 % avg / 56.2 % peak, 0 overruns, built-in amp 1, audio running, snapshot 5 applied
```
payload 29 02 32 02 00 00 00 00 01 01 00 00 00 00 04 05 00
wire    00 04 01 20 28 02 11 05 29 02 32 02 01 01 01 03 01 01 01 01 01 03 04 05 03 A6 E9 00
```

#### METERS (seq 41): input -18.3 dBFS, output -6.2 dBFS, 0 clips
```
payload DA F8 94 FD 00 00 00 00
wire    00 04 01 21 29 02 08 05 DA F8 94 FD 01 01 01 03 B3 4B 00
```

#### MODEL_BEGIN (seq 50, ack requested): transfer 1, 1871 weights, 7484 bytes, CRC32 0x9C3A5E21
```
payload 01 4F 07 3C 1D 00 00 21 5E 3A 9C 4D 79 20 54 4F 4E 45 33 30 30 30 20 41 6D 70 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
wire    00 06 01 30 32 01 2B 06 01 4F 07 3C 1D 01 14 21 5E 3A 9C 4D 79 20 54 4F 4E 45 33 30 30 30 20 41 6D 70 01 01 01 01 01 01 01 01 01 01 01 01 01 01 01 01 03 D3 B7 00
```

#### MODEL_CHUNK (seq 51, ack requested): transfer 1, offset 0, 8 bytes (real chunks carry up to 192)
```
payload 01 00 00 00 00 00 00 80 3F 00 00 00 BF
wire    00 06 01 31 33 01 0D 02 01 01 01 01 01 01 03 80 3F 01 01 04 BF 05 9D 00
```

#### ACK from the Seed (seq 60): MODEL_CHUNK seq 51 OK
```
payload 31 33 00
wire    00 04 01 05 3C 02 03 03 31 33 03 88 FC 00
```

#### MODEL_RESULT (seq 61): transfer 1 OK, Seed computed CRC32 0x9C3A5E21
```
payload 01 00 21 5E 3A 9C
wire    00 04 01 33 3D 02 06 02 01 07 21 5E 3A 9C A4 4C 00
```
