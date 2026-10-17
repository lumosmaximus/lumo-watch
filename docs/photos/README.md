# Real photos and videos: the shot list

Real pictures pull readers in; renders do not. Everything below is shot on
the real device. Photos go in `docs/photos/`, videos in `docs/video/`; the
README shows a photo as soon as its file exists (the site hides missing
ones), and videos get `<video>` tags once the files are there.

Tips: phone is fine. Landscape for videos. Dim the room a little so the AMOLED
does not blow out; tap to focus on the dial. Keep photos under ~400 KB (the
page loads them from GitHub), videos as H.264 MP4 under ~8 MB, 10-25 s each.

## Photos

| File | Shot | Used in |
|---|---|---|
| `hero.jpg` | the watch on the desk next to the laptop, remote screen lit, cable visible | top of the page |
| `board.jpg` | the bare board, front and back side by side (module, USB-C, battery connector, ribbon) | The board |
| `size.jpg` | the watch in a hand or next to a coin, for scale | The board |
| `hwtest.jpg` | hw_test's boundary pattern on the panel (`west flash` apps/hw_test) | The board |
| `gmt.jpg` | the GMT face, straight on, bezel at 12 | A simple watch first |
| `bezel.jpg` | a thumb turning the bezel, bezel visibly offset | A nicer human interface |
| `chrono_black.jpg`, `chrono_white.jpg` | each chronograph, chrono running | A nicer human interface |
| `picker.jpg` | the face picker on the screen | A nicer human interface |
| `remote.jpg` | the remote screen, playing, title visible | Volume, play, skip |
| `knob.jpg` | finger on the knob, the laptop's volume overlay visible behind | Volume, play, skip |
| `brightness.jpg` | the remote in brightness mode (knob amber), laptop screen noticeably dim or bright behind it | Volume, play, skip |
| `muted.jpg` | the remote showing MUTED in red, laptop mute icon behind | Volume, play, skip |
| `helper.jpg` | the watch showing the title that is playing on the laptop behind it (YouTube visible) | The cable does more than charge |
| `trace.jpg` | the trace viewer open on the laptop next to the watch | Seeing it |

## Videos

| File | Shot (10-25 s) | Used in |
|---|---|---|
| `glide.mp4` | close on the GMT face, 15 s, nothing but the second hand gliding; hold the phone still | A simple watch first |
| `crossing.mp4` | the second hand passing over the minute hand (set the clock just before: `T 2026-10-17 10:30:25`) | Making it glide |
| `bezel.mp4` | turning the bezel both ways, the clicks | A nicer human interface |
| `faces.mp4` | hold BOOT 3 s, the picker, tap each face in turn | A nicer human interface |
| `chrono.mp4` | PWR start, let it run, PWR stop, BOOT reset | A nicer human interface |
| `open_remote.mp4` | two fingers held on the glass, the remote appears; same gesture, the face comes back | Volume, play, skip |
| `volume.mp4` | turn the knob up and down with the laptop's volume overlay in frame; the level meter follows | Volume, play, skip |
| `brightness.mp4` | tap the sun, knob turns amber, turn it: the laptop screen dims and brightens in frame | Volume, play, skip |
| `mute_skip_play.mp4` | tap mute (red, MUTED), tap again; next, previous; play/pause with the visualiser starting and fading | Volume, play, skip |
| `follow.mp4` | a video playing on the laptop, pause it from the trackpad: the watch follows; resume; change the laptop volume: the meter follows | The cable does more than charge |
| `title.mp4` | a long title scrolling on the watch, the YouTube tab visible behind | The cable does more than charge |
| `settime.mp4` | plug in, run `uv run --with pyserial tools/settime.py` in a terminal, the hands jump to the right time | The cable does more than charge |
| `flash.mp4` | `sh tools/flash.sh build-watch` in a terminal, the watch rebooting into download mode and back (optional) | Seeing it |
| `trace.mp4` | screen recording of a trace page playing, then the watch doing the same thing (optional) | Seeing it |

## Order of priority

1. `hero.jpg`, `glide.mp4`, `volume.mp4`, `brightness.mp4`: the four that carry the story.
2. `bezel.mp4`, `open_remote.mp4`, `follow.mp4`, `title.mp4`.
3. The rest.
