/* SPDX-License-Identifier: GPL-2.0-only */
/* QEMU-only userspace exercise of the actual debugfs ABI and network hooks. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "SMOKE FAIL %s errno=%d\n", what, errno);
        exit(1);
    }
}

static int control(const char *text)
{
    int fd = open("/sys/kernel/debug/hnat418/control", O_WRONLY);
    ssize_t result;
    int saved;
    check(fd >= 0, "open control");
    result = write(fd, text, strlen(text));
    saved = errno;
    close(fd);
    errno = saved;
    return result == (ssize_t)strlen(text) ? 0 : -1;
}

static void traffic(void)
{
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(5201)};
    int listener, fd, status, reuse = 1;
    char payload[4096];
    pid_t child;
    memset(payload, 0x5a, sizeof(payload));
    listener = socket(AF_INET, SOCK_STREAM, 0);
    check(listener >= 0, "socket");
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    check(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0, "bind server");
    check(listen(listener, 1) == 0, "listen");
    child = fork();
    check(child >= 0, "fork");
    if (!child) {
        struct sockaddr_in local = {.sin_family = AF_INET};
        ssize_t n;
        size_t received = 0;
        close(listener);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        check(fd >= 0, "client socket");
        inet_pton(AF_INET, "127.0.0.2", &local.sin_addr);
        check(bind(fd, (struct sockaddr *)&local, sizeof(local)) == 0, "client bind");
        check(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0, "client connect");
        while ((n = read(fd, payload, sizeof(payload))) > 0)
            received += n;
        close(fd);
        _exit(received == 65536 ? 0 : 1);
    }
    fd = accept(listener, NULL, NULL);
    check(fd >= 0, "accept");
    for (int i = 0; i < 16; i++)
        check(write(fd, payload, sizeof(payload)) == sizeof(payload), "write stream");
    shutdown(fd, SHUT_WR);
    close(fd);
    close(listener);
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "client received bytes");
}

int main(void)
{
    const char *arm = "arm 127.0.0.1 127.0.0.2 5201 1 1 20\n";
    unsigned char *bytes = malloc(40 * 1024 * 1024);
    size_t total = 0;
    ssize_t n;
    uint32_t size, slots, cpus;
    uint64_t events = 0;
    int fd;

    alarm(30);
    check(bytes != NULL, "allocate read buffer");
    check(control("stop\n") == 0, "idle stop");
    check(control("arm 127.0.0.1 127.0.0.2 80 1 1 20\n") == -1 && errno == EINVAL, "port scope enforced");
    check(control(arm) == 0, "arm");
    check(control(arm) == -1 && errno == EBUSY, "active session not replaced");
    fd = open("/sys/kernel/debug/hnat418/records", O_RDONLY);
    check(fd == -1 && errno == EAGAIN, "live reader forbidden");
    traffic();
    check(control("freeze\n") == 0, "freeze");
    fd = open("/sys/kernel/debug/hnat418/records", O_RDONLY);
    check(fd >= 0, "frozen snapshot open");
    check(control("stop\n") == 0, "disarm with reader open");
    while ((n = read(fd, bytes + total, 4093)) > 0) {
        total += n;
        check(total < 39 * 1024 * 1024, "read bound");
    }
    check(n == 0 && total >= 64, "snapshot read after disarm");
    close(fd);
    check(memcmp(bytes, "H418RING", 8) == 0, "magic");
    memcpy(&size, bytes + 12, 4);
    memcpy(&slots, bytes + 16, 4);
    memcpy(&cpus, bytes + 20, 4);
    check(size == 144 && slots == 16384 && cpus >= 1 && cpus <= 16, "ABI dimensions");
    check(total == 64 + cpus * (32 + slots * size), "exact dump length");
    for (uint32_t cpu = 0; cpu < cpus; cpu++) {
        uint64_t count;
        memcpy(&count, bytes + 64 + cpu * (32 + slots * size) + 8, 8);
        events += count;
    }
    check(events > 0, "real TCP traffic recorded");
    for (int i = 0; i < 25; i++) {
        check(control("arm 127.0.0.1 127.0.0.2 5203 1 0 20\n") == 0, "quiet arm");
        check(control("freeze\n") == 0, "quiet freeze");
        check(control("stop\n") == 0, "quiet stop");
    }
    printf("HNAT418_RUNTIME_SMOKE_PASS bytes=%zu events=%llu cycles=25\n", total,
           (unsigned long long)events);
    free(bytes);
    return 0;
}
