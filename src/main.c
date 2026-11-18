#include "app.h"
#include "platform.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (!platform_init()) {
        fprintf(stderr, "Cannot initialize platform libraries.\n");
        return 1;
    }

    app_initialize();

    int result = ui_run(argc == 2 && !strcmp(argv[1], "--smoke-test"));
    app_shutdown();
    return result;
}
