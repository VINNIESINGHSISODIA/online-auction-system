#ifndef FILE_IO_H
#define FILE_IO_H

#include "models.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

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
   LOCKING HELPERS
   We use fcntl() POSIX record locks — not flock().
   Why fcntl over flock?
     - flock locks the whole file; fcntl can lock byte ranges
     - fcntl locks are released on close() even if you forgot
     - Required by the project spec (system calls)
   ══════════════════════════════════════════════════════════ */

/*
 * lock_file_write(fd)
 *   Blocks until an exclusive (write) lock is acquired on the
 *   entire file. Other readers AND writers block until we unlock.
 */
static inline int lock_file_write(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = F_WRLCK;   /* exclusive write lock  */
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;          /* 0 = lock entire file  */
    return fcntl(fd, F_SETLKW, &fl);  /* W = wait (block)     */
}

/*
 * lock_file_read(fd)
 *   Shared (read) lock — multiple readers can hold it at once,
 *   but blocks if a write lock is held.
 */
static inline int lock_file_read(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;
    return fcntl(fd, F_SETLKW, &fl);
}

/*
 * unlock_file(fd)
 *   Release whatever lock we hold.
 */
static inline int unlock_file(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;
    return fcntl(fd, F_SETLK, &fl);
}

/* ══════════════════════════════════════════════════════════
   USER  CRUD
   Records are fixed-size → seek directly by (id-1)*sizeof(User)
   ══════════════════════════════════════════════════════════ */

/*
 * write_user(u)
 *   Writes user u at offset (u->user_id - 1) * sizeof(User).
 *   Creates the file if it doesn't exist.
 *   Returns 0 on success, -1 on error.
 *
 *   LOCKING: caller must hold write lock on users.dat OR
 *   we grab it internally here (we do it internally).
 */
static inline int write_user(const User *u) {
    int fd = open(FILE_USERS, OPEN_RW, FILE_PERM);
    if (fd < 0) return -1;

    lock_file_write(fd);

    off_t offset = (off_t)(u->user_id - 1) * sizeof(User);
    if (lseek(fd, offset, SEEK_SET) < 0) {
        unlock_file(fd); close(fd); return -1;
    }

    ssize_t written = write(fd, u, sizeof(User));

    unlock_file(fd);
    close(fd);
    return (written == sizeof(User)) ? 0 : -1;
}

/*
 * read_user(user_id, out)
 *   Reads the User record with the given id into *out.
 *   Returns 0 on success, -1 if not found or error.
 */
static inline int read_user(uint32_t user_id, User *out) {
    int fd = open(FILE_USERS, O_RDONLY, FILE_PERM);
    if (fd < 0) return -1;

    lock_file_read(fd);

    off_t offset = (off_t)(user_id - 1) * sizeof(User);
    if (lseek(fd, offset, SEEK_SET) < 0) {
        unlock_file(fd); close(fd); return -1;
    }

    ssize_t r = read(fd, out, sizeof(User));

    unlock_file(fd);
    close(fd);
    return (r == sizeof(User)) ? 0 : -1;
}

/*
 * find_user_by_username(username, out)
 *   Linear scan — we read every record until we find a match.
 *   For a course project with <1000 users this is fine.
 *   Returns 0 if found, -1 if not found.
 */
static inline int find_user_by_username(const char *username, User *out) {
    int fd = open(FILE_USERS, O_RDONLY, FILE_PERM);
    if (fd < 0) return -1;

    lock_file_read(fd);

    User tmp;
    while (read(fd, &tmp, sizeof(User)) == sizeof(User)) {
        if (!tmp.is_deleted &&
            strncmp(tmp.username, username, MAX_USERNAME) == 0) {
            *out = tmp;
            unlock_file(fd);
            close(fd);
            return 0;
        }
    }

    unlock_file(fd);
    close(fd);
    return -1;
}

/*
 * next_user_id()
 *   Counts how many User records exist and returns count+1.
 *   Called when creating a new user.
 */
static inline uint32_t next_user_id(void) {
    int fd = open(FILE_USERS, O_RDONLY, FILE_PERM);
    if (fd < 0) return 1;

    off_t size = lseek(fd, 0, SEEK_END);
    close(fd);

    if (size <= 0) return 1;
    return (uint32_t)(size / sizeof(User)) + 1;
}

/* ══════════════════════════════════════════════════════════
   AUCTION  CRUD  (same fixed-size record pattern)
   ══════════════════════════════════════════════════════════ */

static inline int write_auction(const Auction *a) {
    int fd = open(FILE_AUCTIONS, OPEN_RW, FILE_PERM);
    if (fd < 0) return -1;

    lock_file_write(fd);

    off_t offset = (off_t)(a->auction_id - 1) * sizeof(Auction);
    lseek(fd, offset, SEEK_SET);
    ssize_t w = write(fd, a, sizeof(Auction));

    unlock_file(fd);
    close(fd);
    return (w == sizeof(Auction)) ? 0 : -1;
}

