#!/usr/bin/env python3
"""Mock Daisy Seed3: the Seed side of the link protocol, on a PC serial port.

Stands in for the Seed until its firmware speaks the protocol. With the ESP32's UART selection
switch (SW1) on UART1, the ESP32's Seed link goes to the "USB TO UART" Type-C port, so this
script talks to the real touchscreen firmware with no extra hardware.

What it does (docs/SEED_LINK_PROTOCOL.md):
  * HELLO as role MOCK_SEED (3) with the Seed's real built-in amp names, HEARTBEAT at 1 Hz,
    PONG, STATUS at 2 Hz and METERS at 30 Hz. STATUS and METERS values are SIMULATED; the role
    tells the ESP32 so it labels them MOCK.
  * Applies SET_PARAMS / SET_FX_STATE / SET_CHAIN / SET_MASTER / SELECT_MODEL and snapshots,
    and shows the resulting state in a live table.
  * Receives model uploads (MODEL_BEGIN/CHUNK/COMMIT), checks the CRC32, ACKs every chunk.
  * Fault injection: corrupt outgoing frames, drop received bytes, drop or delay ACKs, go silent,
    reboot with a new boot_id. Keys while running: r = reboot, c = corrupt next 20 frames,
    d = toggle dropping ACKs, s = silent for 5 s, q = quit.

    python tools/mock_seed.py --port COM7
    python tools/mock_seed.py --port COM7 --events logs/mock.jsonl --no-table   (for scripted tests)
"""

import argparse
import json
import math
import os
import random
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import slp  # noqa: E402

BUILTINS = [(1, "Fender Twin65"), (2, "Vox AC30 Chimey"), (3, "Marshall JCM800 G5")]  # models/amps.json order
A2_LITE_WEIGHTS = 1871
LINK_TIMEOUT_S = 3.0


def load_param_names():
    try:
        reg = json.loads((Path(__file__).resolve().parent / "param_ids.json").read_text(encoding="utf-8"))
        return {int(e["id"], 16): k for k, e in reg["params"].items()}
    except (OSError, ValueError, KeyError):
        return {}


