#define _GNU_SOURCE
#include <stdlib.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <ucontext.h>
#include <stdio.h>
#include "coro.h"

#ifdef VALGRIND
#include <valgrind/valgrind.h>
#endif

#define DEFAULT_STACK_SIZE   (16*1024)
#define container_of(type, addr, field) ((type *)((char *)(addr) - ((size_t)&(((type *)0)->field))))
#define LIST_INIT(l) do {(l)->next=(l);(l)->prev=(l);} while (0)
#define LIST_DEL(e)  do {(e)->prev->next=(e)->next;(e)->next->prev=(e)->prev;LIST_INIT(e);} while (0)
#define LIST_EMPTY(l) ((l)->next == l)
#define LIST_ADD(e,l) do {(e)->next=(l)->next;(e)->prev=(l);(l)->next->prev=(e);(l)->next=(e);} while (0)
#define LIST_ADD_TAIL(e,l) do {(e)->next=(l);(e)->prev=(l)->prev;(l)->prev->next=(e);(l)->prev=(e);} while (0)

struct list { struct list *next, *prev; };

struct coro {
    struct list links;
    int state;
    int flags;
    int heap_index;

#ifdef VALGRIND
    unsigned int vstackid;
#endif
    uint64_t expires;
    uint64_t schedule_time;
    ucontext_t context;
};

struct pollctx {
    struct pollctx *next;
    coro_t *coro;
    int fd;
    int events;
    int revents;
    int is_wait;
};

struct fd_data { unsigned short events; struct pollctx *next; };
struct fdset {
    int epfd;
    struct fd_data *fd_data;
    size_t fd_data_size;
    struct epoll_event evtlist[1024];
};

struct heap { size_t nodes; size_t size; coro_t **heaps; };
struct coro_context {
    coro_t *main;
    coro_t *idle;

    struct list runable;
    struct list zombie;
    struct heap timer;
    struct fdset fdset;
    int run_count;
    int active_count;
    int stack_size;
    int rcvtimeout;
    int sndtimeout;
};

enum {ST_RUNNING=1, ST_RUNNABLE, ST_IOWAIT, ST_SLEEPING, ST_ZOMBIE};
enum {FL_MAIN=1, FL_IDLE=2, FL_ON_SLEEPQ=4, FL_ON_RUNQ=8};

static __thread struct coro_context  _local_context;
static __thread coro_t             *_current_thread;
#define CURRENT()   (_current_thread)
#define LOCALCTX()  (&_local_context)

static void add_to_runq(coro_t *coro);

static uint64_t ustime(void)
{
    struct timeval tv;
    (void)gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

#define CHILD_INDEX(n)	 (((n) * 2) + 1)
#define PARENT_INDEX(n)	 (((n) - 1) / 2)
#define HEAP_SWAP(h,a,b) do {				                    \
    coro_t *tmp = h->heaps[a];		                        \
    h->heaps[a] = h->heaps[b], h->heaps[a]->heap_index = a;     \
    h->heaps[b] = tmp, h->heaps[b]->heap_index = b;	            \
} while (0)

static inline void heap_siftdown(struct heap *h, size_t pos)
{
    size_t child = CHILD_INDEX(pos);
    for (; child < h->nodes; pos = child, child = CHILD_INDEX(child)) {
        if (child + 1 < h->nodes) {
            if (h->heaps[child]->expires > h->heaps[child+1]->expires)
                child++;
        }

        if (h->heaps[pos]->expires <= h->heaps[child]->expires)
            break;
        HEAP_SWAP(h, pos, child);
    }
}

static inline void heap_siftup(struct heap *h, size_t pos)
{
    size_t parent = 0;
    for (; pos; pos = parent) {
        parent = PARENT_INDEX(pos);
        if (h->heaps[pos]->expires >= h->heaps[parent]->expires)
            break;
        HEAP_SWAP(h, pos, parent);
    }
}

static inline int heap_resize(struct heap *h, size_t size)
{
    coro_t **n = NULL;
    assert(size > h->size);
    if (!(n = realloc(h->heaps, size * sizeof(*n))))
        return -1;
    h->heaps = n, h->size = size;
    return 0;
}

