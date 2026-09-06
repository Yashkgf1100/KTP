

#include "ksocket.h"

#define SELECT_US 100000   /* 100 ms */

/* ── Packet helpers ──────────────────────────────────────────────────────── */

static ssize_t xsend_ack(int fd, struct sockaddr_in *d, uint16_t s, uint16_t w)
{
    char b[PKTSZ];
    memcpy(b, "ACK\0", 4);
    uint16_t ns = htons(s), nw = htons(w);
    memcpy(b+4, &ns, 2); memcpy(b+6, &nw, 2); memset(b+HDRSZ, 0, MSGSZ);
    return sendto(fd, b, PKTSZ, 0, (struct sockaddr *)d, sizeof(*d));
}

static ssize_t xsend_data(int fd, struct sockaddr_in *d, uint16_t s, const char *m)
{
    char b[PKTSZ];
    memcpy(b, "DATA", 4);
    uint16_t ns = htons(s), nw = 0;
    memcpy(b+4, &ns, 2); memcpy(b+6, &nw, 2); memcpy(b+HDRSZ, m, MSGSZ);
    return sendto(fd, b, PKTSZ, 0, (struct sockaddr *)d, sizeof(*d));
}

static ssize_t xsend_fin(int fd, struct sockaddr_in *d)
{
    char b[PKTSZ]; memcpy(b, "FIN\0", 4); memset(b+4, 0, 4+MSGSZ);
    return sendto(fd, b, PKTSZ, 0, (struct sockaddr *)d, sizeof(*d));
}

static ssize_t xsend_fak(int fd, struct sockaddr_in *d)
{
    char b[PKTSZ]; memcpy(b, "FAK\0", 4); memset(b+4, 0, 4+MSGSZ);
    return sendto(fd, b, PKTSZ, 0, (struct sockaddr *)d, sizeof(*d));
}

static void xparse(const char *p, char *t, uint16_t *s, uint16_t *w, char *m)
{
    memcpy(t, p, 4); t[4] = '\0';
    *s = ntohs(*(uint16_t *)(p+4));
    *w = ntohs(*(uint16_t *)(p+6));
    memcpy(m, p+HDRSZ, MSGSZ);
}

/* ── Thread R state ──────────────────────────────────────────────────────── */

static fd_set master_fds;
static int    max_fd = -1;

static void do_close(k_sockinfo *sm, int semid, int i)
{
    sem_wait_slot(semid, i);
    if (sm[i].sockfd >= 0) {
        FD_CLR(sm[i].sockfd, &master_fds);
        close(sm[i].sockfd);
        sm[i].sockfd = -1;
    }
    sm[i].is_free   = true;
    sm[i].is_closed = true;
    sem_post_slot(semid, i);
    printf("R: freed KTP %d\n", i);
}

/* ── handle_data (semaphore held by caller) ──────────────────────────────── */

static void handle_data(k_sockinfo *sm, int i, uint16_t seq, const char *msg)
{
    int slot = -1;
    for (int j = sm[i].rwnd.base, c = 0; c < sm[i].rwnd.size;
         j = (j+1)%WINSZ, c++)
        if (sm[i].rwnd.seq[j] == seq) { slot = j; break; }

    if (slot == -1) {
        printf("R: KTP %d DATA %u out-of-window re-ACK %u\n",
               i, seq, sm[i].rwnd.last_ack);
        xsend_ack(sm[i].sockfd, &sm[i].dest_addr,
                  sm[i].rwnd.last_ack, (uint16_t)sm[i].rwnd.size);
        return;
    }
    if (sm[i].rwnd.recvd[slot]) {
        printf("R: KTP %d DATA %u dup re-ACK %u\n", i, seq, sm[i].rwnd.last_ack);
        xsend_ack(sm[i].sockfd, &sm[i].dest_addr,
                  sm[i].rwnd.last_ack, (uint16_t)sm[i].rwnd.size);
        return;
    }

    sm[i].rwnd.recvd[slot] = true;
    memcpy(sm[i].recv_buff[slot], msg, MSGSZ);

    int advance = 0;
    for (int k = sm[i].rwnd.base, ct = 0; ct < sm[i].rwnd.size;
         k = (k+1)%WINSZ, ct++) {
        if (!sm[i].rwnd.recvd[k]) break;
        advance++;
    }

    if (advance == 0) {
        printf("R: KTP %d DATA %u buffered OOO\n", i, seq);
        return;
    }

    for (int s = 0; s < advance; s++) {
        int b = sm[i].rwnd.base;
        sm[i].rwnd.last_ack = sm[i].rwnd.seq[b];
        uint16_t fr = sm[i].rwnd.next_seq; if (!fr) fr = 1;
        sm[i].rwnd.seq[b]   = fr;
        sm[i].rwnd.next_seq = (uint16_t)(fr % SEQSPACE + 1);
        sm[i].rwnd.base     = (b+1) % WINSZ;
        sm[i].rwnd.size--;
    }

    if (sm[i].rwnd.size == 0) sm[i].nospace = true;

    printf("R: KTP %d DATA %u ok; ACK %u rwnd %d\n",
           i, seq, sm[i].rwnd.last_ack, sm[i].rwnd.size);
    xsend_ack(sm[i].sockfd, &sm[i].dest_addr,
              sm[i].rwnd.last_ack, (uint16_t)sm[i].rwnd.size);
}

