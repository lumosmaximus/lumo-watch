# The helper app (macOS)

Keeps the watch's remote in step with the computer: volume, mute, brightness,
play state, and what is playing (YouTube in Chrome/Edge, Spotify, Music).

```
uv run tools/agent/agent_mac.py
```

- For YouTube: in Chrome, View > Developer > Allow JavaScript from Apple Events.
  The first run asks whether the terminal may control Chrome: allow.
- It finds the watch by USB id (1209:0001) and reconnects after a flash.
- Only one copy runs at a time (a lock file).
- The protocol is a few console lines, listed at the top of the script and in
  `docs/diagrams/05-usb-and-helper.md`.

A Windows version (media session API, pycaw, WMI) is planned.