static inline int read_auction(uint32_t auction_id, Auction *out) {
    int fd = open(FILE_AUCTIONS, O_RDONLY, FILE_PERM);
    if (fd < 0) return -1;

    lock_file_read(fd);

    off_t offset = (off_t)(auction_id - 1) * sizeof(Auction);
    lseek(fd, offset, SEEK_SET);
    ssize_t r = read(fd, out, sizeof(Auction));

    unlock_file(fd);
    close(fd);
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
   BID  (append-only — we never update old bids)
   ══════════════════════════════════════════════════════════ */

static inline int append_bid(const Bid *b) {
    /* O_APPEND makes the write+lseek atomic at kernel level */
    int fd = open(FILE_BIDS, OPEN_APP, FILE_PERM);
    if (fd < 0) return -1;

    lock_file_write(fd);
    ssize_t w = write(fd, b, sizeof(Bid));
    unlock_file(fd);
    close(fd);
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
   Fixed array of MAX_SESSIONS slots.
   Slot index = position in file (slot 0 at offset 0, etc.)
   ══════════════════════════════════════════════════════════ */

static inline int write_session(int slot, const Session *s) {
    int fd = open(FILE_SESSIONS, OPEN_RW, FILE_PERM);
    if (fd < 0) return -1;

    lock_file_write(fd);
    lseek(fd, (off_t)slot * sizeof(Session), SEEK_SET);
    ssize_t w = write(fd, s, sizeof(Session));
    unlock_file(fd);
    close(fd);
    return (w == sizeof(Session)) ? 0 : -1;
}

/*
 * find_session_slot(user_id)
 *   Returns slot index if user has an active session, -1 otherwise.
 */
static inline int find_session_slot(uint32_t user_id) {
    int fd = open(FILE_SESSIONS, O_RDONLY, FILE_PERM);
    if (fd < 0) return -1;

    lock_file_read(fd);

    Session s;
    int slot = 0;
    while (read(fd, &s, sizeof(Session)) == sizeof(Session)) {
        if (s.is_active && s.user_id == user_id) {
            unlock_file(fd);
            close(fd);
            return slot;
        }
        slot++;
    }

    unlock_file(fd);
    close(fd);
    return -1;
}

/*
 * find_free_slot()
 *   Returns first inactive slot, or -1 if all full.
 */
static inline int find_free_slot(void) {
    int fd = open(FILE_SESSIONS, OPEN_RW, FILE_PERM);
    if (fd < 0) return 0;  /* fresh file → use slot 0 */

    lock_file_read(fd);

    Session s;
    int slot = 0;
    while (slot < MAX_SESSIONS &&
           read(fd, &s, sizeof(Session)) == sizeof(Session)) {
        if (!s.is_active) {
            unlock_file(fd);
            close(fd);
            return slot;
        }
        slot++;
    }

    unlock_file(fd);
    close(fd);
    return (slot < MAX_SESSIONS) ? slot : -1;
}

/* ══════════════════════════════════════════════════════════
   TRANSACTION LOG  (append-only text log for auditability)
   ══════════════════════════════════════════════════════════ */
static inline void log_txn(const char *msg) {
    int fd = open(FILE_TXN_LOG, O_WRONLY | O_CREAT | O_APPEND, FILE_PERM);
    if (fd < 0) return;

    /* O_APPEND + single write() is atomic for small writes on Linux/macOS */
    char buf[512];
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    int len = snprintf(buf, sizeof(buf), "[%s] %s\n", ts, msg);
    write(fd, buf, len);
    close(fd);
}

/* ══════════════════════════════════════════════════════════
   FEEDBACK  (append-only)
   ══════════════════════════════════════════════════════════ */
static inline int append_feedback(const Feedback *f) {
    int fd = open(FILE_FEEDBACK, OPEN_APP, FILE_PERM);
    if (fd < 0) return -1;
    lock_file_write(fd);
    ssize_t w = write(fd, f, sizeof(Feedback));
    unlock_file(fd);
    close(fd);
    return (w == sizeof(Feedback)) ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════
   DISPUTE  CRUD
   ══════════════════════════════════════════════════════════ */
static inline int write_dispute(const Dispute *d) {
    int fd = open(FILE_DISPUTES, OPEN_RW, FILE_PERM);
    if (fd < 0) return -1;
    lock_file_write(fd);
    off_t offset = (off_t)(d->dispute_id - 1) * sizeof(Dispute);
    lseek(fd, offset, SEEK_SET);
    ssize_t w = write(fd, d, sizeof(Dispute));
    unlock_file(fd);
    close(fd);
    return (w == sizeof(Dispute)) ? 0 : -1;
}

static inline int read_dispute(uint32_t dispute_id, Dispute *out) {
    int fd = open(FILE_DISPUTES, O_RDONLY, FILE_PERM);
    if (fd < 0) return -1;
    lock_file_read(fd);
    off_t offset = (off_t)(dispute_id - 1) * sizeof(Dispute);
    lseek(fd, offset, SEEK_SET);
    ssize_t r = read(fd, out, sizeof(Dispute));
    unlock_file(fd);
    close(fd);
    return (r == sizeof(Dispute)) ? 0 : -1;
}

static inline uint32_t next_dispute_id(void) {
    int fd = open(FILE_DISPUTES, O_RDONLY, FILE_PERM);
    if (fd < 0) return 1;
    off_t size = lseek(fd, 0, SEEK_END);
    close(fd);
    return (size <= 0) ? 1 : (uint32_t)(size / sizeof(Dispute)) + 1;
}

#endif /* FILE_IO_H */
