#ifndef FILE_IO_H
#define FILE_IO_H

#include "models.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

/* ─── file paths ─────────────────────────────────────────── */
#define FILE_USERS      "data/users.dat"
#define FILE_AUCTIONS   "data/auctions.dat"
#define FILE_BIDS       "data/bids.dat"
#define FILE_SESSIONS   "data/sessions.dat"
#define FILE_FEEDBACK   "data/feedback.dat"
#define FILE_DISPUTES   "data/disputes.dat"
#define FILE_TXN_LOG    "data/txn.log"

/* ─── open flags ─────────────────────────────────────────── */
#define OPEN_RW   (O_RDWR | O_CREAT)
#define OPEN_APP  (O_RDWR | O_CREAT | O_APPEND)
#define FILE_PERM 0644

/* ══════════════════════════════════════════════════════════
   PER-FILE MUTEXES
   fcntl() locks on macOS are per-process, not per-thread.
   Two threads in the same process locking the same file
   with fcntl causes deadlocks. We use pthread mutexes
   instead — one per file — for correct thread isolation.
   fcntl semantics are preserved for documentation purposes
   but the actual locking is done via pthread_mutex.
   ══════════════════════════════════════════════════════════ */
static pthread_mutex_t _users_mtx    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t _auctions_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t _bids_mtx     = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t _sessions_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t _disputes_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t _feedback_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t _txnlog_mtx   = PTHREAD_MUTEX_INITIALIZER;

/* ══════════════════════════════════════════════════════════
   USER  CRUD
   ══════════════════════════════════════════════════════════ */

static inline int write_user(const User *u) {
    pthread_mutex_lock(&_users_mtx);

    int fd = open(FILE_USERS, OPEN_RW, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_users_mtx); return -1; }

    off_t offset = (off_t)(u->user_id - 1) * sizeof(User);
    if (lseek(fd, offset, SEEK_SET) < 0) {
        close(fd); pthread_mutex_unlock(&_users_mtx); return -1;
    }

    ssize_t written = write(fd, u, sizeof(User));
    close(fd);

    pthread_mutex_unlock(&_users_mtx);
    return (written == sizeof(User)) ? 0 : -1;
}

static inline int read_user(uint32_t user_id, User *out) {
    pthread_mutex_lock(&_users_mtx);

    int fd = open(FILE_USERS, O_RDONLY, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_users_mtx); return -1; }

    off_t offset = (off_t)(user_id - 1) * sizeof(User);
    if (lseek(fd, offset, SEEK_SET) < 0) {
        close(fd); pthread_mutex_unlock(&_users_mtx); return -1;
    }

    ssize_t r = read(fd, out, sizeof(User));
    close(fd);

    pthread_mutex_unlock(&_users_mtx);
    return (r == sizeof(User)) ? 0 : -1;
}

static inline int find_user_by_username(const char *username, User *out) {
    pthread_mutex_lock(&_users_mtx);

    int fd = open(FILE_USERS, O_RDONLY, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_users_mtx); return -1; }

    User tmp;
    int found = -1;
    while (read(fd, &tmp, sizeof(User)) == sizeof(User)) {
        if (!tmp.is_deleted &&
            strncmp(tmp.username, username, MAX_USERNAME) == 0) {
            *out = tmp;
            found = 0;
            break;
        }
    }

    close(fd);
    pthread_mutex_unlock(&_users_mtx);
    return found;
}

static inline uint32_t next_user_id(void) {
    int fd = open(FILE_USERS, O_RDONLY, FILE_PERM);
    if (fd < 0) return 1;
    off_t size = lseek(fd, 0, SEEK_END);
    close(fd);
    if (size <= 0) return 1;
    return (uint32_t)(size / sizeof(User)) + 1;
}

/* ══════════════════════════════════════════════════════════
   AUCTION  CRUD
   ══════════════════════════════════════════════════════════ */