/* ── handle_ack (semaphore held by caller) ───────────────────────────────── */

static void handle_ack(k_sockinfo *sm, int i, uint16_t seq, uint16_t rwv)
{
    printf("R: KTP %d ACK %u rwnd %u\n", i, seq, rwv);
    int target = -1;
    for (int j = sm[i].swnd.base, c = 0; c < sm[i].swnd.size;
         j = (j+1)%WINSZ, c++)
        if (sm[i].swnd.seq[j] == seq) { target = j; break; }

    if (target == -1) { sm[i].swnd.size = (int)rwv; return; }

    while (1) {
        int cur = sm[i].swnd.base;
        sm[i].send_empty[cur]   = true;
        sm[i].swnd.sent_at[cur] = -1;
        uint16_t fr = sm[i].swnd.next_seq; if (!fr) fr = 1;
        sm[i].swnd.seq[cur]   = fr;
        sm[i].swnd.next_seq   = (uint16_t)(fr % SEQSPACE + 1);
        int done = (cur == target);
        sm[i].swnd.base = (cur+1) % WINSZ;
        if (done) break;
    }
    sm[i].swnd.size = (int)rwv;
}

/* ── Thread R ────────────────────────────────────────────────────────────── */

void *threadR(void *arg)
{
    (void)arg;
    k_sockinfo *sm    = k_shmat();
    int         semid = k_semget();
    if (!sm || semid == -1) { perror("threadR"); exit(1); }
    FD_ZERO(&master_fds); max_fd = -1;
    struct timeval tv;
    char pkt[PKTSZ];

    for (;;) {
        /* 1. Bind pending sockets */
        for (int i = 0; i < N; i++) {
            sem_wait_slot(semid, i);
            if (!sm[i].is_free && sm[i].bind_pending && !sm[i].is_bound) {
                int ufd = socket(AF_INET, SOCK_DGRAM, 0);
                if (ufd < 0) { perror("threadR:socket"); }
                else if (bind(ufd, (struct sockaddr *)&sm[i].src_addr,
                              sizeof(sm[i].src_addr)) < 0) {
                    perror("threadR:bind"); close(ufd);
                } else {
                    sm[i].sockfd       = ufd;
                    sm[i].bind_pending = false;
                    sm[i].is_bound     = true;
                    FD_SET(ufd, &master_fds);
                    if (ufd > max_fd) max_fd = ufd;
                    printf("R: KTP %d bound fd %d\n", i, ufd);
                }
            }
            sem_post_slot(semid, i);
        }

        /* 2. Free any sockets closed by thread S */
        for (int i = 0; i < N; i++) {
            sem_wait_slot(semid, i);
            bool need = !sm[i].is_free && sm[i].is_closed && sm[i].sockfd >= 0;
            sem_post_slot(semid, i);
            if (need) do_close(sm, semid, i);
        }

        /* 3. select() */
        fd_set rfds = master_fds;
        tv.tv_sec = 0; tv.tv_usec = SELECT_US;
        int nr = select(max_fd+1, &rfds, NULL, NULL, &tv);
        if (nr < 0) { if (errno == EINTR) continue; perror("select"); continue; }

        if (nr == 0) {
            /*
             * Timeout: resend probe ACK for any socket where the receiver
             * buffer is non-empty (rwnd > 0) but the sender's window is
             * stalled (swnd.size == 0).  We keep sending every timeout until
             * the sender's ACK comes back — this handles the case where the
             * probe ACK itself gets dropped.
             */
            for (int i = 0; i < N; i++) {
                sem_wait_slot(semid, i);
                if (!sm[i].is_free && sm[i].is_bound
                    && sm[i].rwnd.size > 0
                    && sm[i].rwnd.last_ack > 0
                    && sm[i].nospace == false) {
                    printf("R: KTP %d probe ACK %u rwnd %d\n",
                           i, sm[i].rwnd.last_ack, sm[i].rwnd.size);
                    xsend_ack(sm[i].sockfd, &sm[i].dest_addr,
                              sm[i].rwnd.last_ack, (uint16_t)sm[i].rwnd.size);
                }
                sem_post_slot(semid, i);
            }
            continue;
        }

        /* 4. Dispatch ready packets */
        for (int i = 0; i < N; i++) {
            /* Snapshot fd under lock, release BEFORE recvfrom */
            sem_wait_slot(semid, i);
            bool rdy = !sm[i].is_free && sm[i].is_bound
                       && FD_ISSET(sm[i].sockfd, &rfds);
            int ufd = sm[i].sockfd;
            sem_post_slot(semid, i);
            if (!rdy) continue;

            struct sockaddr_in sndr; socklen_t sl = sizeof(sndr);
            ssize_t nb = recvfrom(ufd, pkt, PKTSZ, 0,
                                  (struct sockaddr *)&sndr, &sl);
            if (nb < 0) { perror("recvfrom"); continue; }
            if (nb == 0) {
                sem_wait_slot(semid, i);
                sm[i].is_closing = true;
                sem_post_slot(semid, i);
                continue;
            }

            if (dropMessage(P)) {
                char t[5]; uint16_t s, w; char m[MSGSZ];
                xparse(pkt, t, &s, &w, m);
                printf("R: DROP %s %u KTP %d\n", t, s, i);
                continue;
            }

            char t[5]; uint16_t seq, rwv; char msg[MSGSZ];
            xparse(pkt, t, &seq, &rwv, msg);

            sem_wait_slot(semid, i);
            bool fp = (sndr.sin_addr.s_addr == sm[i].dest_addr.sin_addr.s_addr
                       && sndr.sin_port == sm[i].dest_addr.sin_port);
            if (!fp) {
                printf("R: KTP %d spurious src\n", i);
                sem_post_slot(semid, i); continue;
            }

            if      (!strcmp(t, "DATA")) handle_data(sm, i, seq, msg);
            else if (!strcmp(t, "ACK"))  handle_ack(sm, i, seq, rwv);
            else if (!strcmp(t, "FIN")) {
                printf("R: KTP %d FIN->FAK\n", i);
                xsend_fak(sm[i].sockfd, &sm[i].dest_addr);
                sm[i].is_closed = true;
            } else if (!strcmp(t, "FAK")) {
                printf("R: KTP %d FAK\n", i);
                sm[i].is_closed = true;
            } else {
                printf("R: KTP %d unknown '%.4s'\n", i, t);
            }
            sem_post_slot(semid, i);
        }
    }
    return NULL;
}

