"""Seed link protocol v1 in Python: framing, CRCs and every message.

Mirrors firmware/main/seed_link_proto/seed_link_proto.c (the C version is the reference; the
cross-check in tools/test_slp_py.py decodes the C encoder's output and re-encodes it byte for
byte). Used by tools/mock_seed.py.
"""

import struct
import zlib

VERSION = 1
MAX_PAYLOAD = 256
FLAG_ACK_REQ = 0x01

HELLO, HEARTBEAT, PING, PONG, ACK, LOG = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
SET_PARAMS, SET_FX_STATE, SET_CHAIN, SET_MASTER, SELECT_MODEL = 0x10, 0x11, 0x12, 0x13, 0x14
SNAPSHOT_BEGIN, SNAPSHOT_END = 0x15, 0x16
STATUS, METERS = 0x20, 0x21
MODEL_BEGIN, MODEL_CHUNK, MODEL_COMMIT, MODEL_RESULT, MODEL_ABORT = 0x30, 0x31, 0x32, 0x33, 0x34

TYPE_NAMES = {v: k for k, v in dict(
    HELLO=HELLO, HEARTBEAT=HEARTBEAT, PING=PING, PONG=PONG, ACK=ACK, LOG=LOG, SET_PARAMS=SET_PARAMS,
    SET_FX_STATE=SET_FX_STATE, SET_CHAIN=SET_CHAIN, SET_MASTER=SET_MASTER, SELECT_MODEL=SELECT_MODEL,
    SNAPSHOT_BEGIN=SNAPSHOT_BEGIN, SNAPSHOT_END=SNAPSHOT_END, STATUS=STATUS, METERS=METERS,
    MODEL_BEGIN=MODEL_BEGIN, MODEL_CHUNK=MODEL_CHUNK, MODEL_COMMIT=MODEL_COMMIT,
    MODEL_RESULT=MODEL_RESULT, MODEL_ABORT=MODEL_ABORT).items()}

ROLE_UI, ROLE_SEED, ROLE_MOCK_SEED = 1, 2, 3
OK, ERR_STATE, ERR_CRC, ERR_RANGE, ERR_NO_MEMORY, ERR_BUSY, ERR_REJECTED, ERR_TIMEOUT = range(8)
MODEL_BUILTIN, MODEL_UPLOADED, MODEL_BYPASS = 1, 2, 3
STATUS_BYPASS, STATUS_MODEL_FAILED, STATUS_AUDIO_RUNNING = 0x01, 0x02, 0x04
SNAP_REASONS = {1: "hello", 2: "preset", 3: "boot", 4: "periodic", 5: "resync"}
FX_NAMES = {1: "gate", 2: "comp", 3: "eq", 4: "drive", 5: "amp", 6: "octave", 7: "chorus",
            8: "phaser", 9: "tremolo", 10: "delay", 11: "reverb", 12: "cab"}
NAME_LEN, FW_LEN, MODEL_NAME_LEN, CHUNK_MAX, MAX_PARAMS = 24, 16, 32, 192, 63
METER_SILENCE = -12000


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc32(data: bytes, crc: int = 0) -> int:
    return zlib.crc32(data, crc) & 0xFFFFFFFF


def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_at, code = 0, 1
    for i, b in enumerate(data):
        if b == 0:
            out[code_at] = code
            code_at, code = len(out), 1
            out.append(0)
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_at] = code
                if i + 1 == len(data):
                    return bytes(out)
                code_at, code = len(out), 1
                out.append(0)
    out[code_at] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0 or i + code - 1 > len(data):
            raise ValueError("bad COBS")
        block = data[i:i + code - 1]
        if 0 in block:
            raise ValueError("bad COBS")
        out += block
        i += code - 1
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def frame_encode(ftype: int, seq: int, payload: bytes = b"", flags: int = 0) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long")
    body = struct.pack("<BBBBH", VERSION, ftype, seq & 0xFF, flags, len(payload)) + payload
    return b"\x00" + cobs_encode(body + struct.pack("<H", crc16(body))) + b"\x00"


class Frame:
    __slots__ = ("type", "seq", "flags", "payload")

    def __init__(self, ftype, seq, flags, payload):
        self.type, self.seq, self.flags, self.payload = ftype, seq, flags, payload

    def __repr__(self):
        return f"<{TYPE_NAMES.get(self.type, hex(self.type))} seq={self.seq} len={len(self.payload)}>"


