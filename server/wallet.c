#include "wallet.h"
#include file_io.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

/*
 * WHY A PER-USER MUTEX ARRAY?
 *
 * Two threads could modify the same user's wallet simultaneously:
 *   Thread A (bid on auction #1): place_hold(user=5, 200)
 *   Thread B (bid on auction #2): place_hold(user=5, 300)
 *
 * Both read wallet_balance=1000, wallet_hold=0.
 * Both write wallet_hold=200 / wallet_hold=300.
 * One update is lost. User now has hold=300 but owes 500.
 *
 * Fix: one mutex per user. Threads bidding on DIFFERENT auctions
 * for DIFFERENT users run fully parallel. Only same-user ops serialize.
 *
 * We use user_id % WALLET_LOCK_BUCKETS to map any user to a bucket.
 * 64 buckets means at most 1/64 chance of two different users sharing
 * a lock — acceptable for a course project.
 * Production systems use one mutex per user (stored alongside the record).
 */
#define WALLET_LOCK_BUCKETS 64
static pthread_mutex_t wallet_locks[WALLET_LOCK_BUCKETS];
static int locks_initialized = 0;
static pthread_once_t locks_once = PTHREAD_ONCE_INIT;

static void init_wallet_locks(void) {
    for (int i = 0; i < WALLET_LOCK_BUCKETS; i++)
        pthread_mutex_init(&wallet_locks[i], NULL);
    locks_initialized = 1;
}

static pthread_mutex_t *get_wallet_lock(uint32_t user_id) {
    pthread_once(&locks_once, init_wallet_locks);
    return &wallet_locks[user_id % WALLET_LOCK_BUCKETS];
}

/* ──────────────────────────────────────────────────────────
   Internal helper: read user, apply a wallet delta, write back.
   All four public functions use this under their mutex.

   balance_delta: positive = credit, negative = debit
   hold_delta:    positive = freeze more, negative = unfreeze
   ────────────────────────────────────────────────────────── */
static int apply_wallet_delta(uint32_t user_id,
                               double balance_delta,
                               double hold_delta) {
    User u;
    if (read_user(user_id, &u) < 0) {
        fprintf(stderr, "[wallet] user %u not found\n", user_id);
        return -1;
    }

    double new_balance = u.wallet_balance + balance_delta;
    double new_hold    = u.wallet_hold    + hold_delta;

    /* sanity guards — should never trigger if logic is correct */
    if (new_balance < 0) {
        fprintf(stderr, "[wallet] ERROR: user %u balance would go negative "
                        "(%.2f + %.2f)\n", user_id, u.wallet_balance, balance_delta);
        return -1;
    }
    if (new_hold < 0) {
        /* hold can briefly go to 0 but not below */
        new_hold = 0;
    }
    if (new_hold > new_balance) {
        fprintf(stderr, "[wallet] ERROR: user %u hold (%.2f) > balance (%.2f)\n",
                user_id, new_hold, new_balance);
        return -1;
    }

    u.wallet_balance = new_balance;
    u.wallet_hold    = new_hold;

    return write_user(&u);
}

/* ══════════════════════════════════════════════════════════
   place_hold(user_id, amount)
   Called when a bidder wins the top spot.
   Freezes `amount` from their available balance.
   Their total balance stays the same; only hold increases.

   Available = balance - hold
   Before: balance=1000, hold=0,   available=1000
   After:  balance=1000, hold=500, available=500
   ══════════════════════════════════════════════════════════ */
int place_hold(uint32_t user_id, double amount) {
    pthread_mutex_t *lk = get_wallet_lock(user_id);
    pthread_mutex_lock(lk);

    /*
     * Re-read inside the lock to get the latest balance.
     * The optimistic check in bid_handler was a pre-flight;
     * this is the authoritative check.
     */
    User u;
    if (read_user(user_id, &u) < 0) {
        pthread_mutex_unlock(lk);
        return -1;
    }

    double available = u.wallet_balance - u.wallet_hold;
    if (amount > available) {
        fprintf(stderr, "[wallet] place_hold: user %u has %.2f available, "
                        "needs %.2f\n", user_id, available, amount);
        pthread_mutex_unlock(lk);
        return -1;
    }

    int result = apply_wallet_delta(user_id, 0.0, +amount);

    pthread_mutex_unlock(lk);

    char log[128];
    snprintf(log, sizeof(log), "HOLD user=%u amount=+%.2f", user_id, amount);
    log_txn(log);

    return result;
}

