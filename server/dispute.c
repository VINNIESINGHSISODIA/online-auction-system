#include "dispute.h"
#include "wallet.h"
#include file_io.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ══════════════════════════════════════════════════════════
   FILE A DISPUTE
   Called by a bidder who believes something went wrong
   with an auction (shill bidding, item not delivered, etc.)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t auction_id;
    char     reason[MAX_DESC];
} FileDisputePayload;

void handle_file_dispute(int client_fd, const char *buf, User *u) {
    FileDisputePayload *req = (FileDisputePayload *)buf;

    /* verify auction exists */
    Auction a;
    if (read_auction(req->auction_id, &a) < 0) {
        send_msg(client_fd, MSG_RESOLVE_DISPUTE_RESP, ERR_NOT_FOUND, NULL, 0);
        return;
    }

    /*
     * Only allow disputes on CLOSED auctions.
     * You can't dispute something still in progress —
     * wait for it to settle first.
     */
    if (a.status != AUCTION_CLOSED) {
        send_msg(client_fd, MSG_RESOLVE_DISPUTE_RESP, ERR_AUCTION_CLOSED, NULL, 0);
        return;
    }

    /* build the dispute record */
    Dispute d;
    memset(&d, 0, sizeof(d));
    d.dispute_id          = next_dispute_id();
    d.auction_id          = req->auction_id;
    d.raised_by_user_id   = u->user_id;
    d.resolved_by_user_id = 0;        /* nobody yet          */
    d.status              = DISPUTE_OPEN;
    d.raised_at           = time(NULL);
    d.resolved_at         = 0;
    strncpy(d.reason, req->reason, MAX_DESC - 1);

    if (write_dispute(&d) < 0) {
        send_msg(client_fd, MSG_RESOLVE_DISPUTE_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }

    /*
     * Mark the auction as DISPUTED so it stands out
     * in the moderator's queue and bidders can see its status.
     */
    a.status = AUCTION_DISPUTED;
    write_auction(&a);

    send_msg(client_fd, MSG_RESOLVE_DISPUTE_RESP, ERR_OK, &d, sizeof(Dispute));

    char log[256];
    snprintf(log, sizeof(log),
             "FILE_DISPUTE dispute_id=%u auction=%u by_user=%u",
             d.dispute_id, d.auction_id, u->user_id);
    log_txn(log);

    printf("[dispute] dispute #%u filed for auction #%u by user %u\n",
           d.dispute_id, d.auction_id, u->user_id);
}

/* ══════════════════════════════════════════════════════════
   VIEW DISPUTES  (moderator sees all open disputes)
   ══════════════════════════════════════════════════════════ */
void handle_view_disputes(int client_fd) {
    int fd = open(FILE_DISPUTES, O_RDONLY, FILE_PERM);
    if (fd < 0) {
        /* no disputes file yet = no disputes */
        send_msg(client_fd, MSG_VIEW_DISPUTES_RESP, ERR_OK, NULL, 0);
        return;
    }

    lock_file_read(fd);

    Dispute list[128];
    int count = 0;
    Dispute d;

    while (read(fd, &d, sizeof(Dispute)) == sizeof(Dispute) && count < 128) {
        if (d.status == DISPUTE_OPEN)
            list[count++] = d;
    }

    unlock_file(fd);
    close(fd);

    send_msg(client_fd, MSG_VIEW_DISPUTES_RESP, ERR_OK,
             list, (uint32_t)(count * sizeof(Dispute)));
}

/* ══════════════════════════════════════════════════════════
   RESOLVE DISPUTE  (moderator accepts or rejects)

   Two outcomes:
   RESOLVED → refund the winner (they didn't get the item)
              release their payment back to their wallet
   REJECTED → dispute was baseless, auction stands
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t      dispute_id;
    DisputeStatus new_status;          /* RESOLVED or REJECTED */
    char          resolution[MAX_DESC];
} ResolveDisputePayload;

