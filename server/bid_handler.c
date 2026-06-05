#include "bid_handler.h"
#include "wallet.h"
#include "notify.h"
#include "../common/file_io.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>

/*
 * ══════════════════════════════════════════════════════════
 * THE CORE CONCURRENCY PROBLEM — read this carefully
 * ══════════════════════════════════════════════════════════
 *
 * Alice and Bob both want to bid on Auction #5 at the same time.
 *
 * WITHOUT locking:
 *   Thread A reads auction: current_bid = ₹500
 *   Thread B reads auction: current_bid = ₹500
 *   Thread A checks ₹600 > ₹500 → TRUE, proceeds
 *   Thread B checks ₹600 > ₹500 → TRUE, proceeds  ← WRONG
 *   Thread A writes current_bid = ₹600
 *   Thread B writes current_bid = ₹600
 *   Result: two winners, two wallets debited. Bank loses money.
 *
 * WITH a per-auction semaphore (binary = mutex):
 *   Thread A acquires sem for Auction #5
 *   Thread B tries to acquire → BLOCKS
 *   Thread A reads, checks, writes, releases sem
 *   Thread B unblocks, reads UPDATED value, its bid may now lose
 *
 * WHY PER-AUCTION and not one global lock?
 *   A global lock would serialize ALL bids on ALL auctions.
 *   If Auction #1 and Auction #2 are unrelated, there's no reason
 *   a bid on #1 should block a bid on #2. Per-auction semaphores
 *   give us maximum parallelism with correct isolation.
 * ══════════════════════════════════════════════════════════
 */

#define MAX_AUCTIONS 1024   /* max concurrent auctions we track */

/* Array of binary semaphores — one per auction slot.
   Initialized lazily on first use.                    */
static sem_t  auction_sem[MAX_AUCTIONS];
static int    sem_initialized[MAX_AUCTIONS] = {0};
static pthread_mutex_t sem_init_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * get_auction_sem(auction_id)
 * Returns pointer to the semaphore for this auction.
 * Initializes it the first time it's accessed (lazy init).
 */
static sem_t *get_auction_sem(uint32_t auction_id) {
    uint32_t idx = (auction_id - 1) % MAX_AUCTIONS;

    pthread_mutex_lock(&sem_init_lock);
    if (!sem_initialized[idx]) {
        sem_init(&auction_sem[idx], 0, 1); /* binary semaphore, starts at 1 */
        sem_initialized[idx] = 1;
    }
    pthread_mutex_unlock(&sem_init_lock);

    return &auction_sem[idx];
}

/* ══════════════════════════════════════════════════════════
   BID REQUEST PAYLOAD
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t auction_id;
    double   amount;
} PlaceBidPayload;

/* ══════════════════════════════════════════════════════════
   handle_place_bid — the critical section
   ══════════════════════════════════════════════════════════ */
