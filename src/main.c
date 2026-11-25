// TorBox for PS5: a web UI on port 8080 that browses a TorBox account and downloads to the console.
#include "app.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef PS5_TITLE
int sceSystemServiceLoadExec(const char *path, char *const argv[]);
#endif

int torbox_main(void) {
    plat_init();
    tb_init();
    rd_init();
    plat_log("TorBox for PS5 %s (%s) starting, data in %s", APP_VERSION, plat_mode(), plat_app_dir());
    dl_init(3);
    int port = APP_PORT;
    const char *env = getenv("TORBOX_PORT");
    if (env && atoi(env) > 0) port = atoi(env);
    char ip[64];
    plat_local_ip(ip, sizeof ip);
    plat_notify("TorBox: open http://%s:%d in a browser", ip, port);
    if (httpd_run(port) < 0) {
        plat_notify("TorBox: port %d is busy (already running?)", port);
        return 1;
    }
    dl_shutdown();
    plat_notify("TorBox: server stopped");
    return 0;
}

#ifndef PS5_TITLE
int main(void) { return torbox_main(); }
#endif