class Decoder:
    """Byte-at-a-time stream decoder with the same error counters as the C one."""

    def __init__(self):
        self.buf = bytearray()
        self.overflow = False
        self.frames_ok = 0
        self.errors = dict(cobs=0, short=0, version=0, crc=0, overflow=0)

    def error_count(self):
        return sum(self.errors.values())

    def feed(self, data: bytes):
        """Yields every complete valid Frame in data."""
        for b in data:
            if b != 0:
                if len(self.buf) < 6 + MAX_PAYLOAD + 2 + 3:
                    self.buf.append(b)
                else:
                    self.overflow = True
                continue
            raw, overflow = bytes(self.buf), self.overflow
            self.buf.clear()
            self.overflow = False
            if not raw:
                continue
            if overflow:
                self.errors["overflow"] += 1
                continue
            try:
                frame = cobs_decode(raw)
            except ValueError:
                self.errors["cobs"] += 1
                continue
            if len(frame) < 8 or struct.unpack_from("<H", frame, 4)[0] + 8 != len(frame):
                self.errors["short"] += 1
                continue
            if crc16(frame[:-2]) != struct.unpack_from("<H", frame, len(frame) - 2)[0]:
                self.errors["crc"] += 1
                continue
            if frame[0] != VERSION:
                self.errors["version"] += 1
                continue
            self.frames_ok += 1
            yield Frame(frame[1], frame[2], frame[3], frame[6:-2])


def _text(s: str, width: int) -> bytes:
    return s.encode("utf-8")[:width].ljust(width, b"\x00")


def _untext(b: bytes) -> str:
    return b.split(b"\x00", 1)[0].decode("utf-8", errors="replace")


# --- messages: pack_* returns the payload, unpack_* returns a dict (raises ValueError) ---

def pack_hello(role, boot_id, fw, sample_rate=0, block=0, builtins=()):
    p = struct.pack("<BBI", VERSION, role, boot_id) + _text(fw, FW_LEN) + struct.pack("<IHB", sample_rate, block, len(builtins))
    for bid, name in builtins:
        p += struct.pack("<B", bid) + _text(name, NAME_LEN)
    return p


def unpack_hello(p):
    if len(p) < 29 or len(p) != 29 + p[28] * 25 or not 1 <= p[1] <= 3:
        raise ValueError("bad HELLO")
    ver, role, boot = struct.unpack_from("<BBI", p)
    sr, blk, n = struct.unpack_from("<IHB", p, 22)
    builtins = [(p[29 + i * 25], _untext(p[30 + i * 25:54 + i * 25])) for i in range(n)]
    return dict(proto=ver, role=role, boot_id=boot, fw=_untext(p[6:22]), sample_rate=sr, block=blk, builtins=builtins)


def pack_heartbeat(uptime_ms, rx_frames, rx_errors):
    return struct.pack("<III", uptime_ms & 0xFFFFFFFF, rx_frames & 0xFFFFFFFF, rx_errors & 0xFFFFFFFF)


def unpack_heartbeat(p):
    if len(p) != 12:
        raise ValueError("bad HEARTBEAT")
    up, rx, err = struct.unpack("<III", p)
    return dict(uptime_ms=up, rx_frames=rx, rx_errors=err)


def pack_ping(token, t_us):
    return struct.pack("<II", token & 0xFFFFFFFF, t_us & 0xFFFFFFFF)


def unpack_ping(p):
    if len(p) != 8:
        raise ValueError("bad PING")
    token, t = struct.unpack("<II", p)
    return dict(token=token, t_send_us=t)


def pack_ack(acked_type, acked_seq, status):
    return struct.pack("<BBB", acked_type, acked_seq, status)


def unpack_ack(p):
    if len(p) != 3:
        raise ValueError("bad ACK")
    return dict(acked_type=p[0], acked_seq=p[1], status=p[2])


def pack_set_params(params):
    return struct.pack("<B", len(params)) + b"".join(struct.pack("<Hh", i, v) for i, v in params)


def unpack_set_params(p):
    if len(p) < 1 or not 1 <= p[0] <= MAX_PARAMS or len(p) != 1 + 4 * p[0]:
        raise ValueError("bad SET_PARAMS")
    return [struct.unpack_from("<Hh", p, 1 + 4 * i) for i in range(p[0])]


def pack_set_fx_state(entries):
    return struct.pack("<B", len(entries)) + b"".join(struct.pack("<BBB", fx, 1 if on else 0, m) for fx, on, m in entries)


def unpack_set_fx_state(p):
    if len(p) < 1 or not 1 <= p[0] <= 12 or len(p) != 1 + 3 * p[0]:
        raise ValueError("bad SET_FX_STATE")
    out = []
    for i in range(p[0]):
        fx, on, m = p[1 + 3 * i:4 + 3 * i]
        if not 1 <= fx <= 12 or on > 1:
            raise ValueError("bad SET_FX_STATE entry")
        out.append((fx, bool(on), m))
    return out


def pack_set_chain(fx_ids):
    return struct.pack("<B", len(fx_ids)) + bytes(fx_ids)