static void timer_del(struct heap *h, coro_t *coro)
{
    if (!(coro->flags & FL_ON_SLEEPQ))
        return;

    coro->flags &= ~FL_ON_SLEEPQ;
    assert(coro->heap_index < h->nodes && h->heaps[coro->heap_index] == coro);

    h->nodes--;
    h->heaps[coro->heap_index] = h->heaps[h->nodes];
    h->heaps[coro->heap_index]->heap_index = coro->heap_index;

    heap_siftdown(h, coro->heap_index);
    coro->heap_index = -1;
}

static int timer_add(struct heap *h, coro_t *coro, uint64_t timeout)
{
    timer_del(h, coro);
    assert(!(coro->flags & FL_ON_SLEEPQ));
    if (h->nodes >= h->size && heap_resize(h, h->size * 2))
        return -1;

    coro->expires = ustime() +  timeout;
    coro->flags |= FL_ON_SLEEPQ;
    coro->heap_index = h->nodes;
    h->heaps[h->nodes] = coro;
    h->nodes++;
    heap_siftup(h, coro->heap_index);

    return 0;
}

static coro_t *timer_min(struct heap *h)
{
    return h->nodes > 0 ? h->heaps[0] : NULL;
}

static void timer_dispatch(struct heap *h)
{
    coro_t *coro = NULL;

    while ((coro = timer_min(h)) != NULL && coro->expires <= ustime()) {
        assert(coro->flags & FL_ON_SLEEPQ && coro != LOCALCTX()->idle);
        timer_del(h, coro);
        add_to_runq(coro);
    }
}

static int fd_data_expand(struct fdset *self, int maxfd)
{
    struct fd_data *ptr = NULL;
    size_t n = self->fd_data_size;
    while (maxfd >= n)
        n <<= 1;

    if ((ptr = realloc(self->fd_data, n * sizeof(struct fd_data))) == NULL)
        return -1;

    memset(ptr + self->fd_data_size, 0, (n - self->fd_data_size) * sizeof(struct fd_data));
    self->fd_data = ptr;
    self->fd_data_size = n;

    return 0;
}

static int fdset_init(void)
{
    struct fdset *self = &LOCALCTX()->fdset;
    if ((self->epfd = epoll_create(4096)) < 0)
        return -1;
    fcntl(self->epfd, F_SETFD, FD_CLOEXEC);
    self->fd_data_size = 4096;
    self->fd_data = calloc(self->fd_data_size, sizeof(struct fd_data));
    if (!self->fd_data) {
        close(self->epfd);
        self->epfd = -1;
        return -1;
    }

    return 0;
}

static int fdset_set(struct fdset *self, int fd, int events)
{
    int old_events = 0;

    if (fd >= self->fd_data_size)
        return 0;

    old_events = self->fd_data[fd].events;
    self->fd_data[fd].events = events;

    if (events != old_events) {
        struct epoll_event ev;
        int op = events ? (old_events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD) : EPOLL_CTL_DEL;
        ev.events = events|EPOLLET;
        ev.data.fd = fd;
        if (epoll_ctl(self->epfd, op, fd, &ev) < 0) {
            self->fd_data[fd].events = old_events;
            return -1;
        }
    }

    return 0;
}

static void fdset_del(struct fdset *self, struct pollctx *pq)
{
    struct pollctx **pn = NULL;

    assert(pq->fd < self->fd_data_size);
    fdset_set(self, pq->fd, self->fd_data[pq->fd].events & ~pq->events);

    for (pn = &self->fd_data[pq->fd].next; *pn; pn = &(*pn)->next) {
        if (*pn == pq) {
            *pn = pq->next, pq->next = NULL;
            return;
        }
    }
}