class MockSeed:
    def __init__(self, ser, args):
        self.ser, self.args = ser, args
        self.rnd = random.Random()
        self.param_names = load_param_names()
        self.events = open(args.events, "a", encoding="utf-8") if args.events else None
        self.t0 = time.monotonic()
        self.reboot()

    # --- lifecycle ------------------------------------------------------------------------
    def reboot(self):
        self.boot_id = self.rnd.getrandbits(32)
        self.boot_t = time.monotonic()
        self.seq = 0
        self.dec = slp.Decoder()
        self.peer = None             # dict from the ESP32's HELLO
        self.peer_heard = False
        self.last_rx = 0.0
        self.tx_frames = 0
        self.next = dict(hello=0.0, heartbeat=0.0, status=0.0, meters=0.0)
        # Audio state as the Seed would hold it; a reboot loses everything.
        self.params, self.fx, self.chain = {}, {}, []
        self.master, self.muted = None, None
        self.model = None
        self.snap = None             # (id, reason, frames counted so far) while a snapshot is open
        self.applied_snapshot = 0
        self.snapshots = []          # (time, id, reason, frames, complete)
        self.param_frames = []       # arrival times of SET_PARAMS, to show coalescing
        self.uploads = {}            # hash -> name
        self.transfer = None
        self.clips = 0
        self.corrupt_left = 0
        self.drop_acks = self.args.drop_ack_rate >= 1.0
        self.silent_until = 0.0
        self.log("boot", boot_id=f"{self.boot_id:08x}")

    def log(self, _ev, **kw):
        if self.events:
            self.events.write(json.dumps({"t": round(time.monotonic() - self.t0, 4), "ev": _ev, **kw}) + "\n")
            self.events.flush()
        if self.args.no_table and _ev not in ("meters", "status"):
            print(f"{time.monotonic() - self.t0:9.3f} {_ev} {kw}")

    def uptime_ms(self):
        return int((time.monotonic() - self.boot_t) * 1000)

    # --- transmit -------------------------------------------------------------------------
    def send(self, ftype, payload=b"", flags=0):
        if time.monotonic() < self.silent_until:
            return
        wire = bytearray(slp.frame_encode(ftype, self.seq, payload, flags))
        self.seq = (self.seq + 1) & 0xFF
        if self.corrupt_left or self.rnd.random() < self.args.corrupt_rate:
            self.corrupt_left = max(0, self.corrupt_left - 1)
            wire[self.rnd.randrange(1, len(wire) - 1)] ^= 1 << self.rnd.randrange(8)
            if 0 in wire[1:-1]:  # a flipped bit may not create a delimiter mid-frame
                wire[wire.index(0, 1)] = 0x55
            self.log("tx_corrupted", type=slp.TYPE_NAMES.get(ftype))
        self.ser.write(wire)
        self.tx_frames += 1

    def send_hello(self):
        self.send(slp.HELLO, slp.pack_hello(slp.ROLE_MOCK_SEED, self.boot_id, "mock-seed-1", 48000, 48, BUILTINS))

    def ack(self, frame, status):
        if self.drop_acks or self.rnd.random() < self.args.drop_ack_rate:
            self.log("ack_dropped", seq=frame.seq)
            return
        if self.args.ack_delay_ms:
            time.sleep(self.args.ack_delay_ms / 1000.0)
        self.send(slp.ACK, slp.pack_ack(frame.type, frame.seq, status))

    # --- receive --------------------------------------------------------------------------
    def poll_rx(self):
        data = self.ser.read(self.ser.in_waiting or 1)
        if not data:
            return
        if self.args.drop_rx_rate:
            data = bytes(b for b in data if self.rnd.random() >= self.args.drop_rx_rate)
        if time.monotonic() < self.silent_until:
            return  # deaf as well as mute while "unplugged"
        for f in self.dec.feed(data):
            self.last_rx = time.monotonic()
            try:
                self.handle(f)
            except ValueError as e:
                self.log("bad_payload", type=slp.TYPE_NAMES.get(f.type, f.type), error=str(e))

    def handle(self, f):
        t = f.type
        if self.snap and t not in (slp.SNAPSHOT_END, slp.HEARTBEAT, slp.PING):
            self.snap[2] += 1
        if t == slp.HELLO:
            h = slp.unpack_hello(f.payload)
            new = self.peer is None or self.peer["boot_id"] != h["boot_id"]
            self.peer, self.peer_heard = h, True
            self.log("peer_hello", boot_id=f"{h['boot_id']:08x}", fw=h["fw"], new=new)
            if new:
                self.send_hello()  # answer each new peer boot once; no HELLO ping-pong
        elif t == slp.HEARTBEAT:
            pass
        elif t == slp.PING:
            self.send(slp.PONG, f.payload)
        elif t == slp.SET_PARAMS:
            for pid, val in slp.unpack_set_params(f.payload):
                self.params[pid] = val
            if self.snap is None:
                self.param_frames.append(time.monotonic())
            self.log("set_params", n=f.payload[0], in_snapshot=self.snap is not None,
                     params={self.param_names.get(i, hex(i)): v for i, v in slp.unpack_set_params(f.payload)})
        elif t == slp.SET_FX_STATE:
            for fx, on, m in slp.unpack_set_fx_state(f.payload):
                self.fx[fx] = (on, m)
            self.log("set_fx_state", n=f.payload[0])
        elif t == slp.SET_CHAIN:
            self.chain = slp.unpack_set_chain(f.payload)
            self.log("set_chain", chain=[slp.FX_NAMES[c] for c in self.chain])
        elif t == slp.SET_MASTER:
            m = slp.unpack_set_master(f.payload)
            self.master, self.muted = m["volume"], m["muted"]
            self.log("set_master", **m)
        elif t == slp.SELECT_MODEL:
            self.model = slp.unpack_select_model(f.payload)
            self.log("select_model", **self.model)
        elif t == slp.SNAPSHOT_BEGIN:
            s = slp.unpack_snapshot(f.payload)
            self.snap = [s["snapshot_id"], s["value"], 0]
        elif t == slp.SNAPSHOT_END:
            s = slp.unpack_snapshot(f.payload)
            ok = bool(self.snap) and self.snap[0] == s["snapshot_id"] and self.snap[2] == s["value"]
            reason = slp.SNAP_REASONS.get(self.snap[1], "?") if self.snap else "?"
            self.snapshots.append((time.monotonic(), s["snapshot_id"], reason, s["value"], ok))
            if ok:
                self.applied_snapshot = s["snapshot_id"]
            self.log("snapshot", id=s["snapshot_id"], reason=reason, frames=s["value"], complete=ok)
            self.snap = None
        elif t == slp.MODEL_BEGIN:
            b = slp.unpack_model_begin(f.payload)
            if b["weight_count"] != A2_LITE_WEIGHTS:
                self.ack(f, slp.ERR_REJECTED)
                return
            self.transfer = dict(b, buf=bytearray(b["byte_count"]), got=0, t=time.monotonic())
            self.log("model_begin", name=b["name"], bytes=b["byte_count"], crc=f"{b['crc32']:08x}")
            self.ack(f, slp.OK)
        elif t == slp.MODEL_CHUNK:
            c = slp.unpack_model_chunk(f.payload)
            tr = self.transfer
            if not tr or tr["transfer_id"] != c["transfer_id"]:
                self.ack(f, slp.ERR_STATE)
            elif c["offset"] + len(c["data"]) > tr["byte_count"]:
                self.ack(f, slp.ERR_RANGE)
            else:
                tr["buf"][c["offset"]:c["offset"] + len(c["data"])] = c["data"]
                tr["got"] = max(tr["got"], c["offset"] + len(c["data"]))
                self.ack(f, slp.OK)
        elif t in (slp.MODEL_COMMIT, slp.MODEL_ABORT):
            e = slp.unpack_model_end(f.payload)
            tr = self.transfer
            if t == slp.MODEL_ABORT or not tr or tr["transfer_id"] != e["transfer_id"]:
                self.log("model_abort", transfer=e["transfer_id"])
                self.transfer = None
                if t == slp.MODEL_COMMIT:
                    self.send(slp.MODEL_RESULT, slp.pack_model_end(e["transfer_id"], slp.ERR_STATE, 0))
                return
            crc = slp.crc32(bytes(tr["buf"]))
            status = slp.OK if crc == tr["crc32"] else slp.ERR_CRC
            if status == slp.OK:
                self.uploads[crc] = tr["name"]
            self.log("model_result", name=tr["name"], crc=f"{crc:08x}", expected=f"{tr['crc32']:08x}",
                     ok=status == slp.OK, seconds=round(time.monotonic() - tr["t"], 3))
            self.send(slp.MODEL_RESULT, slp.pack_model_end(e["transfer_id"], status, crc))
            self.transfer = None
        else:
            self.log("unhandled", type=t)

    # --- periodic -------------------------------------------------------------------------
    def tick(self):
        now = time.monotonic()
        if self.args.reboot_every and now - self.boot_t > self.args.reboot_every:
            self.reboot()
        if not self.peer_heard and now >= self.next["hello"]:
            self.send_hello()
            self.next["hello"] = now + 0.5
        if self.peer_heard and now - self.last_rx > LINK_TIMEOUT_S:
            self.peer_heard = False  # link lost: announce again until the ESP32 answers
            self.log("link_lost")
        if now >= self.next["heartbeat"]:
            self.send(slp.HEARTBEAT, slp.pack_heartbeat(self.uptime_ms(), self.dec.frames_ok, self.dec.error_count()))
            self.next["heartbeat"] = now + 1.0
        if now >= self.next["status"]:
            self.send(slp.STATUS, self.status_payload())
            self.next["status"] = now + 0.5
        if now >= self.next["meters"]:
            self.send(slp.METERS, self.meters_payload(now))
            self.next["meters"] = now + 1 / 30

    def status_payload(self):
        m = self.model or dict(kind=slp.MODEL_BUILTIN, builtin_id=1, hash=0)
        failed = m["kind"] == slp.MODEL_UPLOADED and m["hash"] not in self.uploads
        flags = slp.STATUS_AUDIO_RUNNING | (slp.STATUS_BYPASS if m["kind"] == slp.MODEL_BYPASS else 0) \
            | (slp.STATUS_MODEL_FAILED if failed else 0)
        # SIMULATED load: A2-Lite measures about 55 % on the real Seed (docs/performance.md there).
        avg = 553 + self.rnd.randrange(-5, 6) if m["kind"] != slp.MODEL_BYPASS else 3
        return slp.pack_status(avg, avg + 9, 0, m["kind"], m["builtin_id"], m["hash"], flags, self.applied_snapshot)

    def meters_payload(self, now):
        # SIMULATED signal: a slow swell between -40 and -12 dBFS, output follows master volume.
        in_db = -26 + 14 * math.sin(now * 0.7)
        if self.muted:
            out_cdb = slp.METER_SILENCE
        else:
            vol = self.master if self.master is not None else 63
            out_db = in_db + 6 + (20 * math.log10(max(vol, 1) / 100))
            out_cdb = int(min(out_db, 0) * 100)
        return slp.pack_meters(int(in_db * 100), out_cdb, self.clips)

    # --- display --------------------------------------------------------------------------
    def table(self):
        now = time.monotonic()
        rate = sum(1 for t in self.param_frames if now - t < 1.0)
        self.param_frames = [t for t in self.param_frames if now - t < 5.0]
        link = "CONNECTED" if self.peer_heard else "waiting for ESP32 HELLO"
        lines = [
            "MOCK SEED3  (simulated STATUS/METERS; not a real Seed)    keys: r reboot  c corrupt  d drop ACKs  s silent  q quit",
            f"port {self.ser.port} @ {self.ser.baudrate}   boot_id {self.boot_id:08x}   uptime {self.uptime_ms() / 1000:7.1f} s   link: {link}",
            f"ESP32: {('boot ' + format(self.peer['boot_id'], '08x') + ' fw ' + self.peer['fw']) if self.peer else '-'}",
            f"rx frames {self.dec.frames_ok}  rx errors {self.dec.errors}  tx frames {self.tx_frames}   SET_PARAMS frames in last 1 s: {rate}",
            f"faults: drop ACKs {'ON' if self.drop_acks else 'off'}  corrupt next {self.corrupt_left}  "
            f"{'SILENT' if now < self.silent_until else ''}",
            "",
            f"master {self.master if self.master is not None else '-'}   muted {self.muted if self.muted is not None else '-'}   "
            f"model {self.model_text()}   applied snapshot {self.applied_snapshot}",
            "chain: " + (" > ".join(slp.FX_NAMES[c] for c in self.chain) + " > cab" if self.chain else "-"),
            "",
        ]
        by_fx = {}
        for pid, val in sorted(self.params.items()):
            name = self.param_names.get(pid, hex(pid))
            fx, _, knob = name.partition(".")
            by_fx.setdefault(fx, []).append(f"{knob}={val}")
        for fx_id, fx_name in slp.FX_NAMES.items():
            on, model = self.fx.get(fx_id, (None, None))
            state = "-" if on is None else ("ON " if on else "byp")
            lines.append(f"  {fx_name:<8} {state} m{model if model is not None else '-'}  " + "  ".join(by_fx.get(fx_name, [])))
        lines.append("")
        lines.append("last snapshots: " + ", ".join(
            f"#{sid} {reason} {frames}f {'ok' if ok else 'INCOMPLETE'} ({now - t:.0f}s ago)" for t, sid, reason, frames, ok in self.snapshots[-4:]))
        if self.transfer:
            tr = self.transfer
            lines.append(f"upload: {tr['name']} {tr['got']}/{tr['byte_count']} bytes")
        if self.uploads:
            lines.append("uploaded: " + ", ".join(f"{n} ({h:08x})" for h, n in self.uploads.items()))
        return "\n".join(lines)

    def model_text(self):
        if not self.model:
            return "-"
        if self.model["kind"] == slp.MODEL_BUILTIN:
            return dict(BUILTINS).get(self.model["builtin_id"], f"builtin {self.model['builtin_id']}?")
        if self.model["kind"] == slp.MODEL_UPLOADED:
            return self.uploads.get(self.model["hash"], f"uploaded {self.model['hash']:08x} (NOT RECEIVED)")
        return "bypass"


