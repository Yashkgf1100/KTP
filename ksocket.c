

#include "ksocket.h"

/* ── IPC helpers ─────────────────────────────────────────────────────────── */

int k_shmget(void)
{
    key_t key = ftok(SHM_PATH, SHM_PROJ);
    if (key == -1) return -1;
    return shmget(key, 0, 0);
}

k_sockinfo *k_shmat(void)
{
    int shmid = k_shmget();
    if (shmid == -1) return NULL;
    k_sockinfo *sm = (k_sockinfo *)shmat(shmid, NULL, 0);
    if (sm == (void *)-1) return NULL;
    return sm;
}

int k_shmdt(k_sockinfo *sm)
{
    return shmdt(sm);
}

int k_semget(void)
{
    key_t key = ftok(SEM_PATH, SEM_PROJ);
    if (key == -1) return -1;
    return semget(key, 0, 0);
}

void sem_wait_slot(int semid, int slot)
{
    struct sembuf sb = { .sem_num = (unsigned short)slot,
                         .sem_op  = -1,
                         .sem_flg = 0 };
    while (semop(semid, &sb, 1) == -1) {
        if (errno != EINTR) { perror("sem_wait_slot"); exit(1); }
    }
}

void sem_post_slot(int semid, int slot)
{
    struct sembuf sb = { .sem_num = (unsigned short)slot,
                         .sem_op  = 1,
                         .sem_flg = 0 };
    while (semop(semid, &sb, 1) == -1) {
        if (errno != EINTR) { perror("sem_post_slot"); exit(1); }
    }
}

/* ── Window helper ───────────────────────────────────────────────────────── */

window init_window(void)
{
    window w;
    w.base     = 0;
    w.size     = WINSZ;
    w.next_seq = (uint16_t)(WINSZ + 1); /* next fresh seq after pre-loaded set */
    w.last_ack = 0;
    for (int i = 0; i < WINSZ; i++) {
        w.seq[i]     = (uint16_t)(i + 1); /* slots start with seq 1..WINSZ     */
        w.recvd[i]   = false;
        w.sent_at[i] = -1;
    }
    return w;
}

/* ── Simulation helper ───────────────────────────────────────────────────── */

bool dropMessage(float p)
{
    return ((float)rand() / (float)RAND_MAX) < p;
}

/* ── k_socket ────────────────────────────────────────────────────────────── */

ksockfd_t k_socket(int domain, int type, int protocol)
{
    if (domain != AF_INET || type != SOCK_KTP) {
        errno = EINVAL;
        return -1;
    }
    (void)protocol;

    k_sockinfo *sm = k_shmat();
    if (!sm) return -1;

    int semid = k_semget();
    if (semid == -1) { k_shmdt(sm); return -1; }

    ksockfd_t fd = -1;
    for (int i = 0; i < N; i++) {
        sem_wait_slot(semid, i);
        if (sm[i].is_free) {
            sm[i].is_free      = false;
            sm[i].pid          = getpid();
            sm[i].sockfd       = -1;
            sm[i].bind_pending = false;
            sm[i].is_bound     = false;
            sm[i].nospace       = false;
            sm[i].probe_needed  = false;
            sm[i].is_closing    = false;
            sm[i].is_closed    = false;
            sm[i].fin_sent_at  = -1;
            sm[i].fin_retries  = 0;

            sm[i].stat_msgs    = 0;
            sm[i].stat_trans   = 0;

            memset(&sm[i].src_addr,  0, sizeof(sm[i].src_addr));
            memset(&sm[i].dest_addr, 0, sizeof(sm[i].dest_addr));

            for (int j = 0; j < BUFSZ; j++)
                sm[i].send_empty[j] = true;

            sm[i].swnd = init_window();
            sm[i].rwnd = init_window();

            fd = i;
            sem_post_slot(semid, i);
            break;
        }
        sem_post_slot(semid, i);
    }

    k_shmdt(sm);

    if (fd == -1) {
        errno = ENOSPACE;
        return -1;
    }

    printf("k_socket: allocated KTP socket %d (pid %d)\n", fd, getpid());
    return fd;
}

/* ── k_bind ──────────────────────────────────────────────────────────────── */

int k_bind(ksockfd_t sockfd,
           const char *src_ip, int src_port,
           const char *dest_ip, int dest_port)
{
    if (sockfd < 0 || sockfd >= N) { errno = EBADF; return -1; }

    k_sockinfo *sm = k_shmat();
    if (!sm) return -1;

    int semid = k_semget();
    if (semid == -1) { k_shmdt(sm); return -1; }

    sem_wait_slot(semid, sockfd);

    if (sm[sockfd].is_free) {
        sem_post_slot(semid, sockfd);
        k_shmdt(sm);
        errno = EBADF;
        return -1;
    }

    sm[sockfd].src_addr.sin_family      = AF_INET;
    sm[sockfd].src_addr.sin_port        = htons((uint16_t)src_port);
    sm[sockfd].src_addr.sin_addr.s_addr = inet_addr(src_ip);

    sm[sockfd].dest_addr.sin_family      = AF_INET;
    sm[sockfd].dest_addr.sin_port        = htons((uint16_t)dest_port);
    sm[sockfd].dest_addr.sin_addr.s_addr = inet_addr(dest_ip);

    sm[sockfd].bind_pending = true;   /* signal thread R to do UDP bind        */

    sem_post_slot(semid, sockfd);
    k_shmdt(sm);

    printf("k_bind: KTP socket %d src=%s:%d dest=%s:%d (bind pending)\n",
           sockfd, src_ip, src_port, dest_ip, dest_port);
    return 0;
}