static int fdset_add(struct fdset *self, struct pollctx *pq)
{
    if (pq->fd < 0 || !pq->events || (pq->events & ~(EPOLLIN|EPOLLOUT|EPOLLPRI))) {
        errno = EINVAL;
        return -1;
    }

    if (pq->fd >= self->fd_data_size && fd_data_expand(self, pq->fd) < 0)
        return -1;
    if (fdset_set(self, pq->fd, self->fd_data[pq->fd].events | pq->events) < 0)
        return -1;
    pq->next = self->fd_data[pq->fd].next, self->fd_data[pq->fd].next = pq;
    return 0;
}

static void fdset_dispatch(struct fdset *self, int max)
{
    int timeout = 0, n = 0, i = 0;

    if (!LIST_EMPTY(&LOCALCTX()->runable)) {
        coro_t *min = timer_min(&LOCALCTX()->timer);
        uint64_t now = ustime();
        if (min) {
            int mtimout = min->expires > now ? min->expires - now : -1;
            timeout = mtimout > 0 ? (mtimout > max ? max : mtimout) : 0;
        }
    }

    if ((n = epoll_wait(self->epfd, self->evtlist, 1024, timeout/1000)) <= 0)
        return;

    // 唤醒等待的coro
    for (i = 0; i < n; i++) {
        int fd = self->evtlist[i].data.fd;
        struct pollctx **pn = &self->fd_data[fd].next;

        if (self->evtlist[i].events & (EPOLLERR | EPOLLHUP))
            self->evtlist[i].events |= self->fd_data[fd].events;

        while (*pn) {
            struct pollctx *pq = *pn;
            pq->revents = (pq->events | EPOLLERR | EPOLLHUP) & self->evtlist[i].events;

            if (!pq->revents) {
                pn = &(*pn)->next;
                continue;
            }

            *pn = pq->next;
            pq->is_wait = 0;
            pq->next = NULL;

            if (pq->coro->flags & FL_ON_SLEEPQ)
                timer_del(&LOCALCTX()->timer, pq->coro);
            add_to_runq(pq->coro);
        }
    }
}

static void thread_main(void (*fn) (void *), void *arg)
{
    fn(arg);
	coro_exit();
}

static void add_to_runq(coro_t *coro)
{
    if (coro->flags & FL_ON_RUNQ)
        return;

    assert(coro->state != ST_ZOMBIE);
    coro->state = ST_RUNNABLE;
    assert(LIST_EMPTY(&coro->links));

    LIST_ADD_TAIL(&coro->links, &LOCALCTX()->runable);
    coro->flags |= FL_ON_RUNQ;
    LOCALCTX()->run_count++;
}

static void schedule(void)
{
    int ret = 0;
    coro_t *me = CURRENT(), *coro = NULL;
    uint64_t now = ustime();

    if (!LIST_EMPTY(&LOCALCTX()->runable) && LOCALCTX()->runable.next != &me->links)
        coro = container_of(coro_t, LOCALCTX()->runable.next, links);
    else
        coro = LOCALCTX()->idle;

    if (coro->flags & FL_ON_RUNQ) {
        LIST_DEL(&coro->links);
        coro->flags &= ~FL_ON_RUNQ;
        LOCALCTX()->run_count--;
    }

    if (me == coro)
        return;

    me->schedule_time = now;
    assert(coro->state == ST_RUNNABLE);
    coro->state = ST_RUNNING;
    CURRENT() = coro;

    ret = swapcontext(&me->context, &coro->context);
    assert(ret != -1);
}

static void idle_main(void *arg)
{
    int ret = 0;
    for (;;) {
        coro_yield();
        fdset_dispatch(&LOCALCTX()->fdset, 5000);
        timer_dispatch(&LOCALCTX()->timer);
    }

    ret = swapcontext(&CURRENT()->context, &LOCALCTX()->main->context);
    assert(ret != -1);
}

void coro_yield()
{
    add_to_runq(CURRENT());
    schedule();
}