def key_pressed():
    if os.name != "nt":
        return None
    import msvcrt
    return msvcrt.getwch() if msvcrt.kbhit() else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM7")
    ap.add_argument("--baud", type=int, default=1000000)
    ap.add_argument("--events", help="append JSON lines of events here (for scripted tests)")
    ap.add_argument("--no-table", action="store_true", help="print events instead of the live table")
    ap.add_argument("--seconds", type=float, default=0, help="exit after this long (0 = run until q)")
    ap.add_argument("--corrupt-rate", type=float, default=0.0, help="fraction of sent frames with a flipped bit")
    ap.add_argument("--drop-rx-rate", type=float, default=0.0, help="fraction of received bytes dropped")
    ap.add_argument("--drop-ack-rate", type=float, default=0.0, help="fraction of ACKs not sent")
    ap.add_argument("--ack-delay-ms", type=float, default=0.0)
    ap.add_argument("--reboot-every", type=float, default=0.0, help="simulate a Seed reboot every N seconds")
    args = ap.parse_args()

    import serial  # pyserial

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = args.port, args.baud, 0.002
    ser.dtr = ser.rts = False  # don't reset the ESP32 through its auto-reset circuit
    ser.open()
    mock = MockSeed(ser, args)
    if not args.no_table:
        os.system("")  # enables ANSI escapes in the Windows console
    end = time.monotonic() + args.seconds if args.seconds else None
    next_draw = 0.0
    try:
        while end is None or time.monotonic() < end:
            mock.poll_rx()
            mock.tick()
            k = key_pressed()
            if k == "q":
                break
            if k == "r":
                mock.reboot()
            elif k == "c":
                mock.corrupt_left = 20
            elif k == "d":
                mock.drop_acks = not mock.drop_acks
            elif k == "s":
                mock.silent_until = time.monotonic() + 5.0
                mock.log("silent", seconds=5)
            if not args.no_table and time.monotonic() >= next_draw:
                sys.stdout.write("\x1b[H\x1b[2J" + mock.table() + "\n")
                sys.stdout.flush()
                next_draw = time.monotonic() + 0.3
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        mock.log("exit", rx_frames=mock.dec.frames_ok, rx_errors=mock.dec.errors, tx_frames=mock.tx_frames)


if __name__ == "__main__":
    main()
