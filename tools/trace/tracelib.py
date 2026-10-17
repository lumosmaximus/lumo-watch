# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
"""Read a lumo-watch trace (tools/trace/capture.py) into Python objects.

File: "LWTR", u32 ticks-per-second, then 8-byte events:
    u32 timestamp (ticks), u8 event id, u8 a, u16 b     (little-endian)
Event ids and their arguments are in apps/watch/common/trace.h; the table
below mirrors it, so this file is the one place to update when it changes.

    import tracelib
    t = tracelib.load("docs/data/traces/gmt_idle.trace")
    for f in t.frames(): print(f.t0, f.work_ms, len(f.boxes))
"""

import struct
from dataclasses import dataclass, field

EVENTS = {
    1: "TE", 2: "FRAME_BEGIN", 3: "BOX_BEGIN", 4: "BOX_END", 5: "SPI_WAIT",
    7: "RING_BEGIN", 8: "RING_END", 9: "FRAME_END", 10: "MSG_RX",
    11: "KEY_TX", 12: "TEXT_RX", 13: "FACE",
    0x40: "TOUCH", 0x41: "BUTTON", 0x42: "HID_KEY", 0x43: "HOST_STATE", 0x44: "TEXT_LINE",
    0x45: "TIME_TX", 0x46: "MSG_TX", 0x47: "TRACE_ON",
}
CORE = {e: (1 if e < 0x40 else 0) for e in EVENTS}
FACES = {0: "GMT", 1: "chrono black", 2: "chrono white", 3: "remote"}
MSGS = {1: "time", 2: "stats", 3: "touch", 4: "bench", 5: "button", 6: "hello", 7: "key",
        8: "state", 9: "text", 10: "trace", 11: "trace ctl"}
KEYS = {1: "vol+", 2: "vol-", 3: "mute", 4: "play/pause", 5: "bright+", 6: "bright-",
        7: "next", 8: "prev"}


@dataclass
class Event:
    t: float          # seconds from the first event
    name: str
    a: int
    b: int
    core: int


@dataclass
class Box:
    t0: float
    t1: float         # composed, handed to the SPI
    band: int
    x0: int
    w: int = 0
    h: int = 0


@dataclass
class Frame:
    t_te: float | None  # TE edge that started it (None if free-running)
    t0: float           # work start
    t1: float           # work end
    face: str
    boxes: list = field(default_factory=list)
    spi_waits: list = field(default_factory=list)   # (t0, t1) CPU stalled for DMA
    ring: tuple | None = None                        # (t0, t1) bezel repaint
    work_ms: float = 0.0

    @property
    def stall_ms(self):
        return sum(b - a for a, b in self.spi_waits) * 1000


class Trace:
    def __init__(self, events, hz):
        self.events = events
        self.hz = hz

    def __len__(self):
        return len(self.events)

    @property
    def duration(self):
        return self.events[-1].t if self.events else 0.0

    def of(self, name):
        return [e for e in self.events if e.name == name]

    def frames(self):
        """Group core 1 events into frames."""
        frames, cur, box, te = [], None, None, None
        for e in self.events:
            if e.core != 1:
                continue
            if e.name == "TE":
                te = e.t
            elif e.name == "FRAME_BEGIN":
                cur = Frame(te, e.t, e.t, FACES.get(e.a, str(e.a)))
                te = None
            elif cur is None:
                continue
            elif e.name == "BOX_BEGIN":
                box = Box(e.t, e.t, e.a, e.b)
            elif e.name == "BOX_END" and box:
                box.t1, box.h, box.w = e.t, e.a, e.b
                cur.boxes.append(box)
                box = None
            elif e.name == "SPI_WAIT":  # b = ticks stalled, ending at e.t
                cur.spi_waits.append((e.t - e.b / self.hz, e.t))
            elif e.name == "RING_BEGIN":
                cur.ring = [e.t, e.t]
            elif e.name == "RING_END" and cur.ring:
                cur.ring[1] = e.t
            elif e.name == "FRAME_END":
                cur.t1 = e.t
                cur.work_ms = e.b / 100.0
                frames.append(cur)
                cur = None
        return frames

    def summary(self):
        fr = self.frames()
        if not fr:
            return "no frames"
        periods = [b.t0 - a.t0 for a, b in zip(fr, fr[1:])]
        work = [f.work_ms for f in fr]
        boxes = [len(f.boxes) for f in fr]
        stall = [f.stall_ms for f in fr]
        return (f"{len(self.events)} events over {self.duration:.2f} s, {len(fr)} frames, "
                f"period {1000 * sum(periods) / len(periods):.2f} ms "
                f"(min {1000 * min(periods):.2f}, max {1000 * max(periods):.2f}), "
                f"work {sum(work) / len(work):.2f} ms (max {max(work):.2f}), "
                f"boxes {sum(boxes) / len(boxes):.1f}/frame, "
                f"SPI stall {sum(stall) / len(stall):.2f} ms/frame, "
                f"touches {len(self.of('TOUCH'))}, keys {len(self.of('HID_KEY'))}")


def load(path):
    data = open(path, "rb").read()
    if data[:4] != b"LWTR":
        raise ValueError("not a lumo-watch trace")
    hz = struct.unpack_from("<I", data, 4)[0]
    raw = [struct.unpack_from("<IBBH", data, i) for i in range(8, len(data) - 7, 8)]
    # timestamps are 32-bit ticks and may wrap once in a capture
    t0, last, wraps, events = raw[0][0] if raw else 0, 0, 0, []
    for ts, ev, a, b in raw:
        if ts < last - (1 << 31):
            wraps += 1
        last = ts
        t = ((ts + (wraps << 32)) - t0) / hz
        events.append(Event(t, EVENTS.get(ev, f"ev{ev}"), a, b, CORE.get(ev, 0)))
    events.sort(key=lambda e: e.t)  # core 1 batches arrive a little after core 0 events
    return Trace(events, hz)


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        print(p, "->", load(p).summary())
