#include "../server/auction_engine.h"
#include "../common/wallet.h"
#include "../common/notify.h"
#include "../common/file_io.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* forward declaration */
static void close_auction_internal(Auction *a);

/* ══════════════════════════════════════════════════════════
   CREATE AUCTION
   ══════════════════════════════════════════════════════════ */
typedef struct {
    char     item_name[MAX_ITEM_NAME];
    char     description[MAX_DESC];
    double   start_price;
    double   reserve_price;
    uint32_t duration_seconds;
} CreateAuctionPayload;

void handle_create_auction(int client_fd, const char *buf, User *seller) {
    CreateAuctionPayload *req = (CreateAuctionPayload *)buf;

    if (req->start_price <= 0 || req->duration_seconds < 10) {
        send_msg(client_fd, MSG_CREATE_AUCTION_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }

    time_t now = time(NULL);
    Auction a;
    memset(&a, 0, sizeof(a));
    a.auction_id        = next_auction_id();
    a.seller_id         = seller->user_id;
    a.start_price       = req->start_price;
    a.reserve_price     = req->reserve_price;
    a.current_bid       = req->start_price;
    a.highest_bidder_id = 0;
    a.status            = AUCTION_OPEN;
    a.start_time        = now;
    a.end_time          = now + req->duration_seconds;
    a.bid_count         = 0;
    a.is_deleted        = 0;
    strncpy(a.item_name,   req->item_name,   MAX_ITEM_NAME - 1);
    strncpy(a.description, req->description, MAX_DESC - 1);

    if (write_auction(&a) < 0) {
        send_msg(client_fd, MSG_CREATE_AUCTION_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }

    send_msg(client_fd, MSG_CREATE_AUCTION_RESP, ERR_OK, &a, sizeof(Auction));

    char log[256];
    snprintf(log, sizeof(log),
             "CREATE_AUCTION id=%u seller=%u item='%s' end=%ld",
             a.auction_id, seller->user_id, a.item_name, (long)a.end_time);
    log_txn(log);
    printf("[auction] created #%u '%s' closes at %s",
           a.auction_id, a.item_name, ctime(&a.end_time));
}

/* ══════════════════════════════════════════════════════════
   CLOSE AUCTION  (manual)
   ══════════════════════════════════════════════════════════ */
typedef struct { uint32_t auction_id; } CloseAuctionPayload;

void handle_close_auction(int client_fd, const char *buf, User *requester) {
    CloseAuctionPayload *req = (CloseAuctionPayload *)buf;
    Auction a;
    if (read_auction(req->auction_id, &a) < 0) {
        send_msg(client_fd, MSG_CLOSE_AUCTION_RESP, ERR_NOT_FOUND, NULL, 0);
        return;
    }
    if (a.seller_id != requester->user_id && requester->role != ROLE_ADMIN) {
        send_msg(client_fd, MSG_CLOSE_AUCTION_RESP, ERR_PERMISSION, NULL, 0);
        return;
    }
    if (a.status != AUCTION_OPEN) {
        send_msg(client_fd, MSG_CLOSE_AUCTION_RESP, ERR_AUCTION_CLOSED, NULL, 0);
        return;
    }
    close_auction_internal(&a);
    send_msg(client_fd, MSG_CLOSE_AUCTION_RESP, ERR_OK, &a, sizeof(Auction));

    char log[128];
    snprintf(log, sizeof(log), "CLOSE_AUCTION id=%u by user=%u",
             a.auction_id, requester->user_id);
    log_txn(log);
}

/* ══════════════════════════════════════════════════════════
   CLOSE AUCTION  (internal — settlement logic)

   ACID breakdown for this operation:
   A (Atomic)   — WAL entry written BEFORE wallet changes.
                  If we crash mid-way, log shows what to undo.
   C (Consistent) — auction moves CLOSED, winner is debited,
                    seller is credited. No money created or lost.
   I (Isolated) — auction semaphore (from bid_handler) ensures
                  no bids land while we're settling.
   D (Durable)  — every wallet change is write() to disk,
                  not just in-memory.
   ══════════════════════════════════════════════════════════ */
static void close_auction_internal(Auction *a) {
    a->status = AUCTION_CLOSED;
    write_auction(a);

    int reserve_met = (a->reserve_price == 0 ||
                       a->current_bid >= a->reserve_price);

    if (a->highest_bidder_id != 0 && reserve_met) {
        /* Write-ahead log — written BEFORE modifying wallets */
        char wal[256];
        snprintf(wal, sizeof(wal),
                 "WAL_SETTLE auction=%u winner=%u seller=%u amount=%.2f",
                 a->auction_id, a->highest_bidder_id,
                 a->seller_id, a->current_bid);
        log_txn(wal);

        /* debit winner, credit seller */
        settle_payment(a->highest_bidder_id, a->current_bid);
        credit_wallet(a->seller_id, a->current_bid);

        /* mark winning bid in bids.dat */
        int bfd = open(FILE_BIDS, O_RDWR, FILE_PERM);
        if (bfd >= 0) {
            lock_file_write(bfd);
            Bid b;
            off_t offset = 0;
            while (read(bfd, &b, sizeof(Bid)) == sizeof(Bid)) {
                if (b.auction_id == a->auction_id &&
                    b.bidder_id  == a->highest_bidder_id) {
                    b.is_winning = 1;
                    lseek(bfd, offset, SEEK_SET);
                    write(bfd, &b, sizeof(Bid));
                }
                offset += sizeof(Bid);
            }
            unlock_file(bfd);
            close(bfd);
        }

        notify_auction_won(a->highest_bidder_id, a->auction_id, a->current_bid);
        notify_auction_closed(a->seller_id, a->auction_id);
        printf("[auction] #%u settled: winner=user%u amount=%.2f\n",
               a->auction_id, a->highest_bidder_id, a->current_bid);

    } else {
        /* no winner or reserve not met — unfreeze funds */
        if (a->highest_bidder_id != 0)
            release_hold(a->highest_bidder_id, a->current_bid);
        notify_auction_closed(a->seller_id, a->auction_id);
        printf("[auction] #%u closed: no valid winner\n", a->auction_id);
    }
}

/* ══════════════════════════════════════════════════════════
   TIMER THREAD
   Wakes every second, scans for expired auctions, closes them.

   Why NOT alarm()/SIGALRM?
     SIGALRM is process-wide and interrupts blocking syscalls
     (read, accept) with EINTR in OTHER threads too. That forces
     every single syscall in the program to handle EINTR.
     A dedicated thread with sleep(1) is completely isolated —
     it never affects other threads.
   ══════════════════════════════════════════════════════════ */
static void *timer_thread(void *arg) {
    (void)arg;
    printf("[timer] started\n");

    while (1) {
        sleep(1);
        time_t now = time(NULL);

        int fd = open(FILE_AUCTIONS, O_RDONLY, FILE_PERM);
        if (fd < 0) continue;

        lock_file_read(fd);

        /*
         * Collect expired auction IDs under read lock,
         * then close them AFTER releasing the read lock.
         *
         * Why not close inside the read lock?
         * close_auction_internal() calls write_auction() which
         * tries to acquire a WRITE lock on the same file.
         * Upgrading read→write on the same fd = DEADLOCK.
         */
        uint32_t expired[64];
        int count = 0;
        Auction a;
        while (read(fd, &a, sizeof(Auction)) == sizeof(Auction) && count < 64) {
            if (a.status == AUCTION_OPEN && a.end_time <= now)
                expired[count++] = a.auction_id;
        }

        unlock_file(fd);
        close(fd);

        for (int i = 0; i < count; i++) {
            Auction ea;
            if (read_auction(expired[i], &ea) == 0 && ea.status == AUCTION_OPEN) {
                printf("[timer] auto-closing auction #%u\n", expired[i]);
                close_auction_internal(&ea);
                char log[64];
                snprintf(log, sizeof(log), "TIMER_CLOSE auction_id=%u", expired[i]);
                log_txn(log);
            }
        }
    }
    return NULL;
}

void start_auction_timer(void) {
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, timer_thread, NULL) != 0) {
        perror("[timer] pthread_create failed");
        exit(EXIT_FAILURE);
    }
    pthread_attr_destroy(&attr);
    printf("[timer] auction timer thread launched\n");
}
