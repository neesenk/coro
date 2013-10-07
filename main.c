#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include "coro.h"
#include <assert.h>

int pollnum = 0;
int pollnum2 = 0;
int total = 1;
unsigned long long beg = 0;
unsigned long long longnum = 0;
int init_scokaddr(struct sockaddr_in *sockaddr, const char *addr, unsigned port)
{
    memset(sockaddr, 0, sizeof(*sockaddr));
	sockaddr->sin_family = AF_INET;
	sockaddr->sin_port = htons(port);
	sockaddr->sin_addr.s_addr = inet_addr(addr);
	if (sockaddr->sin_addr.s_addr == INADDR_NONE) {
        struct hostent *hp = gethostbyname(addr);
        if (hp == NULL)
            return -1;
        memcpy(&sockaddr->sin_addr, hp->h_addr, hp->h_length);
	}

    return 0;
}

int set_nonblock(int fd)
{
    int f = 0;
    if ((f = fcntl(fd, F_GETFL, 0)) < 0 || fcntl(fd, F_SETFL, f|O_NONBLOCK) < 0)
        return -1;
    return 0;
}

int create_listener(const char *addr, unsigned port)
{
    int sock = 0, n = 1;
	struct sockaddr_in serv_addr;

    if (init_scokaddr(&serv_addr, addr, port) != 0)
        return -1;

    if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    if (set_nonblock(sock) < 0)
        goto ERROR;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&n, sizeof(n)) < 0)
        goto ERROR;
	if (bind(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        goto ERROR;
	if (listen(sock, 12800) < 0)
        goto ERROR;
    return sock;

ERROR:
    close(sock);
    return -1;
}

int create_connecter(const char *addr, unsigned port)
{
    int sock = 0, n = 1;
	struct sockaddr_in serv_addr;

    if (init_scokaddr(&serv_addr, addr, port) != 0)
        return -1;
    if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0)
        return -1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&n, sizeof(n)) < 0)
        goto ERROR;
    if (set_nonblock(sock) < 0)
        goto ERROR;
	if (coro_connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        goto ERROR;
    return sock;

ERROR:
    printf("errno = %d, errmsg = %s\n", errno, strerror(errno));
    close(sock);
    return -1;
}

struct client {
    struct sockaddr_in addr;
    socklen_t addr_len;
    int fd;
};

static uint64_t st_mstime(void)
{
    struct timeval tv;
    (void)gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

unsigned long long sptime = 0;
int tootm[9] = { 0, };
static void summ(unsigned long long t)
{
    if (t <= 1000)
        tootm[0]++;
    else if (t <= 2000)
        tootm[1]++;
    else if (t <= 5000)
        tootm[2]++;
    else if (t <= 10000)
        tootm[3]++;
    else if (t <= 50000)
        tootm[4]++;
    else if (t <=100000)
        tootm[5]++;
    else if (t <= 500000)
        tootm[6]++;
    else if (t <= 1000000)
        tootm[7]++;
    else
        tootm[8]++;
}

#define summ_printf() printf("summ: %d %d %d %d %d %d %d %d %d\n", tootm[0], tootm[1], tootm[2], \
    tootm[3], tootm[4], tootm[5],tootm[6],tootm[7],tootm[8])

#define N  10000000
void server_lim(struct client *n)
{
    int fd = n->fd;
    char buff[1024];
    int i = 0;

    for (i = 0; i < N; i++) {
        unsigned long long s = st_mstime();
        unsigned long long e = 0;

        ssize_t r = coro_read_fully(fd, buff, 1024);
        ssize_t w = 0;
        if (r <= 0) {
            printf("errno = %d, errmsg = %s\n", errno, strerror(errno));
            printf("%d read err, close\n", fd);
            close(fd);
            return;
        }

        w = coro_write(fd, buff, r);
        if (w != r) {
            printf("errno = %d, errmsg = %s\n", errno, strerror(errno));
            printf("%d write err, close\n", fd);
            close(fd);
            return;
        }
        total++;
        e = st_mstime();
        sptime += e - s;
        summ(e - s);

        if (total % 100000 == 0) {
            unsigned long long t = st_mstime() - beg;
            printf("server: rec = %d, totl = %llu, avg = %llu, sp = %llu, asp = %llu, poll = %d,%d\n", total, t, t / total, sptime, sptime / total, pollnum,pollnum2);
            summ_printf();
        }

        coro_yield();
    }

    close(fd);
}

int port = 0;

int listfd = 0;
int accnum = 0;
void server_main(void)
{
    for (;;) {
        struct client client;
        client.addr_len = sizeof(client.addr);
        client.fd = coro_accept(listfd, (struct sockaddr *)&client.addr, &client.addr_len);

        if (client.fd >= 0) {
            accnum++;
            coro_create((void (*)(void *))server_main, NULL);
            server_lim(&client);
        } else {
            coro_yield();
        }
    }
}

int count = 0;
void client_main(void *argv)
{
    int i = 0;
    // st_sleep(random() % 500);
    for (i = 0; i < 100000; i++) {
        int fd = create_connecter("127.0.0.1", port);
        if (fd < 0) {
            printf("connect failed\n");
            coro_yield();
            continue;
        }

        for (;;) {
            char msg[1024] = "+PING xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
            char buff[1024];
            unsigned long long st = st_mstime();
            unsigned long long et = 0;
            ssize_t w = coro_write(fd, msg, 1024);
            ssize_t r = 0;

            if (w != 1024) {
                printf("errno = %d, errmsg = %s\n", errno, strerror(errno));
                printf("%d write failed close\n", fd);
                close(fd);
                break;
            }

            r = coro_read_fully(fd, buff, w);
            if (r < 0) {
                printf("errno = %d, errmsg = %s\n", errno, strerror(errno));
                printf("%d read failed close %llu, %d %d\n", fd, st_mstime() - st, (int)r, (int)w);
            }

            if (r <= 0) {
                close(fd);
                break;
            }

            assert(memcmp(msg, buff, 1024) == 0);
            total++;
            et = st_mstime();
            summ(et - st);
            sptime += et - st;

            if (total % 100000 == 0) {
                unsigned long long t = st_mstime() - beg;
                printf("client: rec = %d, totl = %llu, avg = %llu, sp = %llu, asp = %llu, poll = %d,%d\n", total, t, t / total, sptime, sptime / total, pollnum,pollnum2);
                summ_printf();
            }
            coro_yield();
        }
    }

    if (total % 100000 == 0) {
        unsigned long long t = st_mstime() - beg;
        printf("client: rec = %d, totl = %llu, avg = %llu, pollnum = %d\n", total, t, t / total, pollnum);
        summ_printf();
    }
}

int main(int argc, char *argv[])
{
    int i = 0;
    coro_init();

    beg = st_mstime();
    port = atoi(argv[2]);
    if (strcmp(argv[1], "client") != 0) {
        listfd = create_listener("127.0.0.1", port);
        if (listfd == -1) {
            printf("listen failed: %d, %s\n", errno, strerror(errno));
            exit(1);
        }
        for (i = 0; i < 50; i++)
            coro_create((void (*)(void *))server_main, NULL);
    } else {
        for (i = 0; i < 200; i++) {
            coro_create(client_main, NULL);
        }
    }

    for (;;) {
        coro_yield();
    }

    return 0;
}