/* ══════════════════════════════════════════════════════════
   release_hold(user_id, amount)
   Called when a bidder is outbid or auction closes with no winner.
   Unfreezes their money — available balance goes back up.

   Before: balance=1000, hold=500, available=500
   After:  balance=1000, hold=0,   available=1000
   ══════════════════════════════════════════════════════════ */
int release_hold(uint32_t user_id, double amount) {
    pthread_mutex_t *lk = get_wallet_lock(user_id);
    pthread_mutex_lock(lk);

    int result = apply_wallet_delta(user_id, 0.0, -amount);

    pthread_mutex_unlock(lk);

    char log[128];
    snprintf(log, sizeof(log), "RELEASE user=%u amount=%.2f", user_id, amount);
    log_txn(log);

    return result;
}

/* ══════════════════════════════════════════════════════════
   settle_payment(user_id, amount)
   Called on auction winner at close time.
   Converts the hold into an actual deduction.
   Both balance AND hold decrease by amount.

   Before: balance=1000, hold=500, available=500
   After:  balance=500,  hold=0,   available=500
                                   ↑ available unchanged!
   The bidder already "felt" this money was gone (it was on hold).
   Settlement just makes it official.
   ══════════════════════════════════════════════════════════ */
int settle_payment(uint32_t user_id, double amount) {
    pthread_mutex_t *lk = get_wallet_lock(user_id);
    pthread_mutex_lock(lk);

    /* deduct balance AND release hold simultaneously */
    int result = apply_wallet_delta(user_id, -amount, -amount);

    pthread_mutex_unlock(lk);

    char log[128];
    snprintf(log, sizeof(log), "SETTLE user=%u amount=%.2f", user_id, amount);
    log_txn(log);

    return result;
}

/* ══════════════════════════════════════════════════════════
   credit_wallet(user_id, amount)
   Called on the seller when their auction closes successfully.
   Pure balance increase, no hold involved.

   Before: balance=200, hold=0
   After:  balance=700, hold=0   (sold item for 500)
   ══════════════════════════════════════════════════════════ */
int credit_wallet(uint32_t user_id, double amount) {
    pthread_mutex_t *lk = get_wallet_lock(user_id);
    pthread_mutex_lock(lk);

    int result = apply_wallet_delta(user_id, +amount, 0.0);

    pthread_mutex_unlock(lk);

    char log[128];
    snprintf(log, sizeof(log), "CREDIT user=%u amount=+%.2f", user_id, amount);
    log_txn(log);

    return result;
}

/* ══════════════════════════════════════════════════════════
   handle_deposit — client adds money to their wallet
   Called from server dispatch (MSG_DEPOSIT_REQ)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    double amount;
} DepositPayload;

void handle_deposit(int client_fd, const char *buf, User *u) {
    DepositPayload *req = (DepositPayload *)buf;

    if (req->amount <= 0) {
        send_msg(client_fd, MSG_DEPOSIT_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }

    if (credit_wallet(u->user_id, req->amount) < 0) {
        send_msg(client_fd, MSG_DEPOSIT_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }

    /* send back updated user record so client can display new balance */
    User updated;
    read_user(u->user_id, &updated);
    *u = updated;   /* keep server-side copy in sync */

    send_msg(client_fd, MSG_DEPOSIT_RESP, ERR_OK, &updated, sizeof(User));
}

/* ══════════════════════════════════════════════════════════
   handle_view_balance — read and return current balance
   ══════════════════════════════════════════════════════════ */
void handle_view_balance(int client_fd, User *u) {
    /*
     * Always re-read from disk — the in-memory User struct
     * in handle_client() could be stale if another thread
     * settled an auction for this user.
     */
    User fresh;
    if (read_user(u->user_id, &fresh) < 0) {
        send_msg(client_fd, MSG_VIEW_BALANCE_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }

    /* send only the wallet fields, but sending the full struct is fine too */
    send_msg(client_fd, MSG_VIEW_BALANCE_RESP, ERR_OK,
             &fresh, sizeof(User));
}
