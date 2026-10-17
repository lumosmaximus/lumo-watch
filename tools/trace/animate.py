#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "matplotlib"]
# ///
# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
"""Turn a trace into something you can watch.

    uv run tools/trace/animate.py docs/data/traces/gmt_idle.trace
        -> docs/data/traces/gmt_idle.html   interactive: play, pause, scrub, speed
    uv run tools/trace/animate.py X.trace --gif [--start 1.0 --length 0.3 --fps 30]
        -> X.gif                            a clip rendered with matplotlib

The page is one file with the events embedded: open it in a browser, embed
it in a post, or share it. It shows, as time plays back slowed down:
  - a timeline: panel TE edges, core 1's CPU (composing each box, stalls
    waiting for the DMA, the bezel repaint), the SPI/DMA transfer to the
    panel (estimated from the bytes sent), core 0's events (touch, buttons,
    USB keys, host state), and the messages between the cores
  - the screen: which bands/boxes of the 466x466 panel were redrawn in the
    current frame
  - numbers: fps, frame work, boxes, stall, face

The event format is apps/watch/common/trace.h; tracelib.py reads it. Other
projects: keep the 8-byte event format and edit tracelib.EVENTS/FRAMES and
the lane mapping in build_model() below.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import tracelib  # noqa: E402

PANEL = 466
QSPI_BYTES_PER_S = 40e6   # 4 lines x 80 MHz
SPI_OVERHEAD_S = 12e-6    # CASET/RASET/RAMWR header per window


def build_model(t: tracelib.Trace):
    frames = []
    for f in t.frames():
        boxes = []
        for b in f.boxes:
            dma0 = b.t1
            dma1 = dma0 + SPI_OVERHEAD_S + (b.w * b.h * 2) / QSPI_BYTES_PER_S
            boxes.append({"t0": b.t0, "t1": b.t1, "d0": dma0, "d1": dma1, "band": b.band,
                          "x0": b.x0, "w": b.w, "h": b.h})
        frames.append({"te": f.t_te, "t0": f.t0, "t1": f.t1, "face": f.face,
                       "work": f.work_ms, "boxes": boxes,
                       "stalls": [[a, b] for a, b in f.spi_waits],
                       "ring": list(f.ring) if f.ring else None})
    core0 = [{"t": e.t, "n": e.name, "a": e.a, "b": e.b}
             for e in t.events if e.core == 0 and e.name != "TRACE_ON"]
    msgs = [{"t": e.t, "id": e.a, "n": tracelib.MSGS.get(e.a, str(e.a))}
            for e in t.events if e.name == "MSG_RX"]
    keys = [{"t": e.t, "n": tracelib.KEYS.get(e.a, str(e.a))}
            for e in t.events if e.name in ("KEY_TX",)]
    return {"duration": t.duration, "frames": frames, "core0": core0, "msgs": msgs,
            "keys": keys, "panel": PANEL, "summary": t.summary()}


HTML = r"""<!doctype html>
<meta charset="utf-8">
<title>lumo-watch trace</title>
<style>
 :root{--bg:#120c18;--fg:#eee;--dim:#8a7f96;--line:#2b2236;--cpu:#ffb0d6;--stall:#ff5c6a;
       --dma:#7ac8ff;--ring:#ffd46e;--te:#b2a0ff;--c0:#8be3c3;--msg:#f0e68c}
 body{margin:0;background:var(--bg);color:var(--fg);font:13px/1.4 -apple-system,Segoe UI,Helvetica,Arial,sans-serif}
 header{display:flex;gap:12px;align-items:center;padding:10px 14px;border-bottom:1px solid var(--line);flex-wrap:wrap}
 header b{font-size:15px} header span.sum{color:var(--dim);flex:1 1 300px}
 button,select{background:#241a2e;color:var(--fg);border:1px solid #3a2d48;border-radius:6px;padding:5px 10px;font:inherit}
 button:hover{background:#30243d} input[type=range]{width:100%}
 main{display:grid;grid-template-columns:1fr 260px;gap:12px;padding:12px 14px}
 canvas{display:block;width:100%;background:#17101f;border:1px solid var(--line);border-radius:8px}
 #screen{aspect-ratio:1;width:100%}
 .stats{margin-top:10px;display:grid;grid-template-columns:1fr auto;gap:2px 10px;color:var(--dim)}
 .stats b{color:var(--fg);font-variant-numeric:tabular-nums;text-align:right}
 .legend{display:flex;gap:10px;flex-wrap:wrap;color:var(--dim);padding:0 14px 10px}
 .legend i{display:inline-block;width:12px;height:10px;border-radius:2px;margin-right:4px;vertical-align:-1px}
 @media (max-width:760px){main{grid-template-columns:1fr}}
</style>
<header>
 <b>lumo-watch trace</b>
 <button id="play">Play</button>
 <label>speed <select id="speed"><option value="0.01">1/100</option><option value="0.02" selected>1/50</option><option value="0.05">1/20</option><option value="0.1">1/10</option><option value="0.25">1/4</option><option value="1">1x</option></select></label>
 <label>window <select id="win"><option value="0.035">35 ms</option><option value="0.05" selected>50 ms</option><option value="0.1">100 ms</option><option value="0.25">250 ms</option><option value="1">1 s</option></select></label>
 <span class="sum" id="sum"></span>
</header>
<div style="padding:6px 14px 0"><input type="range" id="scrub" min="0" max="1000" value="0"></div>
<main>
 <div><canvas id="tl" height="330"></canvas></div>
 <div>
  <canvas id="screen" width="300" height="300"></canvas>
  <div class="stats" id="stats"></div>
 </div>
</main>
<div class="legend">
 <span><i style="background:var(--te)"></i>TE edge</span>
 <span><i style="background:var(--cpu)"></i>core 1 composing a box</span>
 <span><i style="background:var(--stall)"></i>core 1 stalled for DMA</span>
 <span><i style="background:var(--ring)"></i>bezel repaint</span>
 <span><i style="background:var(--dma)"></i>SPI/DMA to the panel (estimated)</span>
 <span><i style="background:var(--c0)"></i>core 0: touch / button / USB key / host</span>
 <span><i style="background:var(--msg)"></i>message core 0 &rarr; core 1</span>
</div>
<script>
const M = __MODEL__;
const tl = document.getElementById('tl'), sc = document.getElementById('screen');
const ctx = tl.getContext('2d'), sctx = sc.getContext('2d');
const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
document.getElementById('sum').textContent = M.summary;
let now = 0, playing = false, last = null;
const speed = () => +document.getElementById('speed').value;
const win = () => +document.getElementById('win').value;
const scrub = document.getElementById('scrub');
scrub.oninput = () => { now = scrub.value / 1000 * M.duration; draw(); };
document.getElementById('play').onclick = toggle;
document.addEventListener('keydown', e => { if (e.code === 'Space') { e.preventDefault(); toggle(); } });
function toggle(){ playing = !playing; document.getElementById('play').textContent = playing ? 'Pause' : 'Play'; last = null; if (playing) requestAnimationFrame(tick); }
function tick(ts){ if (!playing) return; if (last !== null) now += (ts - last) / 1000 * speed(); last = ts; if (now > M.duration) { now = 0; } scrub.value = now / M.duration * 1000; draw(); requestAnimationFrame(tick); }
function fit(){ const r = tl.getBoundingClientRect(); tl.width = r.width * devicePixelRatio; tl.height = 330 * devicePixelRatio; ctx.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0); }
window.onresize = () => { fit(); draw(); };
// lanes: y positions
const L = { te: 28, cpu: 70, dma: 120, c0: 170, msg: 215, frames: 262 };
function hue(band){ return `hsl(${(band * 23) % 360} 70% 70%)`; }
function draw(){
  const W = tl.getBoundingClientRect().width, H = 330, w = win(), t0 = now - w * 0.8, t1 = now + w * 0.2;
  const X = t => 70 + (t - t0) / w * (W - 80);
  ctx.clearRect(0,0,W,H);
  ctx.font = '12px sans-serif'; ctx.fillStyle = css('--dim');
  for (const [k, lab] of [['te','TE'],['cpu','core 1 CPU'],['dma','SPI/DMA'],['c0','core 0'],['msg','msgs'],['frames','frame']]) ctx.fillText(lab, 6, L[k] + 4);
  // grid: every ms (or 10 ms when the window is wide)
  const step = w > 0.2 ? 0.01 : 0.001;
  ctx.strokeStyle = css('--line'); ctx.lineWidth = 1;
  for (let t = Math.ceil(t0 / step) * step; t < t1; t += step){ const x = X(t); ctx.beginPath(); ctx.moveTo(x, 10); ctx.lineTo(x, H - 30); ctx.stroke(); }
  ctx.fillStyle = css('--dim'); ctx.fillText(`${(t0*1000).toFixed(1)} ms`, 70, H - 12); ctx.fillText(`${(t1*1000).toFixed(1)} ms`, W - 70, H - 12);
  let cur = null;
  for (const f of M.frames){
    if (f.t1 < t0 - 0.05 || f.t0 > t1) continue;
    if (f.t0 <= now && (cur === null || f.t0 > cur.t0)) cur = f;
    if (f.te !== null){ ctx.fillStyle = css('--te'); ctx.fillRect(X(f.te) - 1, L.te - 12, 2, 24); }
    // frame span
    ctx.fillStyle = 'rgba(255,255,255,0.08)'; ctx.fillRect(X(f.t0), L.frames - 8, Math.max(1, X(f.t1) - X(f.t0)), 16);
    ctx.fillStyle = css('--dim'); if (X(f.t1) - X(f.t0) > 40) ctx.fillText(`${f.work.toFixed(1)} ms, ${f.boxes.length} boxes`, X(f.t0) + 3, L.frames + 4);
    for (const b of f.boxes){
      ctx.fillStyle = hue(b.band); ctx.fillRect(X(b.t0), L.cpu - 14, Math.max(1, X(b.t1) - X(b.t0)), 28);
      ctx.fillStyle = css('--dma'); ctx.fillRect(X(b.d0), L.dma - 10, Math.max(1, X(b.d1) - X(b.d0)), 20);
    }
    for (const [a, b] of f.stalls){ ctx.fillStyle = css('--stall'); ctx.fillRect(X(a), L.cpu - 14, Math.max(1, X(b) - X(a)), 28); }
    if (f.ring){ ctx.fillStyle = css('--ring'); ctx.fillRect(X(f.ring[0]), L.cpu + 16, Math.max(1, X(f.ring[1]) - X(f.ring[0])), 6); }
  }
  for (const m of M.msgs){ if (m.t < t0 || m.t > t1) continue; ctx.fillStyle = css('--msg'); ctx.fillRect(X(m.t) - 1, L.msg - 10, 2, 20); ctx.fillText(m.n, X(m.t) + 3, L.msg - 12); }
  for (const e of M.core0){ if (e.t < t0 || e.t > t1) continue; ctx.fillStyle = css('--c0');
    const x = X(e.t); let lab = e.n.toLowerCase();
    if (e.n === 'TOUCH') { ctx.beginPath(); ctx.arc(x, L.c0, 5, 0, 7); ctx.fill(); lab = (e.a & 1 ? 'touch ' : 'up ') + (e.a >> 1) + 'f'; }
    else { ctx.fillRect(x - 1, L.c0 - 10, 2, 20); if (e.n === 'HID_KEY') lab = 'key ' + (__KEYS__[e.a] || e.a); if (e.n === 'HOST_STATE') lab = 'host vol ' + e.b; }
    ctx.fillText(lab, x + 6, L.c0 + 4); }
  // playhead
  ctx.strokeStyle = '#fff'; ctx.lineWidth = 1.5; ctx.beginPath(); ctx.moveTo(X(now), 8); ctx.lineTo(X(now), H - 28); ctx.stroke();
  drawScreen(cur);
}
function drawScreen(f){
  const S = 300, k = S / M.panel; sctx.clearRect(0,0,S,S);
  sctx.fillStyle = '#0d0912'; sctx.fillRect(0,0,S,S);
  sctx.strokeStyle = '#3a2d48'; sctx.beginPath(); sctx.arc(S/2, S/2, S/2 - 1, 0, 7); sctx.stroke();
  sctx.beginPath(); sctx.arc(S/2, S/2, 193 * k, 0, 7); sctx.stroke();
  const stats = document.getElementById('stats');
  if (!f){ stats.innerHTML = ''; return; }
  for (const b of f.boxes){
    const done = b.t1 <= now, started = b.t0 <= now; if (!started) continue;
    sctx.fillStyle = done ? hue(b.band) : 'rgba(255,255,255,0.5)';
    sctx.globalAlpha = done ? 0.85 : 0.6; sctx.fillRect(b.x0 * k, b.band * 8 * k, b.w * k, b.h * k); sctx.globalAlpha = 1;
  }
  // fps over the last second
  const recent = M.frames.filter(g => g.t0 > now - 1 && g.t0 <= now);
  const stall = f.stalls.reduce((s,[a,b]) => s + (b - a), 0) * 1000;
  const px = f.boxes.reduce((s,b) => s + b.w * b.h, 0);
  stats.innerHTML = [['face', f.face], ['fps (last 1 s)', recent.length], ['frame work', f.work.toFixed(2) + ' ms'],
    ['boxes', f.boxes.length], ['pixels redrawn', px.toLocaleString()], ['stalled for DMA', stall.toFixed(2) + ' ms'],
    ['TE to work start', f.te !== null ? ((f.t0 - f.te) * 1000).toFixed(2) + ' ms' : 'free-running'],
    ['time', (now * 1000).toFixed(1) + ' ms']].map(([k,v]) => `<span>${k}</span><b>${v}</b>`).join('');
}
fit(); draw();
</script>
"""


def write_html(model, path):
    html = HTML.replace("__MODEL__", json.dumps(model, separators=(",", ":")))
    html = html.replace("__KEYS__", json.dumps({k: v for k, v in tracelib.KEYS.items()}))
    with open(path, "w") as f:
        f.write(html)


def write_gif(model, path, start, length, fps):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation, PillowWriter
    from matplotlib.patches import Rectangle

    win = 0.05
    fig = plt.figure(figsize=(9, 3.6), dpi=80, facecolor="#120c18")
    ax = fig.add_axes([0.07, 0.12, 0.62, 0.8], facecolor="#17101f")
    sx = fig.add_axes([0.72, 0.1, 0.26, 0.85], facecolor="#0d0912")
    for a in (ax, sx):
        a.tick_params(colors="#8a7f96", labelsize=8)
        for sp in a.spines.values():
            sp.set_color("#2b2236")
    lanes = {"TE": 5, "core 1 CPU": 4, "SPI/DMA": 3, "core 0": 2, "msgs": 1}
    ax.set_yticks(list(lanes.values()))
    ax.set_yticklabels(list(lanes.keys()), color="#eee")
    ax.set_ylim(0.3, 5.7)
    sx.set_xlim(0, PANEL)
    sx.set_ylim(PANEL, 0)
    sx.set_xticks([])
    sx.set_yticks([])
    txt = fig.text(0.07, 0.95, "", color="#eee", fontsize=9)

    def hue(band):
        import colorsys
        return colorsys.hsv_to_rgb(((band * 23) % 360) / 360, 0.6, 1.0)

    def frame(i):
        now = start + i / fps
        t0, t1 = now - win * 0.8, now + win * 0.2
        ax.cla()
        ax.set_facecolor("#17101f")
        ax.set_xlim(t0 * 1000, t1 * 1000)
        ax.set_ylim(0.3, 5.7)
        ax.set_yticks(list(lanes.values()))
        ax.set_yticklabels(list(lanes.keys()), color="#eee")
        ax.set_xlabel("ms", color="#8a7f96")
        ax.tick_params(colors="#8a7f96", labelsize=8)
        cur = None
        for f in model["frames"]:
            if f["t1"] < t0 or f["t0"] > t1:
                continue
            if f["t0"] <= now:
                cur = f
            if f["te"] is not None:
                ax.vlines(f["te"] * 1000, 4.6, 5.4, color="#b2a0ff", lw=2)
            for b in f["boxes"]:
                ax.add_patch(Rectangle((b["t0"] * 1000, 3.6), (b["t1"] - b["t0"]) * 1000, 0.8,
                                       color=hue(b["band"])))
                ax.add_patch(Rectangle((b["d0"] * 1000, 2.7), (b["d1"] - b["d0"]) * 1000, 0.6,
                                       color="#7ac8ff"))
            for a, b in f["stalls"]:
                ax.add_patch(Rectangle((a * 1000, 3.6), (b - a) * 1000, 0.8, color="#ff5c6a"))
            if f["ring"]:
                ax.add_patch(Rectangle((f["ring"][0] * 1000, 3.45), (f["ring"][1] - f["ring"][0]) * 1000,
                                       0.12, color="#ffd46e"))
        for m in model["msgs"]:
            if t0 <= m["t"] <= t1:
                ax.vlines(m["t"] * 1000, 0.6, 1.4, color="#f0e68c", lw=2)
        for e in model["core0"]:
            if t0 <= e["t"] <= t1:
                ax.plot(e["t"] * 1000, 2, "o", color="#8be3c3", ms=5)
        ax.axvline(now * 1000, color="white", lw=1)
        sx.cla()
        sx.set_facecolor("#0d0912")
        sx.set_xlim(0, PANEL)
        sx.set_ylim(PANEL, 0)
        sx.set_xticks([])
        sx.set_yticks([])
        sx.add_patch(plt.Circle((PANEL / 2, PANEL / 2), PANEL / 2 - 1, fill=False, color="#3a2d48"))
        if cur:
            for b in cur["boxes"]:
                if b["t0"] <= now:
                    sx.add_patch(Rectangle((b["x0"], b["band"] * 8), b["w"], b["h"],
                                           color=hue(b["band"]) if b["t1"] <= now else "#777"))
            txt.set_text(f"{cur['face']}   frame work {cur['work']:.2f} ms   {len(cur['boxes'])} boxes"
                         f"   t = {now * 1000:.1f} ms")

    n = int(length * fps)
    anim = FuncAnimation(fig, frame, frames=n, interval=1000 / fps)
    anim.save(path, writer=PillowWriter(fps=fps))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--gif", action="store_true")
    ap.add_argument("--start", type=float, default=0.5, help="gif: start, s")
    ap.add_argument("--length", type=float, default=0.25, help="gif: trace seconds covered")
    ap.add_argument("--fps", type=int, default=25, help="gif: frames per trace-second x 100 (slow-mo)")
    ap.add_argument("--speed", type=float, default=0.02, help="gif: playback speed (0.02 = 1/50)")
    a = ap.parse_args()
    t = tracelib.load(a.trace)
    model = build_model(t)
    base = os.path.splitext(a.trace)[0]
    write_html(model, base + ".html")
    print(base + ".html")
    print(model["summary"])
    if a.gif:
        # frames per second of trace time = fps / speed
        write_gif(model, base + ".gif", a.start, a.length, int(a.fps / a.speed))
        print(base + ".gif")


if __name__ == "__main__":
    main()
