// TorBox for PS5, home-screen title: starts the server payload in the background and exits.
//
// The server runs as an elfldr payload, not inside this title: a title is closed when another game starts (and
// stopped in rest mode), which would cut downloads off. The payload keeps running while the user plays.
// Flow: if nothing answers on port 8089 yet, stream /app0/torbox-ps5.elf to elfldr on 127.0.0.1:9021 and wait
// for the server (which first stops a server of an older version on port 8080); then open the console's web
// browser on http://127.0.0.1:8089 (the same page phones and PCs use), and exit.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceKernelDebugOutText(int channel, const char *text);
int sceSystemServiceLoadExec(const char *path, char *const argv[]);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *param);
int sceUserServiceInitialize(void *param);

#define SERVER_PORT 8089
#define ELFLDR_PORT 9021
#define PAYLOAD "/app0/torbox-ps5.elf"

static void notify(const char *fmt, const char *arg) {
    notify_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, fmt, arg);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    char line[3100];
    snprintf(line, sizeof line, "[torbox-title] %s\n", req.message);
    sceKernelDebugOutText(0, line);
}

static int connect_local(int port) {
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
    return s;
}

static int server_up(void) {
    int s = connect_local(SERVER_PORT);
    if (s < 0) return 0;
    close(s);
    return 1;
}

static int send_payload(void) {
    int f = open(PAYLOAD, O_RDONLY);
    if (f < 0) return -2;
    int s = connect_local(ELFLDR_PORT);
    if (s < 0) {
        close(f);
        return -1;
    }
    static char buf[256 * 1024];
    ssize_t n;
    int rc = 0;
    while ((n = read(f, buf, sizeof buf)) > 0) {
        for (ssize_t at = 0; at < n;) {
            struct pollfd p = {s, POLLOUT, 0};
            if (poll(&p, 1, 10000) <= 0) { rc = -3; break; }
            ssize_t w = send(s, buf + at, n - at, 0);
            if (w <= 0) { rc = -3; break; }
            at += w;
        }
        if (rc) break;
    }
    close(f);
    close(s);
    return rc;
}

static void local_ip(char *out, size_t len) {
    snprintf(out, len, "<console IP>");
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (!connect(s, (struct sockaddr *)&a, sizeof a)) {
        socklen_t l = sizeof a;
        if (!getsockname(s, (struct sockaddr *)&a, &l)) inet_ntop(AF_INET, &a.sin_addr, out, len);
    }
    close(s);
}

// A title must not return from main or call exit(): leave through the system service.
void catchReturnFromMain(int status) {
    (void)status;
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) sleep(1);
}

int main(void) {
    char ip[64], url[128];
    local_ip(ip, sizeof ip);
    snprintf(url, sizeof url, "http://%s:%d", ip, SERVER_PORT);
    int up = server_up();
    if (!up) {
        int rc = send_payload();
        if (rc == -1) notify("TorBox: elfldr is not listening on port 9021. Start your ELF loader, or send "
                             "torbox-ps5.elf to it yourself. (%s)", "no server started");
        else if (rc) notify("TorBox: could not start the server (%s)", rc == -2 ? "torbox-ps5.elf missing" : "send failed");
        else {
            // Up to 25 s: the server first stops an older version on port 8080, which can take ~12 s.
            for (int i = 0; i < 125 && !(up = server_up()); i++) usleep(200 * 1000);
            if (!up) notify("TorBox: the server did not answer on %s yet", url); // when up, it says so itself
        }
    }
    if (up) {
        sceUserServiceInitialize(NULL);
        if (sceSystemServiceLaunchWebBrowser("http://127.0.0.1:" "8089", NULL))
            notify("TorBox is running: open %s from a phone or PC", url);
    }
    sleep(2); // let the browser come up / the notification show before the title closes
    catchReturnFromMain(0);
    return 0;
}
