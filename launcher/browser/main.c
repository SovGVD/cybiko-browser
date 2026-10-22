/* The Browser icon on the Cybiko desktop. The browser itself runs in the
 * emulator (src/components/cy_browser); this program only asks for it and
 * waits until it closes, so CyOS goes back to the desktop afterwards. */
#include "../cyapp.h"

long main(int argc, char **argv, char start)
{
    (void)argc;
    (void)argv;
    if (start && cy_app_call(CY_APP_VERSION, 0) >= 1)
        cy_app_call(CY_APP_BROWSER, 0);
    return 0;
}
