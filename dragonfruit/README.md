# Adding the browser to Dragonfruit

The browser needs a few changes on the emulator side. They are in
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
- **cybiko_app** (the firmware's Cybiko mode):
  - `cybiko_browser.c`: installs Browser.app on the RAM disk when it's
    missing or old, answers the app's call, and runs the browser in its own
    task (16 KB stack, internal RAM first, PSRAM if that's short) while the
    CPU task waits. On close it stops Wi-Fi and gives the radio back to the
    Cybiko link.
  - `cybiko_app.c`: the CPU task opens the browser when asked; keys go to
    the browser while it's open.
  - `cybiko_link.c`: creates the Wi-Fi station interface the browser needs,
    and `cybiko_link_resume()` puts ESP-NOW back on its channel.
  - `CMakeLists.txt`: requires the `cy_browser` component.
- **host** (the PC builds): `cybiko_host --browse-url URL --browse KEYS` and
  the SDL `cybiko` app open the browser, fetching with `curl`. Built only when
  `src/components/cy_browser` is there (not on Windows).

Without the submodule the firmware won't build (cybiko_app requires
`cy_browser`), so add both together.