/* ── Thread S ────────────────────────────────────────────────────────────── */

void *threadS(void *arg)
{
    (void)arg;
    k_sockinfo *sm    = k_shmat();
    int         semid = k_semget();
    if (!sm || semid == -1) { perror("threadS"); exit(1); }
    struct timespec ht = { T/2, 0 };

    for (;;) {
        nanosleep(&ht, NULL);
        time_t now = time(NULL);

        for (int i = 0; i < N; i++) {
            sem_wait_slot(semid, i);
            if (sm[i].is_free || !sm[i].is_bound) {
                sem_post_slot(semid, i); continue;
            }

            /* FIN handshake */
            if (sm[i].is_closing && !sm[i].is_closed) {
                if (sm[i].fin_sent_at == (time_t)-1) {
                    xsend_fin(sm[i].sockfd, &sm[i].dest_addr);
                    sm[i].fin_sent_at = now;
                    printf("S: KTP %d FIN\n", i);
                } else if (now - sm[i].fin_sent_at >= T) {
                    if (sm[i].fin_retries < MAXFIN) {
                        sm[i].fin_retries++;
                        xsend_fin(sm[i].sockfd, &sm[i].dest_addr);
                        sm[i].fin_sent_at = now;
                        printf("S: KTP %d FIN retry %d\n", i, sm[i].fin_retries);
                    } else {
                        printf("S: KTP %d force close\n", i);
                        sm[i].is_closed = true;
                    }
                }
                sem_post_slot(semid, i); continue;
            }
            if (sm[i].is_closed) { sem_post_slot(semid, i); continue; }

            /* Retransmit timed-out window */
            int  base = sm[i].swnd.base;
            bool to   = !sm[i].send_empty[base]
                        && sm[i].swnd.sent_at[base] != (time_t)-1
                        && (now - sm[i].swnd.sent_at[base]) >= T;
            if (to) {
                for (int j = sm[i].swnd.base, c = 0; c < sm[i].swnd.size;
                     j = (j+1)%WINSZ, c++) {
                    if (sm[i].send_empty[j]) break;
                    if (sm[i].swnd.sent_at[j] == (time_t)-1) break;
                    printf("S: KTP %d retransmit seq %u slot %d\n",
                           i, sm[i].swnd.seq[j], j);
                    xsend_data(sm[i].sockfd, &sm[i].dest_addr,
                               sm[i].swnd.seq[j], sm[i].send_buff[j]);
                    sm[i].stat_trans++;
                    sm[i].swnd.sent_at[j] = now;
                }
            }

            /* Send new unsent messages */
            for (int j = sm[i].swnd.base, c = 0; c < sm[i].swnd.size;
                 j = (j+1)%WINSZ, c++) {
                if (sm[i].send_empty[j]) continue;
                if (sm[i].swnd.sent_at[j] != (time_t)-1) continue;
                printf("S: KTP %d send seq %u slot %d\n",
                       i, sm[i].swnd.seq[j], j);
                xsend_data(sm[i].sockfd, &sm[i].dest_addr,
                           sm[i].swnd.seq[j], sm[i].send_buff[j]);
                sm[i].stat_trans++;
                sm[i].swnd.sent_at[j] = now;
            }

            sem_post_slot(semid, i);
        }
    }
    return NULL;
}

