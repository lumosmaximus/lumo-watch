# Host renderer

Core 1's display code compiled for the computer, with stub Zephyr headers
(`shim/`), simulated time and a frame buffer instead of the panel. Every
render in `docs/renders/` comes from here, pixel for pixel.

```
sh tools/render/build.sh          # after any change to apps/watch/remote/src/main.c
uv run tools/render/render.py     # all scenes -> docs/renders/ (PNG + GIF)
./tools/render/render --frames 120 --time 10:09:30 --at 0:face=0 --out frames/
```

Scenes (face, scripted touches, buttons, host state, now-playing text) are in
`render.py`'s `SCENES`; the harness options are at the top of `host.c`.