void coro_cleanup(void)
{
    struct list *q = NULL, *s = NULL;
    assert(LOCALCTX()->active_count == 0);

    if (LOCALCTX()->idle)
        free(LOCALCTX()->idle);
    if (LOCALCTX()->main)
        free(LOCALCTX()->main);
    free(LOCALCTX()->timer.heaps);
    free(LOCALCTX()->fdset.fd_data);
    close(LOCALCTX()->fdset.epfd);

    for (q = LOCALCTX()->zombie.next; q != &LOCALCTX()->zombie; q = s) {
        coro_t *coro = container_of(coro_t, q, links);
        s = coro->links.next;
        assert(coro->state == ST_ZOMBIE);
        free(coro);
    }
}

int coro_init(void)
{
    if (CURRENT() != NULL)
        return 0;

    memset(LOCALCTX(), 0, sizeof(*LOCALCTX()));

    LIST_INIT(&LOCALCTX()->runable);
    LIST_INIT(&LOCALCTX()->zombie);

    LOCALCTX()->rcvtimeout = 1000000;
    LOCALCTX()->sndtimeout = 1000000;
    LOCALCTX()->stack_size = DEFAULT_STACK_SIZE;

    if ((LOCALCTX()->timer.heaps = calloc(1024, sizeof(coro_t *))) == NULL)
        return -1;
    LOCALCTX()->timer.size = 1024;

    if (fdset_init() != 0)
        return -1;

    if (!(LOCALCTX()->idle = coro_create(idle_main, NULL)))
        return -1;
    LOCALCTX()->idle->flags |= FL_IDLE;

    if ((LOCALCTX()->main  = calloc(1, sizeof(coro_t))) == NULL)
        return -1;
    LIST_INIT(&LOCALCTX()->main->links);
    LOCALCTX()->main->state = ST_RUNNING;
    LOCALCTX()->main->flags = FL_MAIN;
    CURRENT() = LOCALCTX()->main;

    return 0;
}

void coro_exit(void)
{
    coro_t *coro = CURRENT();

    coro->state = ST_ZOMBIE;
    assert(coro->links.next == &coro->links);
    LIST_ADD_TAIL(&coro->links, &LOCALCTX()->zombie);

    LOCALCTX()->active_count--;
#ifdef VALGRIND
    if (!(coro->flags & FL_MAIN))
        VALGRIND_STACK_DEREGISTER(coro->vstackid);
#endif
    schedule();
}

coro_t *coro_create(void (*fn) (void *), void *arg)
{
    coro_t *coro = NULL;
    size_t n = 1 + (LOCALCTX()->stack_size + sizeof(coro_t) - 1) / sizeof(coro_t);

    assert(n > 1);
    if (LIST_EMPTY(&LOCALCTX()->zombie)) { // 从zombie队列取一个
        if ((coro = calloc(n, sizeof(*coro))) == NULL)
            return NULL;
    } else {
        coro = container_of(coro_t, LOCALCTX()->zombie.next, links);
        assert(coro->state == ST_ZOMBIE);
        LIST_DEL(&coro->links);
    }

    if (getcontext(&coro->context) == -1) {
        free(coro);
        return NULL;
    }

    LIST_INIT(&coro->links);
    coro->context.uc_stack.ss_sp    = (char *)(coro + 1);
    coro->context.uc_stack.ss_size  = LOCALCTX()->stack_size;
    coro->context.uc_link           = &LOCALCTX()->idle->context;
    makecontext(&coro->context, (void (*)(void))thread_main, 2, fn, arg);
#ifdef VALGRIND
    coro->vstackid = VALGRIND_STACK_REGISTER((void *)(coro+1), (void *)(coro+n));
#endif
    LOCALCTX()->active_count++;
    add_to_runq(coro);

    return coro;
}

int coro_sleep(int usec)
{
    coro_t *me = CURRENT();
	me->state = ST_SLEEPING;
    timer_add(&LOCALCTX()->timer, me, usec);
	schedule();

	return 0;
}

int coro_poll(int fd, int events, int timeout)
{
    coro_t *me = CURRENT();
    struct pollctx ctx = {.coro=me, .fd=fd, .events=events, .revents=0, .is_wait=1};

    if (fdset_add(&LOCALCTX()->fdset, &ctx) < 0)
        return -1;
    if (timeout >= 0)
        timer_add(&LOCALCTX()->timer, me, timeout);

    me->state = ST_IOWAIT;
    schedule();
    fdset_del(&LOCALCTX()->fdset, &ctx);

    if (ctx.is_wait) {
        errno = ETIMEDOUT;
        return -1;
    }

    return ctx.revents;
}

