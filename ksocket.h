
/*
 * ksocket.h — KTP socket API
 * Reliable message-oriented transport over UDP using sliding window.
 */

#ifndef KSOCKET_H
#define KSOCKET_H

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/select.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Socket type */
#define SOCK_KTP    256

/* Sizes */
#define MSGSZ       512                   /* application payload bytes         */
#define HDRSZ       8                     /* 4-byte type + 2-byte seq + 2-byte rwnd */
#define PKTSZ       (HDRSZ + MSGSZ)      /* total wire packet size            */

/* Sequence space */
#define SEQBITS     5
#define SEQSPACE    (1 << SEQBITS)       /* 32 — must be > 2*WINSZ            */

/* Buffer / window sizing  */
#define BUFSZ       10                   /* message slots per socket           */
#define WINSZ       BUFSZ                /* max window == buffer size          */

/*  Timing and reliability */
#define T           5                    /* retransmit timeout (seconds)       */
#define P           0.30f                 /* simulated drop probability         */
#define MAXFIN      5                    /* max FIN retransmits before force-close */

/*  Concurrency*/
#define N           10                   /* max simultaneous KTP sockets       */

/*  IPC keys*/
#define SHM_PATH    "/tmp"
#define SHM_PROJ    'K'
#define SEM_PATH    "/tmp"
#define SEM_PROJ    'S'

/*  Error codes  */
#define ENOSPACE    ENOSPC               /* buffer / socket table full         */
#define ENOTBOUND   ENOTCONN             /* dest does not match k_bind addr    */
#define ENOMESSAGE  ENOMSG               /* recv buffer empty                  */

/*  Types  */
typedef int ksockfd_t;

/* Sliding window — used for both swnd and rwnd */
typedef struct {
    int      base;               /* circular index of oldest outstanding slot  */
    int      size;               /* usable slots right now                     */
    uint16_t next_seq;           /* next seq to assign (swnd) / expect (rwnd)  */
    uint16_t last_ack;           /* last in-order seq ACKed (rwnd only)        */
    uint16_t seq[WINSZ];         /* sequence number for each slot              */
    bool     recvd[WINSZ];       /* slot holds a valid message (rwnd)          */
    time_t   sent_at[WINSZ];     /* transmit epoch; -1 = not yet sent (swnd)   */
} window;

/* Per-socket shared-memory entry */
typedef struct {
    /* Allocation */
    bool     is_free;
    pid_t    pid;
    int      sockfd;             /* underlying UDP fd (managed by thread R)    */

    /* Addressing */
    struct sockaddr_in src_addr;
    struct sockaddr_in dest_addr;
    bool     bind_pending;       /* k_bind done; thread R must bind UDP socket */
    bool     is_bound;           /* UDP socket has been bound                  */

    /* Buffers */
    char     send_buff[BUFSZ][MSGSZ];
    char     recv_buff[BUFSZ][MSGSZ];
    bool     send_empty[BUFSZ];

    /* Windows */
    window   swnd;
    window   rwnd;

    /* Flow control */
    bool     nospace;            /* recv buffer was full (set when rwnd hits 0) */
    bool     probe_needed;       /* app freed a slot; thread R must send one probe ACK */

    /* Teardown */
    bool     is_closing;         /* k_close() called; start FIN handshake      */
    bool     is_closed;          /* fully closed; slot may be freed            */
    time_t   fin_sent_at;        /* time of last FIN (-1 = not sent yet)       */
    int      fin_retries;

    size_t   stat_msgs;          /* Total unique messages queued               */
    size_t   stat_trans;         /* Total physical transmissions (inc. retries)*/
} k_sockinfo;

/* Required by semctl(SETALL) */
union semun {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
};

/*  User API  */

/*
 * k_socket – allocate a KTP socket.
 * Finds a free slot in SM[], initialises it, returns the index.
 * Returns index >= 0, or -1 / errno = ENOSPACE.
 */
ksockfd_t k_socket(int domain, int type, int protocol);

/*
 * k_bind – record local and remote addresses; request UDP bind.
 * Sets bind_pending; thread R creates and binds the UDP socket.
 * Returns 0 or -1.
 */
int k_bind(ksockfd_t sockfd,
           const char *src_ip, int src_port,
           const char *dest_ip, int dest_port);

/*
 * k_sendto – enqueue a message for reliable delivery.
 * Verifies dest_addr matches the bound remote, copies up to MSGSZ bytes into
 * the first free send_buff slot, and marks it unsent (sent_at = -1).
 * Returns bytes enqueued, or -1 / errno = ENOTBOUND | ENOSPACE.
 */
ssize_t k_sendto(ksockfd_t sockfd,
                 const void *buf, size_t len, int flags,
                 const struct sockaddr *dest_addr, socklen_t addrlen);

/*
 * k_recvfrom – dequeue the next delivered message.
 * Copies the payload at rwnd.base (if recvd) into buf, releases the slot,
 * and increments rwnd.size so thread R can accept another message.
 * Returns MSGSZ on success, or -1 / errno = ENOMESSAGE.
 */
ssize_t k_recvfrom(ksockfd_t sockfd,
                   void *buf, size_t len, int flags,
                   struct sockaddr *src_addr, socklen_t *addrlen);

/*
 * k_close – begin graceful teardown.
 * Sets is_closing; thread S will send FIN and wait for FAK.
 * Returns 0 or -1.
 */
int k_close(ksockfd_t sockfd);

/* ── IPC helpers (shared between library and initksocket) ────────────────── */
int         k_shmget(void);
k_sockinfo *k_shmat(void);
int         k_shmdt(k_sockinfo *sm);
int         k_semget(void);
void        sem_wait_slot(int semid, int slot);
void        sem_post_slot(int semid, int slot);

/* ── Protocol / simulation helpers ──────────────────────────────────────── */
window init_window(void);
bool   dropMessage(float p);

#endif /* KSOCKET_H */