#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#include "../common/models.h"
#include "../common/file_io.h"
#include "../common/auth.h"
#include "../common/auction_engine.h"
#include "../common/bid_handler.h"
#include "../common/wallet.h"
#include "../common/dispute.h"
#include "../common/admin.h"
#include "../common/notify.h"

/* ─── configuration ──────────────────────────────────────── */
#define PORT             8080
#define BACKLOG          16      /* TCP listen backlog                */
#define THREAD_POOL_SIZE  8      /* worker threads (2 × CPU cores)    */
#define WORK_QUEUE_SIZE  64      /* max pending connections           */

/* ══════════════════════════════════════════════════════════
   THREAD POOL  — work queue + worker threads
   ══════════════════════════════════════════════════════════ */
typedef struct {
    int  fds[WORK_QUEUE_SIZE];  /* circular buffer of client fds    */
    int  front;                 /* index of next fd to consume      */
    int  rear;                  /* index where next fd will land    */
    int  count;                 /* current items in queue           */

    pthread_mutex_t lock;       /* guards front/rear/count          */
    sem_t           available;  /* counting semaphore: #jobs ready  */
} WorkQueue;

static WorkQueue   wq;
static pthread_t   pool[THREAD_POOL_SIZE];
static int         server_fd = -1;

/* ──────────────────────────────────────────────────────────
   enqueue_client(fd)
   Called by the main accept() loop.
   Pushes fd into the circular buffer, then sem_post() wakes
   exactly one sleeping worker.
   ────────────────────────────────────────────────────────── */
static int enqueue_client(int fd) {
    pthread_mutex_lock(&wq.lock);

    if (wq.count == WORK_QUEUE_SIZE) {
        /* Queue full — this is our DoS protection.
           We reject gracefully instead of crashing.       */
        pthread_mutex_unlock(&wq.lock);
        fprintf(stderr, "[server] work queue full, dropping connection\n");
        close(fd);
        return -1;
    }

    wq.fds[wq.rear] = fd;
    wq.rear  = (wq.rear + 1) % WORK_QUEUE_SIZE;
    wq.count++;

    pthread_mutex_unlock(&wq.lock);

    sem_post(&wq.available);   /* wake one worker */
    return 0;
}

/* ──────────────────────────────────────────────────────────
   dequeue_client()
   Called by worker threads. Blocks (via sem_wait) until a
   job is available, then pops and returns the fd.
   ────────────────────────────────────────────────────────── */
static int dequeue_client(void) {
    sem_wait(&wq.available);   /* sleep here if queue is empty     */

    pthread_mutex_lock(&wq.lock);

    int fd = wq.fds[wq.front];
    wq.front = (wq.front + 1) % WORK_QUEUE_SIZE;
    wq.count--;

    pthread_mutex_unlock(&wq.lock);
    return fd;
}

/* ══════════════════════════════════════════════════════════
   MESSAGE I/O HELPERS
   send_msg / recv_msg wrap the raw socket read/write so the
   rest of the code never does partial-read arithmetic.
   ══════════════════════════════════════════════════════════ */

/*
 * send_msg(fd, type, status, payload, length)
 *   Sends a MessageHeader followed by `length` bytes of payload.
 *   Returns 0 on success, -1 on error.
 */
int send_msg(int fd, MessageType type, int status,
             const void *payload, uint32_t length) {
    MessageHeader hdr = { .type = type, .length = length, .status = status };

    /* send header */
    if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) return -1;

    /* send payload (if any) */
    if (length > 0 && payload != NULL) {
        ssize_t sent = 0;
        const char *ptr = payload;
        while (sent < (ssize_t)length) {
            ssize_t n = write(fd, ptr + sent, length - sent);
            if (n <= 0) return -1;
            sent += n;
        }
    }
    return 0;
}

/*
 * recv_msg(fd, hdr_out, buf, buf_size)
 *   Reads a MessageHeader into *hdr_out, then reads hdr.length
 *   bytes of payload into buf (capped at buf_size).
 *   Returns 0 on success, -1 on disconnect or error.
 */