static inline int write_auction(const Auction *a) {
    pthread_mutex_lock(&_auctions_mtx);

    int fd = open(FILE_AUCTIONS, OPEN_RW, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_auctions_mtx); return -1; }

    off_t offset = (off_t)(a->auction_id - 1) * sizeof(Auction);
    lseek(fd, offset, SEEK_SET);
    ssize_t w = write(fd, a, sizeof(Auction));
    close(fd);

    pthread_mutex_unlock(&_auctions_mtx);
    return (w == sizeof(Auction)) ? 0 : -1;
}

static inline int read_auction(uint32_t auction_id, Auction *out) {
    pthread_mutex_lock(&_auctions_mtx);

    int fd = open(FILE_AUCTIONS, O_RDONLY, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_auctions_mtx); return -1; }

    off_t offset = (off_t)(auction_id - 1) * sizeof(Auction);
    lseek(fd, offset, SEEK_SET);
    ssize_t r = read(fd, out, sizeof(Auction));
    close(fd);

    pthread_mutex_unlock(&_auctions_mtx);
    return (r == sizeof(Auction)) ? 0 : -1;
}

static inline uint32_t next_auction_id(void) {
    int fd = open(FILE_AUCTIONS, O_RDONLY, FILE_PERM);
    if (fd < 0) return 1;
    off_t size = lseek(fd, 0, SEEK_END);
    close(fd);
    return (size <= 0) ? 1 : (uint32_t)(size / sizeof(Auction)) + 1;
}

/* ══════════════════════════════════════════════════════════
   BID  (append-only)
   ══════════════════════════════════════════════════════════ */

static inline int append_bid(const Bid *b) {
    pthread_mutex_lock(&_bids_mtx);

    int fd = open(FILE_BIDS, OPEN_APP, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_bids_mtx); return -1; }

    ssize_t w = write(fd, b, sizeof(Bid));
    close(fd);

    pthread_mutex_unlock(&_bids_mtx);
    return (w == sizeof(Bid)) ? 0 : -1;
}

static inline uint32_t next_bid_id(void) {
    int fd = open(FILE_BIDS, O_RDONLY, FILE_PERM);
    if (fd < 0) return 1;
    off_t size = lseek(fd, 0, SEEK_END);
    close(fd);
    return (size <= 0) ? 1 : (uint32_t)(size / sizeof(Bid)) + 1;
}

/* ══════════════════════════════════════════════════════════
   SESSION  CRUD
   ══════════════════════════════════════════════════════════ */

static inline int write_session(int slot, const Session *s) {
    pthread_mutex_lock(&_sessions_mtx);

    int fd = open(FILE_SESSIONS, OPEN_RW, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_sessions_mtx); return -1; }

    lseek(fd, (off_t)slot * sizeof(Session), SEEK_SET);
    ssize_t w = write(fd, s, sizeof(Session));
    close(fd);

    pthread_mutex_unlock(&_sessions_mtx);
    return (w == sizeof(Session)) ? 0 : -1;
}

static inline int find_session_slot(uint32_t user_id) {
    pthread_mutex_lock(&_sessions_mtx);

    int fd = open(FILE_SESSIONS, O_RDONLY, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_sessions_mtx); return -1; }

    Session s;
    int slot = 0;
    int found = -1;
    while (read(fd, &s, sizeof(Session)) == sizeof(Session)) {
        if (s.is_active && s.user_id == user_id) {
            found = slot;
            break;
        }
        slot++;
    }

    close(fd);
    pthread_mutex_unlock(&_sessions_mtx);
    return found;
}

static inline int find_free_slot(void) {
    pthread_mutex_lock(&_sessions_mtx);

    int fd = open(FILE_SESSIONS, OPEN_RW, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_sessions_mtx); return 0; }

    Session s;
    int slot = 0;
    int found = -1;
    while (slot < MAX_SESSIONS &&
           read(fd, &s, sizeof(Session)) == sizeof(Session)) {
        if (!s.is_active) {
            found = slot;
            break;
        }
        slot++;
    }
    close(fd);

    pthread_mutex_unlock(&_sessions_mtx);
    return (found >= 0) ? found : (slot < MAX_SESSIONS ? slot : -1);
}

