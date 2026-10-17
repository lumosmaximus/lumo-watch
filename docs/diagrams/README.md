# Diagram prompts

One file per diagram. Paste a file's contents into Claude Design (or any
diagram tool) to get the picture; every fact in them comes from the sources
named at the top of each file, so if the code changes, update the prompt.

| File | Diagram |
|---|---|
| `01-hardware.md` | the board: chips, buses, pins, what talks to what |
| `02-cores-and-memory.md` | the two cores, their images, RAM/flash layout, how core 1 is started |
| `03-messages.md` | every message between the cores, direction, size, when it is sent |
| `04-render-pipeline.md` | one frame on core 1: TE, dirty bands, compose stages, SPI/DMA overlap |
| `05-usb-and-helper.md` | the USB composite device, the HID keys, and the helper app's return path |
| `06-faces-and-ui.md` | the screens: faces, picker, remote, and how you get between them |
| `07-asset-pipeline.md` | Python generators -> C arrays -> flash/RAM, the bezel/knob lookup |

Style for all of them: clean, flat, one accent colour per core (core 0 teal,
core 1 pink), monospace labels for register/pin names, no brand logos.
