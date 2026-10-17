# Diagram 6: the screens and how you move between them

Source: `apps/watch/remote/src/main.c` (face_setup, pick_face,
handle_buttons, remote_*), `tools/watchface/*.py`.

Draw a state diagram of round screens (each a 1:1 circle thumbnail) with
labelled transitions:

**Faces**
- **GMT** (dive-watch style): black dial, two-tone 24-hour bezel (blue night
  half, red day half) that turns by touch and clicks every 6 degrees (one
  minute), gold hour/minute hands with lume, red-tipped 24-hour GMT hand,
  gliding second hand with a lume dot, date window at 3.
- **Chronograph, black** (moon-watch style): black dial, fixed tachymeter
  bezel, 1/5-second track, three subdials: running seconds at 9, 30-minute
  counter at 3, 12-hour counter at 6; steel baton hands; central chrono hand.
- **Chronograph, white**: white lacquer dial, black bezel and print, red
  model line, black hands with cream lume, chrono hand with an open lozenge
  and a red tip.
- **Remote**: a knob ring of 48 rounded rainbow bars (white marker bar) that
  turns with the finger, a 60-segment level meter inside it, icon-only
  buttons: mute (top; red with a curved "MUTED" when muted), previous/next
  (left/right), play/pause (centre: triangle when paused, two bars when
  playing, a rainbow bar visualiser around it that bounces while playing and
  fades when paused), sun (bottom: brightness mode, knob turns amber). Title
  and artist under the play button, scrolling when long.
- **Face picker**: four thumbnails in a 2x2 grid, the selected one ringed.

**Transitions**
- Any face -> picker: hold BOOT 3 s. Picker -> face: tap a thumbnail; or BOOT
  moves the ring, PWR picks; 15 s without input returns.
- Any face <-> remote: hold two fingers 1.5 s, or one finger still in the
  middle 2 s.
- GMT: drag on the ring turns the bezel.
- Chronographs: PWR starts/stops the chronograph, BOOT (short) resets it.
- Remote: knob = volume (or brightness after the sun); tap buttons; PWR =
  play/pause; BOOT = toggle brightness mode; 30 s without touch returns the
  knob to volume. With the helper app: mute, play state and level follow the
  computer.

**Inputs box**: touch (CST9217, with finger count), BOOT (GPIO), PWR (via the
I/O expander), the computer's state over USB.

Clean, flat; the thumbnails can be stylised, no brand marks.