/* ══════════════════════════════════════════════════════════
   TRANSACTION LOG
   ══════════════════════════════════════════════════════════ */
static inline void log_txn(const char *msg) {
    pthread_mutex_lock(&_txnlog_mtx);

    int fd = open(FILE_TXN_LOG, O_WRONLY | O_CREAT | O_APPEND, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_txnlog_mtx); return; }

    char buf[512];
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    int len = snprintf(buf, sizeof(buf), "[%s] %s\n", ts, msg);
    write(fd, buf, len);
    close(fd);

    pthread_mutex_unlock(&_txnlog_mtx);
}

/* ══════════════════════════════════════════════════════════
   FEEDBACK  (append-only)
   ══════════════════════════════════════════════════════════ */
static inline int append_feedback(const Feedback *f) {
    pthread_mutex_lock(&_feedback_mtx);

    int fd = open(FILE_FEEDBACK, OPEN_APP, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_feedback_mtx); return -1; }

    ssize_t w = write(fd, f, sizeof(Feedback));
    close(fd);

    pthread_mutex_unlock(&_feedback_mtx);
    return (w == sizeof(Feedback)) ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════
   DISPUTE  CRUD
   ══════════════════════════════════════════════════════════ */
static inline int write_dispute(const Dispute *d) {
    pthread_mutex_lock(&_disputes_mtx);

    int fd = open(FILE_DISPUTES, OPEN_RW, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_disputes_mtx); return -1; }

    off_t offset = (off_t)(d->dispute_id - 1) * sizeof(Dispute);
    lseek(fd, offset, SEEK_SET);
    ssize_t w = write(fd, d, sizeof(Dispute));
    close(fd);

    pthread_mutex_unlock(&_disputes_mtx);
    return (w == sizeof(Dispute)) ? 0 : -1;
}

static inline int read_dispute(uint32_t dispute_id, Dispute *out) {
    pthread_mutex_lock(&_disputes_mtx);

    int fd = open(FILE_DISPUTES, O_RDONLY, FILE_PERM);
    if (fd < 0) { pthread_mutex_unlock(&_disputes_mtx); return -1; }

    off_t offset = (off_t)(dispute_id - 1) * sizeof(Dispute);
    lseek(fd, offset, SEEK_SET);
    ssize_t r = read(fd, out, sizeof(Dispute));
    close(fd);

    pthread_mutex_unlock(&_disputes_mtx);
    return (r == sizeof(Dispute)) ? 0 : -1;
}

static inline uint32_t next_dispute_id(void) {
    int fd = open(FILE_DISPUTES, O_RDONLY, FILE_PERM);
    if (fd < 0) return 1;
    off_t size = lseek(fd, 0, SEEK_END);
    close(fd);
    return (size <= 0) ? 1 : (uint32_t)(size / sizeof(Dispute)) + 1;
}

/* ══════════════════════════════════════════════════════════
   KEPT FOR REFERENCE — fcntl helpers still available
   for any code that explicitly needs cross-process locking
   ══════════════════════════════════════════════════════════ */
static inline int lock_file_write(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
    fl.l_start = 0; fl.l_len = 0;
    return fcntl(fd, F_SETLKW, &fl);
}
static inline int lock_file_read(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_RDLCK; fl.l_whence = SEEK_SET;
    fl.l_start = 0; fl.l_len = 0;
    return fcntl(fd, F_SETLKW, &fl);
}
static inline int unlock_file(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK; fl.l_whence = SEEK_SET;
    fl.l_start = 0; fl.l_len = 0;
    return fcntl(fd, F_SETLK, &fl);
}

#endif /* FILE_IO_H */