#include "../common/notify.h"
#include "../common/file_io.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

/*
 * ══════════════════════════════════════════════════════════
 * THE DESIGN PROBLEM
 * ══════════════════════════════════════════════════════════
 *
 * Normal flow:  client sends request → server sends response.
 *               The client's thread is driving.
 *
 * Notification: server needs to send UNSOLICITED data to a
 *               client that is currently blocked waiting for
 *               its own next request to type.
 *
 * Example:
 *   Bob's thread: blocked on recv_msg() waiting for Bob to type
 *   Alice's thread: just placed a higher bid → needs to tell Bob
 *
 * Solution: a registry of (user_id → client_fd).
 *   Alice's thread looks up Bob's fd → writes MSG_NOTIFY_OUTBID.
 *   Bob's recv_msg() loop receives it and displays it to Bob.
 *
 * The client handles this by checking the MessageType in its
 * receive loop — if it's a NOTIFY type, display it without
 * waiting for user input.
 *
 * ══════════════════════════════════════════════════════════
 * WHY write() ON A SOCKET IS SAFE FROM ANOTHER THREAD
 * ══════════════════════════════════════════════════════════
 *
 * A single write() call on a socket fd is atomic at the kernel
 * level for messages smaller than PIPE_BUF (~4096 bytes on most
 * systems). Our notification payloads are tiny (< 64 bytes).
 * Two threads writing to DIFFERENT fds is always safe.
 * Two threads writing to the SAME fd is NOT safe — but each
 * client has exactly one fd owned by exactly one worker thread,
 * so only one thread ever calls recv_msg() on any given fd.
 * The notification writer calls send_msg() (write-only) on that
 * fd from a different thread, which is safe for small writes.
 *
 * For production, you'd use a per-client outbound queue +
 * condition variable. For this project, direct write() is fine.
 * ══════════════════════════════════════════════════════════
 */

/* ── connection registry ─────────────────────────────────── */
#define MAX_REGISTRY 128

typedef struct {
    uint32_t user_id;
    int      client_fd;
    int      active;
} RegistryEntry;

static RegistryEntry  registry[MAX_REGISTRY];
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

/* ══════════════════════════════════════════════════════════
   notify_register(user_id, client_fd)
   Called by auth.c after a successful login.
   Adds the user→fd mapping to the registry.
   ══════════════════════════════════════════════════════════ */
void notify_register(uint32_t user_id, int client_fd) {
    pthread_mutex_lock(&registry_lock);

    /* find a free slot */
    for (int i = 0; i < MAX_REGISTRY; i++) {
        if (!registry[i].active) {
            registry[i].user_id   = user_id;
            registry[i].client_fd = client_fd;
            registry[i].active    = 1;
            break;
        }
    }

    pthread_mutex_unlock(&registry_lock);
}

/* ══════════════════════════════════════════════════════════
   notify_unregister(user_id)
   Called by auth.c on logout or disconnect.
   Removes the user from the registry so stale fds are
   never written to.
   ══════════════════════════════════════════════════════════ */
void notify_unregister(uint32_t user_id) {
    pthread_mutex_lock(&registry_lock);

    for (int i = 0; i < MAX_REGISTRY; i++) {
        if (registry[i].active && registry[i].user_id == user_id) {
            registry[i].active    = 0;
            registry[i].client_fd = -1;
            break;
        }
    }

    pthread_mutex_unlock(&registry_lock);
}

/* ──────────────────────────────────────────────────────────
   lookup_fd(user_id)
   Returns the client_fd for a logged-in user, or -1 if
   they are not currently connected.
   Called INSIDE registry_lock.
   ────────────────────────────────────────────────────────── */
static int lookup_fd(uint32_t user_id) {
    for (int i = 0; i < MAX_REGISTRY; i++) {
        if (registry[i].active && registry[i].user_id == user_id)
            return registry[i].client_fd;
    }
    return -1;
}

