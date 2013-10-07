#ifndef __CORO_H__
#define __CORO_H__
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif
    typedef struct coro coro_t;

    coro_t *coro_create(void (*start) (void *arg), void *arg);
    int     coro_init(void);
    void    coro_cleanup(void);
    void    coro_exit(void);
    void    coro_yield(void);
    int     coro_poll(int fd, int events, int timeout);
    int     coro_sleep(int usec);

    void    coro_set_rcvtimeout(size_t timeout);
    void    coro_set_sndtimeout(size_t timeout);

    int     coro_accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
    int     coro_connect(int fd, const struct sockaddr *addr, int addrlen);
    ssize_t coro_read(int fd, void *buf, size_t nbyte);
    ssize_t coro_recv(int fd, void *buf, size_t nbyte, int flags);
    ssize_t coro_read_fully(int fd, void *buf, size_t nbyte);
    ssize_t coro_write(int fd, const void *buf, size_t nbyte);
    ssize_t coro_send(int fd, const void *buf, size_t nbyte, int flags);
    ssize_t coro_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
    ssize_t coro_sendto(int fd, const void *msg, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
    ssize_t coro_recvmsg(int fd, struct msghdr *msg, int flags);
    ssize_t coro_sendmsg(int fd, const struct msghdr *msg, int flags);
#ifdef __cplusplus
}
#endif
#endif  /* !__CORO_H__ */