int recv_msg(int fd, MessageHeader *hdr_out, void *buf, size_t buf_size) {
    /* read header — must be exact */
    ssize_t r = read(fd, hdr_out, sizeof(MessageHeader));
    if (r != sizeof(MessageHeader)) return -1;

    if (hdr_out->length == 0) return 0;

    /* read payload */
    uint32_t to_read = hdr_out->length < buf_size
                     ? hdr_out->length : (uint32_t)buf_size;
    ssize_t got = 0;
    char *ptr = buf;
    while (got < (ssize_t)to_read) {
        ssize_t n = read(fd, ptr + got, to_read - got);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════
   DISPATCH — route incoming messages to the right handler
   ══════════════════════════════════════════════════════════ */

/*
 * handle_client(fd)
 *   Runs the full conversation with one connected client.
 *   Loops receiving messages and calling the right handler
 *   until the client disconnects or sends MSG_LOGOUT_REQ.
 *
 *   Each handler returns 0 (keep going) or -1 (close connection).
 */
static void handle_client(int client_fd) {
    char buf[4096];
    MessageHeader hdr;
    int running = 1;

    /* The logged-in user for this connection (filled by auth) */
    User current_user;
    memset(&current_user, 0, sizeof(current_user));
    int authenticated = 0;

    printf("[server] client connected on fd=%d\n", client_fd);

    while (running) {
        memset(buf, 0, sizeof(buf));

        if (recv_msg(client_fd, &hdr, buf, sizeof(buf)) < 0) {
            printf("[server] client fd=%d disconnected\n", client_fd);
            break;
        }

        /* ── unauthenticated: only allow LOGIN ── */
        if (!authenticated && hdr.type != MSG_LOGIN_REQ) {
            send_msg(client_fd, MSG_ERROR, ERR_AUTH_FAIL, NULL, 0);
            continue;
        }

        switch (hdr.type) {

            /* ── AUTH ── */
            case MSG_LOGIN_REQ:
                authenticated = handle_login(client_fd, buf, &current_user);
                break;

            case MSG_LOGOUT_REQ:
                handle_logout(client_fd, &current_user);
                running = 0;
                break;

            case MSG_CHANGE_PASS_REQ:
                handle_change_password(client_fd, buf, &current_user);
                break;

            /* ── BIDDER ── */
            case MSG_VIEW_AUCTIONS_REQ:
                handle_view_auctions(client_fd);
                break;

            case MSG_PLACE_BID_REQ:
                if (current_user.role == ROLE_BIDDER)
                    handle_place_bid(client_fd, buf, &current_user);
                else
                    send_msg(client_fd, MSG_ERROR, ERR_PERMISSION, NULL, 0);
                break;

            case MSG_VIEW_BALANCE_REQ:
                handle_view_balance(client_fd, &current_user);
                break;

            case MSG_DEPOSIT_REQ:
                handle_deposit(client_fd, buf, &current_user);
                break;

            case MSG_VIEW_MY_BIDS_REQ:
                handle_view_my_bids(client_fd, &current_user);
                break;

            case MSG_SUBMIT_FEEDBACK_REQ:
                handle_submit_feedback(client_fd, buf, &current_user);
                break;

            /* ── AUCTIONEER ── */
            case MSG_CREATE_AUCTION_REQ:
                if (current_user.role == ROLE_AUCTIONEER ||
                    current_user.role == ROLE_ADMIN)
                    handle_create_auction(client_fd, buf, &current_user);
                else
                    send_msg(client_fd, MSG_ERROR, ERR_PERMISSION, NULL, 0);
                break;

            case MSG_CLOSE_AUCTION_REQ:
                if (current_user.role == ROLE_AUCTIONEER ||
                    current_user.role == ROLE_ADMIN)
                    handle_close_auction(client_fd, buf, &current_user);
                else
                    send_msg(client_fd, MSG_ERROR, ERR_PERMISSION, NULL, 0);
                break;

            /* ── MODERATOR ── */
            case MSG_VIEW_DISPUTES_REQ:
                if (current_user.role >= ROLE_MODERATOR)
                    handle_view_disputes(client_fd);
                else
                    send_msg(client_fd, MSG_ERROR, ERR_PERMISSION, NULL, 0);
                break;

            case MSG_RESOLVE_DISPUTE_REQ:
                if (current_user.role >= ROLE_MODERATOR)
                    handle_resolve_dispute(client_fd, buf, &current_user);
                else
                    send_msg(client_fd, MSG_ERROR, ERR_PERMISSION, NULL, 0);
                break;

            case MSG_VIEW_FEEDBACK_REQ:
                if (current_user.role >= ROLE_MODERATOR)
                    handle_view_feedback(client_fd);
                else
                    send_msg(client_fd, MSG_ERROR, ERR_PERMISSION, NULL, 0);
                break;

            case MSG_TOGGLE_ACCOUNT_REQ:
                if (current_user.role >= ROLE_MODERATOR)
                    handle_toggle_account(client_fd, buf, &current_user);
                else
                    send_msg(client_fd, MSG_ERROR, ERR_PERMISSION, NULL, 0);
                break;

            /* ── ADMIN ── */
            case MSG_ADD_USER_REQ:
                if (current_user.role == ROLE_ADMIN)
                    handle_add_user(client_fd, buf);
                else
                    send_msg(client_fd, MSG_ERROR, ERR_PERMISSION, NULL, 0);
                break;

            case MSG_LIST_USERS_REQ:
                if (current_user.role == ROLE_ADMIN)
                    handle_list_users(client_fd);
                else
                    send_msg(client_fd, MSG_ERROR, ERR_PERMISSION, NULL, 0);
                break;

            case MSG_CHANGE_ROLE_REQ:
                if (current_user.role == ROLE_ADMIN)
                    handle_change_role(client_fd, buf);
                else
                    send_msg(client_fd, MSG_ERROR, ERR_PERMISSION, NULL, 0);
                break;

            default:
                send_msg(client_fd, MSG_ERROR, ERR_INTERNAL, NULL, 0);
                break;
        }
    }

    /* Clean up: if still logged in (e.g. abrupt disconnect), clear session */
    if (authenticated && current_user.user_id != 0) {
        int slot = find_session_slot(current_user.user_id);
        if (slot >= 0) {
            Session empty = {0};
            write_session(slot, &empty);
        }
    }

    close(client_fd);
    printf("[server] closed fd=%d\n", client_fd);
}

/* ══════════════════════════════════════════════════════════
   WORKER THREAD ENTRY POINT
   ══════════════════════════════════════════════════════════ */
static void *worker_thread(void *arg) {
    (void)arg;   /* unused */

    /*
     * This loop is why the pool is efficient.
     * The thread never exits — it just loops back to
     * sem_wait() after each client, parking itself cheaply
     * until the next job arrives.
     */
    while (1) {
        int client_fd = dequeue_client();   /* BLOCKS here when idle */
        handle_client(client_fd);           /* serves one client     */
        /* loop → dequeue_client() → blocks again */
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════
   INIT  &  MAIN
   ══════════════════════════════════════════════════════════ */

static void init_work_queue(void) {
    memset(&wq, 0, sizeof(wq));
    pthread_mutex_init(&wq.lock, NULL);
    sem_init(&wq.available, 0, 0);  /* starts at 0: no jobs yet */
}

static void init_thread_pool(void) {
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&pool[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
        /*
         * pthread_detach: we don't need to join these threads.
         * They run forever (or until the process exits).
         * Detaching frees their resources automatically.
         */
        pthread_detach(pool[i]);
    }
    printf("[server] thread pool: %d workers ready\n", THREAD_POOL_SIZE);
}

static void ensure_data_dir(void) {
    mkdir("data", 0755);   /* ignore error if already exists */
}

/* Graceful shutdown on Ctrl-C */
static void handle_sigint(int sig) {
    (void)sig;
    printf("\n[server] shutting down...\n");
    if (server_fd >= 0) close(server_fd);
    exit(0);
}

int main(void) {
    signal(SIGINT,  handle_sigint);
    signal(SIGPIPE, SIG_IGN);  /* ignore broken pipe (client disconnect) */

    ensure_data_dir();

    /* ── 1. create the server socket ── */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    /* SO_REUSEADDR: lets us restart the server immediately after a crash
       without waiting for TIME_WAIT to expire */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* ── 2. bind to port ── */
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(PORT)
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(EXIT_FAILURE);
    }

    /* ── 3. listen ── */
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen"); exit(EXIT_FAILURE);
    }

    printf("[server] listening on port %d\n", PORT);

    /* ── 4. spin up thread pool ── */
    init_work_queue();
    init_thread_pool();

    /* ── 5. start the auction timer thread ── */
    start_auction_timer();

    /* ── 6. accept loop ── */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &addrlen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;  /* interrupted by signal, retry */
            perror("accept");
            continue;
        }

        printf("[server] new connection from %s fd=%d\n",
               inet_ntoa(client_addr.sin_addr), client_fd);

        enqueue_client(client_fd);
        /* main thread immediately loops back to accept() —
           the worker handles everything else              */
    }

    return 0;
}
