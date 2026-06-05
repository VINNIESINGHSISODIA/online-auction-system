#include "auth.h"
#include file_io.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

/*
 * WHY A MUTEX HERE?
 * Two threads could simultaneously:
 *   - check "is user already logged in?" → both see NO
 *   - both create a session                → user logged in TWICE
 * The auth_lock prevents that window.
 * This is a classic TOCTOU (Time-Of-Check-Time-Of-Use) race.
 */
static pthread_mutex_t auth_lock = PTHREAD_MUTEX_INITIALIZER;

/* ──────────────────────────────────────────────────────────
   sha256_hex(input, out64)
   A minimal SHA-256 wrapper.
   In a real project use OpenSSL's SHA256(). For this course
   project we call the system `openssl` binary via popen()
   to keep dependencies zero.
   ────────────────────────────────────────────────────────── */
static void sha256_hex(const char *input, char out[65]) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "echo -n '%s' | openssl dgst -sha256 | awk '{print $2}'",
             input);
    FILE *fp = popen(cmd, "r");
    if (!fp) { strncpy(out, "0000000000000000000000000000000000000000000000000000000000000000", 64); return; }
    fgets(out, 65, fp);
    pclose(fp);
    /* strip trailing newline */
    out[strcspn(out, "\n")] = '\0';
}

/* ══════════════════════════════════════════════════════════
   LOGIN REQUEST PAYLOAD  (what the client sends us)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];   /* plaintext from client */
} LoginPayload;

/* ══════════════════════════════════════════════════════════
   handle_login(client_fd, buf, out_user)
   Returns 1 if login succeeded (authenticated=true),
           0 if login failed.
   ══════════════════════════════════════════════════════════ */
int handle_login(int client_fd, const char *buf, User *out_user) {
    LoginPayload *req = (LoginPayload *)buf;

    /* ── Step 1: find the user record ── */
    User u;
    if (find_user_by_username(req->username, &u) < 0) {
        send_msg(client_fd, MSG_LOGIN_RESP, ERR_AUTH_FAIL, NULL, 0);
        return 0;
    }

    /* ── Step 2: check account status ── */
    if (u.status == ACCOUNT_BANNED) {
        send_msg(client_fd, MSG_LOGIN_RESP, ERR_ACCOUNT_BANNED, NULL, 0);
        return 0;
    }
    if (u.status == ACCOUNT_INACTIVE) {
        send_msg(client_fd, MSG_LOGIN_RESP, ERR_ACCOUNT_BANNED, NULL, 0);
        return 0;
    }

    /* ── Step 3: verify password (hash and compare) ── */
    char hashed[65] = {0};
    sha256_hex(req->password, hashed);
    if (strncmp(hashed, u.password, 64) != 0) {
        send_msg(client_fd, MSG_LOGIN_RESP, ERR_AUTH_FAIL, NULL, 0);
        return 0;
    }

    /*
     * ── Step 4: one-session-per-user check ──
     * We lock here to make the CHECK + CREATE atomic.
     * Without the lock, two simultaneous logins could
     * both pass the check and both succeed.
     */
    pthread_mutex_lock(&auth_lock);

    int existing = find_session_slot(u.user_id);
    if (existing >= 0) {
        pthread_mutex_unlock(&auth_lock);
        send_msg(client_fd, MSG_LOGIN_RESP, ERR_ALREADY_LOGGED, NULL, 0);
        return 0;
    }

    /* ── Step 5: create session ── */
    int slot = find_free_slot();
    if (slot < 0) {
        pthread_mutex_unlock(&auth_lock);
        send_msg(client_fd, MSG_LOGIN_RESP, ERR_INTERNAL, NULL, 0);
        return 0;
    }

    Session s = {
        .user_id    = u.user_id,
        .client_fd  = client_fd,
        .login_time = time(NULL),
        .is_active  = 1
    };
    write_session(slot, &s);

    pthread_mutex_unlock(&auth_lock);

    /* ── Step 6: update last_login in users.dat ── */
    u.last_login = time(NULL);
    write_user(&u);

    /* ── Step 7: reply and log ── */
    send_msg(client_fd, MSG_LOGIN_RESP, ERR_OK, &u, sizeof(User));

    char log[128];
    snprintf(log, sizeof(log), "LOGIN user_id=%u username=%s",
             u.user_id, u.username);
    log_txn(log);

    *out_user = u;
    printf("[auth] user '%s' (role=%d) logged in on fd=%d\n",
           u.username, u.role, client_fd);
    return 1;
}

/* ══════════════════════════════════════════════════════════
   handle_logout
   ══════════════════════════════════════════════════════════ */
void handle_logout(int client_fd, User *u) {
    pthread_mutex_lock(&auth_lock);

    int slot = find_session_slot(u->user_id);
    if (slot >= 0) {
        Session empty = {0};
        write_session(slot, &empty);
    }

    pthread_mutex_unlock(&auth_lock);

    send_msg(client_fd, MSG_LOGOUT_RESP, ERR_OK, NULL, 0);

    char log[128];
    snprintf(log, sizeof(log), "LOGOUT user_id=%u", u->user_id);
    log_txn(log);

    printf("[auth] user '%s' logged out\n", u->username);
}

/* ══════════════════════════════════════════════════════════
   handle_change_password
   ══════════════════════════════════════════════════════════ */
typedef struct {
    char old_password[MAX_PASSWORD];
    char new_password[MAX_PASSWORD];
} ChangePassPayload;

void handle_change_password(int client_fd, const char *buf, User *u) {
    ChangePassPayload *req = (ChangePassPayload *)buf;

    /* verify old password */
    char hashed_old[65] = {0};
    sha256_hex(req->old_password, hashed_old);
    if (strncmp(hashed_old, u->password, 64) != 0) {
        send_msg(client_fd, MSG_CHANGE_PASS_RESP, ERR_AUTH_FAIL, NULL, 0);
        return;
    }

    /* hash and store new password */
    char hashed_new[65] = {0};
    sha256_hex(req->new_password, hashed_new);
    strncpy(u->password, hashed_new, MAX_PASSWORD);
    write_user(u);

    send_msg(client_fd, MSG_CHANGE_PASS_RESP, ERR_OK, NULL, 0);

    char log[128];
    snprintf(log, sizeof(log), "CHANGE_PASS user_id=%u", u->user_id);
    log_txn(log);
}