/* ──────────────────────────────────────────────────────────
   push_notification(user_id, type, payload, length)
   Core internal helper. Looks up the fd and sends the msg.
   If user is offline, silently drops it — they'll see the
   result when they next query their bids or balance.
   ────────────────────────────────────────────────────────── */
static void push_notification(uint32_t user_id,
                               MessageType type,
                               const void *payload,
                               uint32_t length) {
    pthread_mutex_lock(&registry_lock);
    int fd = lookup_fd(user_id);
    pthread_mutex_unlock(&registry_lock);

    /*
     * We release the lock BEFORE calling send_msg().
     * Why? send_msg() calls write(), which can block if the
     * kernel send buffer is full. Holding the registry lock
     * while blocking would prevent ALL other notifications
     * from being sent or registered — a serious bottleneck.
     *
     * Race risk: fd could become invalid between lookup and write.
     * We handle this by ignoring EBADF and EPIPE errors — if
     * the client disconnected in that tiny window, the write
     * fails silently and the session cleanup in server.c will
     * unregister them.
     */
    if (fd < 0) {
        /* user is offline — notification dropped, not a problem */
        return;
    }

    MessageHeader hdr = {
        .type   = type,
        .length = length,
        .status = ERR_OK
    };

    /* write header */
    if (write(fd, &hdr, sizeof(hdr)) < 0) {
        if (errno != EPIPE && errno != EBADF)
            perror("[notify] write header");
        return;
    }

    /* write payload */
    if (length > 0 && payload != NULL) {
        if (write(fd, payload, length) < 0) {
            if (errno != EPIPE && errno != EBADF)
                perror("[notify] write payload");
        }
    }
}

/* ══════════════════════════════════════════════════════════
   NOTIFICATION PAYLOADS
   Small fixed structs — one per notification type.
   The client reads the MessageType, then reads exactly
   sizeof(the right struct) bytes as the payload.
   ══════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t auction_id;
    double   new_highest_bid;   /* the amount that beat yours */
} OutbidPayload;

typedef struct {
    uint32_t auction_id;
    double   winning_amount;
} AuctionWonPayload;

typedef struct {
    uint32_t auction_id;
} AuctionClosedPayload;

/* ══════════════════════════════════════════════════════════
   PUBLIC API
   ══════════════════════════════════════════════════════════ */

/*
 * notify_outbid(user_id, auction_id, new_bid)
 * Tells the previous highest bidder they've been beaten.
 * Called from bid_handler.c immediately after a new bid lands.
 */
void notify_outbid(uint32_t user_id,
                   uint32_t auction_id,
                   double   new_highest_bid) {
    OutbidPayload p = {
        .auction_id      = auction_id,
        .new_highest_bid = new_highest_bid
    };
    push_notification(user_id, MSG_NOTIFY_OUTBID, &p, sizeof(p));

    printf("[notify] outbid → user %u on auction #%u (new bid: %.2f)\n",
           user_id, auction_id, new_highest_bid);
}

/*
 * notify_auction_won(user_id, auction_id, amount)
 * Tells the winner they've won and what they paid.
 * Called from auction_engine.c during settlement.
 */
void notify_auction_won(uint32_t user_id,
                        uint32_t auction_id,
                        double   winning_amount) {
    AuctionWonPayload p = {
        .auction_id     = auction_id,
        .winning_amount = winning_amount
    };
    push_notification(user_id, MSG_NOTIFY_AUCTION_WON, &p, sizeof(p));

    printf("[notify] auction won → user %u won auction #%u for %.2f\n",
           user_id, auction_id, winning_amount);
}

/*
 * notify_auction_closed(user_id, auction_id)
 * Tells the seller their auction has closed (win or no-win).
 * Called from auction_engine.c after any close.
 */
void notify_auction_closed(uint32_t user_id, uint32_t auction_id) {
    AuctionClosedPayload p = { .auction_id = auction_id };
    push_notification(user_id, MSG_NOTIFY_AUCTION_CLOSED, &p, sizeof(p));

    printf("[notify] auction closed → user %u auction #%u\n",
           user_id, auction_id);
}
