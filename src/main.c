#include "app.h"
#include "platform.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_application(bool smoke_test) {
    if (!platform_init()) {
        fprintf(stderr, "Cannot initialize platform libraries.\n");
        return 1;
    }

    app_initialize();
    app_set_wake(ui_wake);

    int result = ui_run(smoke_test);
    app_shutdown();
    return result;
}

#ifdef _WIN32
#include <windows.h>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show) {
    (void)instance;
    (void)previous;
    (void)show;

    bool smoke =
        getenv("NATIVE_CONTROLLER_SMOKE") != NULL || (command && strstr(command, "--smoke-test") != NULL);
    return run_application(smoke);
}
#else
int main(int argc, char **argv) {
    bool smoke = getenv("NATIVE_CONTROLLER_SMOKE") != NULL || (argc == 2 && !strcmp(argv[1], "--smoke-test"));
    return run_application(smoke);
}
#endif
