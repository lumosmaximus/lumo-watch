# Renders

Made by `tools/render/render.py`: the watch's own display code compiled for the
computer (`tools/render/host.c` + `shim/`), so every pixel is what the panel
shows. PNG = one frame; GIF = a clip at 20 fps (every third frame of the
60 Hz output), real time.

| Scene | What |
|---|---|
| `gmt` | GMT face at 10:09:30, second hand gliding |
| `gmt_bezel_turn` | the bezel turned 10 clicks by script |
| `chrono_black`, `chrono_white` | chronographs with the chronograph running |
| `remote` | the media remote, paused, computer at volume 63 |
| `remote_playing` | playing: visualiser, title and artist, the knob turned up |
| `remote_muted` | mute pressed on the computer |
| `remote_brightness` | after the sun button: knob amber, brightness level |
| `picker` | the face picker (BOOT held 3 s) |

Scenes are defined in `render.py` (`SCENES`): host arguments, which frame to
keep, how many frames the clip covers. `sh tools/render/build.sh` after any
change to `apps/watch/remote/src/main.c`.