/* ── Thread G ────────────────────────────────────────────────────────────── */

void *threadG(void *arg)
{
    (void)arg;
    k_sockinfo *sm    = k_shmat();
    int         semid = k_semget();
    if (!sm || semid == -1) { perror("threadG"); exit(1); }

    for (;;) {
        sleep(T);
        for (int i = 0; i < N; i++) {
            sem_wait_slot(semid, i);
            if (!sm[i].is_free && !sm[i].is_closing && !sm[i].is_closed)
                if (kill(sm[i].pid, 0) == -1 && errno == ESRCH) {
                    printf("G: KTP %d pid %d gone\n", i, sm[i].pid);
                    sm[i].is_closing = true;
                }
            sem_post_slot(semid, i);
        }
    }
    return NULL;
}

/* ── IPC setup ───────────────────────────────────────────────────────────── */

static void init_shm(void)
{
    key_t key = ftok(SHM_PATH, SHM_PROJ);
    if (key == -1) { perror("ftok shm"); exit(1); }
    int id = shmget(key, (size_t)N * sizeof(k_sockinfo),
                    IPC_CREAT | IPC_EXCL | 0600);
    if (id == -1) { perror("shmget"); exit(1); }
    k_sockinfo *sm = (k_sockinfo *)shmat(id, NULL, 0);
    if (sm == (void *)-1) { perror("shmat"); exit(1); }
    for (int i = 0; i < N; i++) {
        sm[i].is_free      = true;
        sm[i].sockfd       = -1;
        sm[i].probe_needed = false;
    }
    shmdt(sm);
    printf("initk: SHM id=%d size=%zu\n", id, (size_t)N * sizeof(k_sockinfo));
}

static void init_sem(void)
{
    key_t key = ftok(SEM_PATH, SEM_PROJ);
    if (key == -1) { perror("ftok sem"); exit(1); }
    int id = semget(key, N, IPC_CREAT | IPC_EXCL | 0600);
    if (id == -1) { perror("semget"); exit(1); }
    unsigned short v[N];
    for (int i = 0; i < N; i++) v[i] = 1;
    union semun a; a.array = v;
    if (semctl(id, 0, SETALL, a) == -1) { perror("semctl"); exit(1); }
    printf("initk: SEM id=%d\n", id);
}

static void cleanup(int sig)
{
    int a = k_shmget(), b = k_semget();
    if (a != -1) shmctl(a, IPC_RMID, NULL);
    if (b != -1) semctl(b, 0, IPC_RMID);
    printf("initk: cleaned up\n");
    if (sig == SIGSEGV) { signal(SIGSEGV, SIG_DFL); raise(SIGSEGV); }
    exit(0);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    srand((unsigned)time(NULL));
    { int s = k_shmget(); if (s != -1) shmctl(s, IPC_RMID, NULL); }
    { int s = k_semget(); if (s != -1) semctl(s, 0, IPC_RMID); }
    init_shm(); init_sem();
    signal(SIGINT, cleanup); signal(SIGTERM, cleanup); signal(SIGSEGV, cleanup);
    pthread_t tr, ts, tg;
    if (pthread_create(&tr, NULL, threadR, NULL) ||
        pthread_create(&ts, NULL, threadS, NULL) ||
        pthread_create(&tg, NULL, threadG, NULL)) {
        perror("pthread_create"); exit(1);
    }
    printf("initk: T=%ds P=%.2f N=%d\n", T, P, N);
    pthread_join(tr, NULL);
    return 0;
}