/* ── k_sendto ────────────────────────────────────────────────────────────── */

ssize_t k_sendto(ksockfd_t sockfd,
                 const void *buf, size_t len, int flags,
                 const struct sockaddr *dest_addr, socklen_t addrlen)
{
    (void)flags; (void)addrlen;
    if (sockfd < 0 || sockfd >= N) { errno = EBADF; return -1; }

    k_sockinfo *sm = k_shmat();
    if (!sm) return -1;

    int semid = k_semget();
    if (semid == -1) { k_shmdt(sm); return -1; }

    sem_wait_slot(semid, sockfd);

    if (sm[sockfd].is_free || sm[sockfd].is_closing || sm[sockfd].is_closed) {
        sem_post_slot(semid, sockfd);
        k_shmdt(sm);
        errno = EBADF;
        return -1;
    }

    /* Verify destination matches the bound remote address */
    if (dest_addr) {
        const struct sockaddr_in *dst = (const struct sockaddr_in *)dest_addr;
        if (dst->sin_addr.s_addr != sm[sockfd].dest_addr.sin_addr.s_addr ||
            dst->sin_port         != sm[sockfd].dest_addr.sin_port) {
            sem_post_slot(semid, sockfd);
            k_shmdt(sm);
            errno = ENOTBOUND;
            return -1;
        }
    }

    /* Find a free send slot (scan whole buffer, not just from swnd.base) */
    int slot = -1;
    for (int j = 0; j < BUFSZ; j++) {
        if (sm[sockfd].send_empty[j]) { slot = j; break; }
    }

    if (slot == -1) {
        sem_post_slot(semid, sockfd);
        k_shmdt(sm);
        errno = ENOSPACE;
        return -1;
    }

    ssize_t copy = (ssize_t)((len < MSGSZ) ? len : MSGSZ);
    memcpy(sm[sockfd].send_buff[slot], buf, (size_t)copy);
    if (copy < MSGSZ)
        memset(sm[sockfd].send_buff[slot] + copy, 0, (size_t)(MSGSZ - copy));

    sm[sockfd].send_empty[slot]       = false;
    sm[sockfd].swnd.sent_at[slot]     = -1;   /* not yet transmitted           */

    sm[sockfd].stat_msgs++;
    
    sem_post_slot(semid, sockfd);
    k_shmdt(sm);

    printf("k_sendto: KTP socket %d enqueued %zd bytes into slot %d\n",
           sockfd, copy, slot);
    return copy;
}

/* ── k_recvfrom ──────────────────────────────────────────────────────────── */

ssize_t k_recvfrom(ksockfd_t sockfd,
                   void *buf, size_t len, int flags,
                   struct sockaddr *src_addr, socklen_t *addrlen)
{
    (void)flags;
    if (sockfd < 0 || sockfd >= N) { errno = EBADF; return -1; }

    k_sockinfo *sm = k_shmat();
    if (!sm) return -1;

    int semid = k_semget();
    if (semid == -1) { k_shmdt(sm); return -1; }

    sem_wait_slot(semid, sockfd);

    if (sm[sockfd].is_free) {
        sem_post_slot(semid, sockfd);
        k_shmdt(sm);
        errno = EBADF;
        return -1;
    }

    int base = sm[sockfd].rwnd.base;

    if (!sm[sockfd].rwnd.recvd[base]) {
        sem_post_slot(semid, sockfd);
        k_shmdt(sm);
        errno = ENOMESSAGE;
        return -1;
    }

    /* Copy payload */
    ssize_t copy = (ssize_t)((len < MSGSZ) ? len : MSGSZ);
    memcpy(buf, sm[sockfd].recv_buff[base], (size_t)copy);

    if (src_addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        memcpy(src_addr, &sm[sockfd].dest_addr, sizeof(struct sockaddr_in));
        *addrlen = sizeof(struct sockaddr_in);
    }

    /* Release the slot */
    sm[sockfd].rwnd.recvd[base] = false;
    sm[sockfd].rwnd.base        = (base + 1) % WINSZ;
    sm[sockfd].rwnd.size++;

    /* If buffer was full, clear nospace and request exactly one probe ACK
     * from thread R so the stalled sender learns the window has reopened. */
    if (sm[sockfd].nospace && sm[sockfd].rwnd.size > 0) {
        sm[sockfd].nospace      = false;
        sm[sockfd].probe_needed = true;
    }

    uint16_t seq = sm[sockfd].rwnd.seq[base];
    sem_post_slot(semid, sockfd);
    k_shmdt(sm);

    printf("k_recvfrom: KTP socket %d delivered seq %u from slot %d\n",
           sockfd, seq, base);
    return copy;
}

/* ── k_close ─────────────────────────────────────────────────────────────── */

int k_close(ksockfd_t sockfd)
{
    if (sockfd < 0 || sockfd >= N) { errno = EBADF; return -1; }

    k_sockinfo *sm = k_shmat();
    if (!sm) return -1;

    int semid = k_semget();
    if (semid == -1) { k_shmdt(sm); return -1; }

    sem_wait_slot(semid, sockfd);

    if (!sm[sockfd].is_free && !sm[sockfd].is_closing)
        sm[sockfd].is_closing = true;

    sem_post_slot(semid, sockfd);
    k_shmdt(sm);

    printf("k_close: KTP socket %d marked for closing\n", sockfd);
    return 0;
}