#define NOT_READY_ERRNO ((errno == EAGAIN) || (errno == EWOULDBLOCK))
#define NETOPT(fnc, event, timeout) do {                        \
    int n;                                                      \
    while ((n = fnc) < 0) {                                     \
        if (errno == EINTR)                                     \
            continue;                                           \
        if (!NOT_READY_ERRNO || coro_poll(fd,event,timeout) < 0)\
            return -1;                                          \
    }                                                           \
    return n;                                                   \
} while (0)

void coro_set_rcvtimeout(size_t timeout)
{
    LOCALCTX()->rcvtimeout = timeout;
}

void coro_set_sndtimeout(size_t timeout)
{
    LOCALCTX()->sndtimeout = timeout;
}

int coro_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    NETOPT(accept4(fd, addr, addrlen, SOCK_NONBLOCK), EPOLLIN, LOCALCTX()->rcvtimeout);
}

int coro_connect(int fd, const struct sockaddr *addr, int addrlen)
{
    int n, err = 0;
    while (connect(fd, addr, addrlen) < 0) {
        if (errno != EINTR) {
            if (errno != EINPROGRESS && (errno != EADDRINUSE || err == 0))
                return -1;
            if (coro_poll(fd, EPOLLOUT, LOCALCTX()->sndtimeout) < 0)
                return -1;
            n = sizeof(int);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, (socklen_t *)&n) < 0)
                return -1;
            if (err) {
                errno = err;
                return -1;
            }
            break;
        }
        err = 1;
    }

    return 0;
}

ssize_t coro_read(int fd, void *buf, size_t nbyte)
{
    NETOPT(read(fd, buf, nbyte), EPOLLIN, LOCALCTX()->rcvtimeout);
}

ssize_t coro_read_fully(int fd, void *buf, size_t nbyte)
{
    size_t readn = 0;

    do {
        ssize_t n = coro_read(fd, (char *)buf + readn, nbyte - readn);
        if (n < 0)
            return -1;
        if (n == 0)
            return readn;
        readn += n;
    } while (readn < nbyte);

    return readn;
}

ssize_t coro_write(int fd, const void *buf, size_t nbyte)
{
    size_t writen = 0;
    do {
		ssize_t n = write(fd, (const char *)buf + writen, nbyte - writen);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (!NOT_READY_ERRNO)
                return -1;
            if (coro_poll(fd, EPOLLOUT, LOCALCTX()->sndtimeout) < 0)
                return -1;
        } else {
            writen += n;
        }
    } while (writen < nbyte);

    return writen;
}

ssize_t coro_recv(int fd, void *buf, size_t len, int flags)
{
    NETOPT(recv(fd, buf, len, flags), EPOLLIN, LOCALCTX()->rcvtimeout);
}

ssize_t coro_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
    NETOPT(recvfrom(fd, buf, len, flags, from, fromlen), EPOLLIN, LOCALCTX()->rcvtimeout);
}

ssize_t coro_send(int fd, const void *buf, size_t len, int flags)
{
    NETOPT(send(fd, buf, len, flags), EPOLLOUT, LOCALCTX()->sndtimeout);
}

ssize_t coro_sendto(int fd, const void *msg, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
    NETOPT(sendto(fd, msg, len, flags, to, tolen), EPOLLOUT, LOCALCTX()->sndtimeout);
}

ssize_t coro_recvmsg(int fd, struct msghdr *msg, int flags)
{
    NETOPT(recvmsg(fd, msg, flags), EPOLLIN, LOCALCTX()->rcvtimeout);
}

ssize_t coro_sendmsg(int fd, const struct msghdr *msg, int flags)
{
    NETOPT(sendmsg(fd, msg, flags), EPOLLOUT, LOCALCTX()->sndtimeout);
}