void handle_place_bid(int client_fd, const char *buf, User *bidder) {
    PlaceBidPayload *req = (PlaceBidPayload *)buf;

    /* ── Step 1: read the auction (no lock needed yet, just a peek) ── */
    Auction a;
    if (read_auction(req->auction_id, &a) < 0) {
        send_msg(client_fd, MSG_PLACE_BID_RESP, ERR_NOT_FOUND, NULL, 0);
        return;
    }
    if (a.status != AUCTION_OPEN) {
        send_msg(client_fd, MSG_PLACE_BID_RESP, ERR_AUCTION_CLOSED, NULL, 0);
        return;
    }

    /* ── Step 2: check wallet (optimistic, before acquiring lock) ── */
    /* Re-read the bidder's latest balance from disk */
    User fresh_bidder;
    if (read_user(bidder->user_id, &fresh_bidder) < 0) {
        send_msg(client_fd, MSG_PLACE_BID_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }
    double available = fresh_bidder.wallet_balance - fresh_bidder.wallet_hold;
    if (req->amount > available) {
        send_msg(client_fd, MSG_PLACE_BID_RESP, ERR_INSUFFICIENT, NULL, 0);
        return;
    }

    /*
     * ── Step 3: ACQUIRE the per-auction semaphore ──
     *
     * From this point until sem_post(), we are the ONLY thread
     * allowed to modify Auction #req->auction_id.
     * All other bid attempts on this auction will block here.
     */
    sem_t *sem = get_auction_sem(req->auction_id);
    sem_wait(sem);   /* ← CRITICAL SECTION BEGINS */

    /* Re-read inside the lock — state may have changed while we waited */
    if (read_auction(req->auction_id, &a) < 0) {
        sem_post(sem);
        send_msg(client_fd, MSG_PLACE_BID_RESP, ERR_NOT_FOUND, NULL, 0);
        return;
    }

    /* Check again inside the lock */
    if (a.status != AUCTION_OPEN) {
        sem_post(sem);
        send_msg(client_fd, MSG_PLACE_BID_RESP, ERR_AUCTION_CLOSED, NULL, 0);
        return;
    }

    /* Bid must beat current highest */
    if (req->amount <= a.current_bid) {
        sem_post(sem);
        send_msg(client_fd, MSG_PLACE_BID_RESP, ERR_BID_TOO_LOW, NULL, 0);
        return;
    }

    /*
     * ── Step 4: update wallet holds ──
     * Release the previous highest bidder's held funds,
     * then hold funds for the new highest bidder.
     * Both wallet operations happen inside the auction lock
     * so the auction and wallet stay consistent with each other.
     */
    uint32_t prev_winner = a.highest_bidder_id;
    double   prev_bid    = a.current_bid;

    if (prev_winner != 0) {
        release_hold(prev_winner, prev_bid);   /* unfreeze loser's money */
    }
    place_hold(bidder->user_id, req->amount);  /* freeze winner's money  */

    /* ── Step 5: update auction record ── */
    a.current_bid        = req->amount;
    a.highest_bidder_id  = bidder->user_id;
    a.bid_count++;
    write_auction(&a);   /* this internally locks auctions.dat */

    /* ── Step 6: record the bid ── */
    Bid b = {
        .bid_id     = next_bid_id(),
        .auction_id = req->auction_id,
        .bidder_id  = bidder->user_id,
        .amount     = req->amount,
        .placed_at  = time(NULL),
        .is_winning = 1
    };
    append_bid(&b);

    sem_post(sem);   /* ← CRITICAL SECTION ENDS */

    /* ── Step 7: respond and notify outbid user ── */
    send_msg(client_fd, MSG_PLACE_BID_RESP, ERR_OK, &b, sizeof(Bid));

    char log[256];
    snprintf(log, sizeof(log),
             "BID auction_id=%u bidder_id=%u amount=%.2f",
             req->auction_id, bidder->user_id, req->amount);
    log_txn(log);

    /* notify the previous highest bidder they've been outbid */
    if (prev_winner != 0 && prev_winner != bidder->user_id) {
        notify_outbid(prev_winner, req->auction_id, req->amount);
    }
}

/* ══════════════════════════════════════════════════════════
   handle_view_auctions — list all open auctions
   ══════════════════════════════════════════════════════════ */
void handle_view_auctions(int client_fd) {
    int fd = open(FILE_AUCTIONS, O_RDONLY, FILE_PERM);
    if (fd < 0) {
        send_msg(client_fd, MSG_VIEW_AUCTIONS_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }

    lock_file_read(fd);

    Auction a;
    /* We'll accumulate open auctions into a stack buffer */
    Auction open_list[256];
    int count = 0;

    while (read(fd, &a, sizeof(Auction)) == sizeof(Auction) &&
           count < 256) {
        if (!a.is_deleted && a.status == AUCTION_OPEN) {
            open_list[count++] = a;
        }
    }

    unlock_file(fd);
    close(fd);

    send_msg(client_fd, MSG_VIEW_AUCTIONS_RESP, ERR_OK,
             open_list, (uint32_t)(count * sizeof(Auction)));
}

/* ══════════════════════════════════════════════════════════
   handle_view_my_bids
   ══════════════════════════════════════════════════════════ */
void handle_view_my_bids(int client_fd, User *u) {
    int fd = open(FILE_BIDS, O_RDONLY, FILE_PERM);
    if (fd < 0) {
        send_msg(client_fd, MSG_VIEW_MY_BIDS_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }

    lock_file_read(fd);

    Bid bids[256];
    int count = 0;
    Bid b;
    while (read(fd, &b, sizeof(Bid)) == sizeof(Bid) && count < 256) {
        if (b.bidder_id == u->user_id)
            bids[count++] = b;
    }

    unlock_file(fd);
    close(fd);

    send_msg(client_fd, MSG_VIEW_MY_BIDS_RESP, ERR_OK,
             bids, (uint32_t)(count * sizeof(Bid)));
}

/* ══════════════════════════════════════════════════════════
   handle_submit_feedback
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t auction_id;
    char     message[MAX_FEEDBACK];
} FeedbackPayload;

void handle_submit_feedback(int client_fd, const char *buf, User *u) {
    FeedbackPayload *req = (FeedbackPayload *)buf;

    Feedback f = {
        .feedback_id      = 0,          /* auto via file size */
        .from_user_id     = u->user_id,
        .about_auction_id = req->auction_id,
        .submitted_at     = time(NULL),
        .reviewed         = 0
    };
    strncpy(f.message, req->message, MAX_FEEDBACK - 1);

    int fd = open(FILE_FEEDBACK, O_RDONLY, FILE_PERM);
    if (fd >= 0) {
        off_t sz = lseek(fd, 0, SEEK_END);
        close(fd);
        f.feedback_id = (uint32_t)(sz / sizeof(Feedback)) + 1;
    }

    if (append_feedback(&f) < 0) {
        send_msg(client_fd, MSG_SUBMIT_FEEDBACK_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }
    send_msg(client_fd, MSG_SUBMIT_FEEDBACK_RESP, ERR_OK, NULL, 0);
}