void handle_resolve_dispute(int client_fd, const char *buf, User *moderator) {
    ResolveDisputePayload *req = (ResolveDisputePayload *)buf;

    if (req->new_status != DISPUTE_RESOLVED &&
        req->new_status != DISPUTE_REJECTED) {
        send_msg(client_fd, MSG_RESOLVE_DISPUTE_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }

    Dispute d;
    if (read_dispute(req->dispute_id, &d) < 0) {
        send_msg(client_fd, MSG_RESOLVE_DISPUTE_RESP, ERR_NOT_FOUND, NULL, 0);
        return;
    }
    if (d.status != DISPUTE_OPEN) {
        send_msg(client_fd, MSG_RESOLVE_DISPUTE_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }

    /* update dispute record */
    d.status              = req->new_status;
    d.resolved_by_user_id = moderator->user_id;
    d.resolved_at         = time(NULL);
    strncpy(d.resolution, req->resolution, MAX_DESC - 1);
    write_dispute(&d);

    /* fetch the auction to find out who won and how much */
    Auction a;
    if (read_auction(d.auction_id, &a) < 0) {
        send_msg(client_fd, MSG_RESOLVE_DISPUTE_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }

    if (req->new_status == DISPUTE_RESOLVED) {
        /*
         * REFUND FLOW:
         * The winner already paid (settle_payment was called at close).
         * We now credit their wallet back and debit the seller.
         *
         * Write-ahead log BEFORE touching wallets — same ACID
         * pattern as auction settlement.
         */
        char wal[256];
        snprintf(wal, sizeof(wal),
                 "WAL_REFUND dispute=%u auction=%u winner=%u seller=%u amount=%.2f",
                 d.dispute_id, a.auction_id,
                 a.highest_bidder_id, a.seller_id, a.current_bid);
        log_txn(wal);

        /* refund winner */
        credit_wallet(a.highest_bidder_id, a.current_bid);

        /*
         * Debit seller — they must return the money.
         * We check they have enough; if not, we flag it
         * (in a real system this would escalate, but for
         * this project we proceed and log the anomaly).
         */
        User seller;
        if (read_user(a.seller_id, &seller) == 0) {
            if (seller.wallet_balance >= a.current_bid) {
                /* apply_wallet_delta via a negative credit */
                credit_wallet(a.seller_id, -a.current_bid);
            } else {
                fprintf(stderr,
                        "[dispute] seller %u has insufficient balance for refund\n",
                        a.seller_id);
                log_txn("REFUND_ANOMALY seller has insufficient balance");
            }
        }

        /* mark auction status back to DISPUTED (already is, leave it) */
        printf("[dispute] dispute #%u RESOLVED — refund issued to user %u\n",
               d.dispute_id, a.highest_bidder_id);

    } else {
        /* REJECTED — auction result stands, restore to CLOSED */
        a.status = AUCTION_CLOSED;
        write_auction(&a);
        printf("[dispute] dispute #%u REJECTED — auction result stands\n",
               d.dispute_id);
    }

    send_msg(client_fd, MSG_RESOLVE_DISPUTE_RESP, ERR_OK, &d, sizeof(Dispute));

    char log[128];
    snprintf(log, sizeof(log),
             "RESOLVE_DISPUTE id=%u status=%d by_mod=%u",
             d.dispute_id, d.status, moderator->user_id);
    log_txn(log);
}

/* ══════════════════════════════════════════════════════════
   VIEW FEEDBACK  (moderator sees all unreviewed feedback)
   ══════════════════════════════════════════════════════════ */
void handle_view_feedback(int client_fd) {
    int fd = open(FILE_FEEDBACK, O_RDONLY, FILE_PERM);
    if (fd < 0) {
        send_msg(client_fd, MSG_VIEW_FEEDBACK_RESP, ERR_OK, NULL, 0);
        return;
    }

    lock_file_read(fd);

    Feedback list[256];
    int count = 0;
    Feedback f;

    while (read(fd, &f, sizeof(Feedback)) == sizeof(Feedback) && count < 256) {
        if (!f.reviewed)
            list[count++] = f;
    }

    unlock_file(fd);
    close(fd);

    /*
     * Mark all returned feedback as reviewed.
     * We do a second pass with a write lock.
     * Why two passes? We don't want to hold a write lock
     * while building the response — keep critical sections short.
     */
    fd = open(FILE_FEEDBACK, O_RDWR, FILE_PERM);
    if (fd >= 0) {
        lock_file_write(fd);
        Feedback tmp;
        off_t offset = 0;
        while (read(fd, &tmp, sizeof(Feedback)) == sizeof(Feedback)) {
            if (!tmp.reviewed) {
                tmp.reviewed = 1;
                lseek(fd, offset, SEEK_SET);
                write(fd, &tmp, sizeof(Feedback));
            }
            offset += sizeof(Feedback);
        }
        unlock_file(fd);
        close(fd);
    }

    send_msg(client_fd, MSG_VIEW_FEEDBACK_RESP, ERR_OK,
             list, (uint32_t)(count * sizeof(Feedback)));
}

/* ══════════════════════════════════════════════════════════
   TOGGLE ACCOUNT  (moderator activates or deactivates a user)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t      target_user_id;
    AccountStatus new_status;     /* ACTIVE or INACTIVE or BANNED */
} ToggleAccountPayload;

void handle_toggle_account(int client_fd, const char *buf, User *moderator) {
    ToggleAccountPayload *req = (ToggleAccountPayload *)buf;

    /* moderators can't ban other moderators or admins */
    User target;
    if (read_user(req->target_user_id, &target) < 0) {
        send_msg(client_fd, MSG_TOGGLE_ACCOUNT_RESP, ERR_NOT_FOUND, NULL, 0);
        return;
    }
    if (target.role >= moderator->role) {
        send_msg(client_fd, MSG_TOGGLE_ACCOUNT_RESP, ERR_PERMISSION, NULL, 0);
        return;
    }

    target.status = req->new_status;
    write_user(&target);

    send_msg(client_fd, MSG_TOGGLE_ACCOUNT_RESP, ERR_OK,
             &target, sizeof(User));

    char log[128];
    snprintf(log, sizeof(log),
             "TOGGLE_ACCOUNT target=%u new_status=%d by_mod=%u",
             target.user_id, target.status, moderator->user_id);
    log_txn(log);
}