def unpack_set_chain(p):
    if len(p) < 1 or len(p) != 1 + p[0] or not 1 <= p[0] <= 11:
        raise ValueError("bad SET_CHAIN")
    ids = list(p[1:])
    if any(not 1 <= f <= 11 for f in ids) or len(set(ids)) != len(ids):
        raise ValueError("bad SET_CHAIN entry")
    return ids


def pack_set_master(volume, muted):
    return struct.pack("<BB", volume, 1 if muted else 0)


def unpack_set_master(p):
    if len(p) != 2 or p[0] > 100 or p[1] > 1:
        raise ValueError("bad SET_MASTER")
    return dict(volume=p[0], muted=bool(p[1]))


def pack_select_model(kind, builtin_id=0, model_hash=0):
    return struct.pack("<BBI", kind, builtin_id, model_hash)


def unpack_select_model(p):
    if len(p) != 6 or not 1 <= p[0] <= 3:
        raise ValueError("bad SELECT_MODEL")
    kind, bid, h = struct.unpack("<BBI", p)
    return dict(kind=kind, builtin_id=bid, hash=h)


def pack_snapshot(snapshot_id, reason_or_count):
    return struct.pack("<HB", snapshot_id, reason_or_count)


def unpack_snapshot(p):
    if len(p) != 3:
        raise ValueError("bad SNAPSHOT")
    sid, r = struct.unpack("<HB", p)
    return dict(snapshot_id=sid, value=r)


def pack_status(cpu_avg_x10, cpu_peak_x10, overruns, model_kind, builtin_id, model_hash, flags, applied_snapshot):
    return struct.pack("<HHIBBIBH", cpu_avg_x10, cpu_peak_x10, overruns, model_kind, builtin_id, model_hash, flags, applied_snapshot)


def unpack_status(p):
    if len(p) != 17:
        raise ValueError("bad STATUS")
    keys = ("cpu_avg_x10", "cpu_peak_x10", "overruns", "model_kind", "builtin_id", "model_hash", "flags", "applied_snapshot_id")
    return dict(zip(keys, struct.unpack("<HHIBBIBH", p)))


def pack_meters(in_cdb, out_cdb, clips):
    return struct.pack("<hhI", in_cdb, out_cdb, clips)


def unpack_meters(p):
    if len(p) != 8:
        raise ValueError("bad METERS")
    i, o, c = struct.unpack("<hhI", p)
    return dict(in_peak_cdb=i, out_peak_cdb=o, clip_count=c)


def pack_model_begin(transfer_id, weight_count, crc, name):
    return struct.pack("<BHII", transfer_id, weight_count, weight_count * 4, crc) + _text(name, MODEL_NAME_LEN)


def unpack_model_begin(p):
    if len(p) != 11 + MODEL_NAME_LEN:
        raise ValueError("bad MODEL_BEGIN")
    tid, wc, bc, crc = struct.unpack_from("<BHII", p)
    if bc != wc * 4:
        raise ValueError("bad MODEL_BEGIN size")
    return dict(transfer_id=tid, weight_count=wc, byte_count=bc, crc32=crc, name=_untext(p[11:]))


def pack_model_chunk(transfer_id, offset, data):
    return struct.pack("<BI", transfer_id, offset) + data


def unpack_model_chunk(p):
    if not 6 <= len(p) <= 5 + CHUNK_MAX:
        raise ValueError("bad MODEL_CHUNK")
    tid, off = struct.unpack_from("<BI", p)
    return dict(transfer_id=tid, offset=off, data=bytes(p[5:]))


def pack_model_end(transfer_id, status=0, crc=0):
    return struct.pack("<BBI", transfer_id, status, crc)


def unpack_model_end(p):
    if len(p) != 6:
        raise ValueError("bad MODEL_END")
    tid, st, crc = struct.unpack("<BBI", p)
    return dict(transfer_id=tid, status=st, crc32=crc)


UNPACKERS = {
    HELLO: unpack_hello, HEARTBEAT: unpack_heartbeat, PING: unpack_ping, PONG: unpack_ping, ACK: unpack_ack,
    SET_PARAMS: unpack_set_params, SET_FX_STATE: unpack_set_fx_state, SET_CHAIN: unpack_set_chain,
    SET_MASTER: unpack_set_master, SELECT_MODEL: unpack_select_model, SNAPSHOT_BEGIN: unpack_snapshot,
    SNAPSHOT_END: unpack_snapshot, STATUS: unpack_status, METERS: unpack_meters, MODEL_BEGIN: unpack_model_begin,
    MODEL_CHUNK: unpack_model_chunk, MODEL_COMMIT: unpack_model_end, MODEL_RESULT: unpack_model_end,
    MODEL_ABORT: unpack_model_end, LOG: lambda p: p.decode("utf-8", errors="replace"),
}
