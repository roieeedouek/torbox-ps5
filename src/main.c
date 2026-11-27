// TorBox for PS5: a web UI on port 8089 that browses TorBox, Real-Debrid and AllDebrid and downloads to the console.
#include "app.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef PS5_TITLE
int sceSystemServiceLoadExec(const char *path, char *const argv[]);
#endif

// One HTTP request to 127.0.0.1:port; out gets the start of the answer. -1 = nothing listening.
static int local_http(int port, const char *req, char *out, size_t len) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) {
        close(s);
        return -1;
    }
    for (size_t at = 0, n = strlen(req); at < n;) { // poll() first: the console ignores O_NONBLOCK on sockets
        struct pollfd p = {s, POLLOUT, 0};
        ssize_t w = poll(&p, 1, 3000) > 0 ? send(s, req + at, n - at, 0) : -1;
        if (w <= 0) break;
        at += (size_t)w;
    }
    size_t got = 0;
    while (got + 1 < len) {
        struct pollfd p = {s, POLLIN, 0};
        ssize_t r = poll(&p, 1, 3000) > 0 ? recv(s, out + got, len - 1 - got, 0) : -1;
        if (r <= 0) break;
        got += (size_t)r;
    }
    out[got] = 0;
    close(s);
    return (int)got;
}

// Versions before 1.1.1 listened on APP_LEGACY_PORT. If one of them still runs, two servers would work on the
// same queue and partial files: ask it to quit, wait until its port closes, then give its downloads time to stop
// and save their state (its shutdown waits up to 10 s for them). Only a server that says it is TorBox for PS5
// (its /api/status) is asked; any other app on that port just sees one GET.
static void stop_legacy_server(int own_port) {
    int port = APP_LEGACY_PORT, settle_ms = 11000;
#ifndef __PROSPERO__
    const char *env = getenv("TORBOX_LEGACY_PORT"); // tests; 0 = don't look
    if (env) port = atoi(env);
    env = getenv("TORBOX_LEGACY_SETTLE_MS");
    if (env) settle_ms = atoi(env);
#endif
    if (port <= 0 || port == own_port) return;
    static char buf[8192];
    if (local_http(port, "GET /api/status HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", buf, sizeof buf) <= 0)
        return;
    if (!strstr(buf, "\"providers\"") || !strstr(buf, "\"default_folder\"") || !strstr(buf, "\"title\":\"TorBox\""))
        return; // not a TorBox for PS5 server
    plat_log("an older TorBox server answers on port %d: stopping it", port);
    local_http(port,
               "POST /api/quit HTTP/1.1\r\nHost: 127.0.0.1\r\nX-TorBox-PS5: 1\r\nContent-Length: 0\r\n"
               "Connection: close\r\n\r\n",
               buf, sizeof buf);
    for (int i = 0; i < 100; i++) {
        if (local_http(port, "GET /api/status HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", buf,
                       sizeof buf) < 0)
            break;
        plat_sleep_ms(100);
    }
    plat_sleep_ms(settle_ms);
    plat_log("older server on port %d stopped", port);
}

int torbox_main(void) {
    plat_init();
    tb_init();
    rd_init();
    ad_init();
    update_init();
    plat_log("TorBox for PS5 %s (%s) starting, data in %s", APP_VERSION, plat_mode(), plat_app_dir());
    int port = APP_PORT;
    const char *env = getenv("TORBOX_PORT");
    if (env && atoi(env) > 0) port = atoi(env);
    // Claim the port before anything else: a second copy started meanwhile (the tile opened twice, a payload sent
    // again) then stops here, before it loads the queue or starts any download.
    int lfd = httpd_listen(port);
    if (lfd < 0) {
        plat_notify("TorBox: port %d is busy (already running?)", port);
        return 1;
    }
    stop_legacy_server(port); // before the queue is loaded; connections wait in the listen backlog meanwhile
    dl_init(3);
    char ip[64];
    plat_local_ip(ip, sizeof ip);
    plat_notify("TorBox: open http://%s:%d in a browser", ip, port);
    httpd_run(lfd);
    dl_shutdown();
    plat_notify("TorBox: server stopped");
    return 0;
}

#ifndef PS5_TITLE
int main(void) { return torbox_main(); }
#endif
