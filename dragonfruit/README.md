# Adding the browser and the WAP gate to Dragonfruit

The browser and the WAP gate need a few changes on the emulator side. They are in
`dragonfruit-integration.patch`, made against Dragonfruit `main` at
935de73.

1. Add this repository as a submodule:

   ```sh
   git submodule add git@github.com:SovGVD/cybiko-browser.git src/components/cy_browser
   ```

2. Apply the patch from the Dragonfruit root:

   ```sh
   git apply src/components/cy_browser/dragonfruit/dragonfruit-integration.patch
   ```

## What the patch changes

- **cybiko** (the emulator core): the `CY_APP_OP` opcode (`0x0042`), which
  calls `host.app_call(user, er0, er1)` and stops `cybiko_run` right after,
  and `cybiko_lcd_show()`, which puts a 160x100 picture on the LCD.
  In `cy_rf.c`, packets heard now reach CyOS at the speed they would come
  over the air (2400 bytes a second) instead of all at once, and A/D
  channel 0 reads the signal strength (high while a packet comes in). CyOS
  samples it 15 ms into each packet to rank the gates it hears; before, it
  read 0 and never picked a gate, so the WAP browser kept waiting.
- **cybiko_app** (the firmware's Cybiko mode):
  - `cybiko_browser.c`: installs Browser.app (the native app, `cyapp/`) on
    the RAM disk when it's missing or old. It also answers the call of the
    earlier launcher app and runs the ESP32-drawn browser in its own
    task (16 KB stack, internal RAM first, PSRAM if that's short) while the
    CPU task waits. On close it stops Wi-Fi and gives the radio back to the
    Cybiko link.
  - `cybiko_app.c`: the CPU task opens the browser when asked; keys go to
    the browser while it's open.
  - `cybiko_link.c`: creates the Wi-Fi station interface the browser needs,
    and `cybiko_link_resume()` puts ESP-NOW back on its channel.
  - `cybiko_gate.c`: the WAP gate (`gate/cy_gate.h`) in a task of its own,
    on the emulated radio of the board's own Cybiko. Its ID is the
    machine's with the first byte changed. It lays Browser's pages out with
    the fonts of the CyOS flash image. Keys in `/Cybiko/cybiko.cfg`:
    `wap.gate = off` turns it off, `wap.home = URL` sets the page Home
    opens. It pauses while the browser has the network.
  - `CMakeLists.txt`: requires the `cy_browser` component.
- **host** (the PC builds): `cybiko_host --browse-url URL --browse KEYS` and
  the SDL `cybiko` app open the browser, fetching with `curl`.
  `cybiko_host --gate [--gate-home URL]` puts the gate on the radio (with
  the fonts of `--flash`). Built only when
  `src/components/cy_browser` is there (not on Windows).

Without the submodule the firmware won't build (cybiko_app requires
`cy_browser`), so add both together.
