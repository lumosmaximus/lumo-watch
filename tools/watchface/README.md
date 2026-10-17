# Face generators

Python + Pillow, run with `uv`. They draw at 3x and shrink, then emit C arrays
into `apps/watch/remote/src/`.

| Script | Makes |
|---|---|
| `watch.py` | the GMT dial, 31 date windows, and the bezel as a polar strip + per-pixel index |
| `chrono.py` | the black and white chronograph dials (tachymeter laid out along the curve) |
| `knob.py` | the remote's knob ring, same polar form as the bezel |

```
uv run watch.py preview            # preview.png
uv run watch.py export             # -> watch_img.c/.h
uv run chrono.py export ../../apps/watch/remote/src
uv run knob.py export ../../apps/watch/remote/src
```

How the polar strip works: `docs/diagrams/07-asset-pipeline.md`.
