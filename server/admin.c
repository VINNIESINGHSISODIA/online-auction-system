#include "admin.h"
#include "../common/file_io.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ──────────────────────────────────────────────────────────
   sha256_hex — same helper used in auth.c
   In a real project this would live in a shared utils.c.
   ────────────────────────────────────────────────────────── */
static void sha256_hex(const char *input, char out[65]) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "echo -n '%s' | openssl dgst -sha256 | awk '{print $2}'",
             input);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        strncpy(out,
            "0000000000000000000000000000000000000000000000000000000000000000",
            64);
        return;
    }
    fgets(out, 65, fp);
    pclose(fp);
    out[strcspn(out, "\n")] = '\0';
}

/* ══════════════════════════════════════════════════════════
   ADD NEW USER  (admin creates any role)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];   /* plaintext — we hash it here */
    char full_name[MAX_NAME];
    Role role;
} AddUserPayload;

void handle_add_user(int client_fd, const char *buf) {
    AddUserPayload *req = (AddUserPayload *)buf;

    /* check username not already taken */
    int ufd = open(FILE_USERS, O_RDONLY, FILE_PERM);
    if (ufd >= 0) {
        User tmp;
        while (read(ufd, &tmp, sizeof(User)) == sizeof(User)) {
            if (!tmp.is_deleted &&
                strncmp(tmp.username, req->username, MAX_USERNAME) == 0) {
                close(ufd);
                send_msg(client_fd, MSG_ADD_USER_RESP, ERR_INTERNAL, NULL, 0);
                return;
            }
        }
        close(ufd);
    }   

    User u;
    memset(&u, 0, sizeof(u));
    u.user_id       = next_user_id();
    u.role          = req->role;
    u.status        = ACCOUNT_ACTIVE;
    u.wallet_balance = 0.0;
    u.wallet_hold    = 0.0;
    u.created_at    = time(NULL);
    u.last_login    = 0;
    u.is_deleted    = 0;
    strncpy(u.username,  req->username,  MAX_USERNAME - 1);
    strncpy(u.full_name, req->full_name, MAX_NAME - 1);

    /* hash the password before storing */
    sha256_hex(req->password, u.password);

    if (write_user(&u) < 0) {
        send_msg(client_fd, MSG_ADD_USER_RESP, ERR_INTERNAL, NULL, 0);
        return;
    }

    /* don't send password hash back to client */
    memset(u.password, 0, sizeof(u.password));
    send_msg(client_fd, MSG_ADD_USER_RESP, ERR_OK, &u, sizeof(User));

    char log[128];
    snprintf(log, sizeof(log),
             "ADD_USER id=%u username=%s role=%d",
             u.user_id, u.username, u.role);
    log_txn(log);

    printf("[admin] created user '%s' (id=%u role=%d)\n",
           u.username, u.user_id, u.role);
}

/* ══════════════════════════════════════════════════════════
   LIST ALL USERS  (admin views everyone)
   ══════════════════════════════════════════════════════════ */
void handle_list_users(int client_fd) {
    int fd = open(FILE_USERS, O_RDONLY, FILE_PERM);
    if (fd < 0) {
        send_msg(client_fd, MSG_LIST_USERS_RESP, ERR_OK, NULL, 0);
        return;
    }

    User list[512];
    int  count = 0;
    User u;
    while (read(fd, &u, sizeof(User)) == sizeof(User) && count < 512) {
        if (!u.is_deleted) {
            memset(u.password, 0, sizeof(u.password));
            list[count++] = u;
        }
    }
    close(fd);

    send_msg(client_fd, MSG_LIST_USERS_RESP, ERR_OK,
             list, (uint32_t)(count * sizeof(User)));
}

/* ══════════════════════════════════════════════════════════
   CHANGE ROLE  (admin promotes/demotes a user)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t target_user_id;
    Role     new_role;
} ChangeRolePayload;

void handle_change_role(int client_fd, const char *buf) {
    ChangeRolePayload *req = (ChangeRolePayload *)buf;

    User u;
    if (read_user(req->target_user_id, &u) < 0) {
        send_msg(client_fd, MSG_CHANGE_ROLE_RESP, ERR_NOT_FOUND, NULL, 0);
        return;
    }

    /*
     * Safety guard: don't allow demoting the last admin.
     * Scan users.dat to count active admins first.
     */
    if (u.role == ROLE_ADMIN && req->new_role != ROLE_ADMIN) {
        int fd = open(FILE_USERS, O_RDONLY, FILE_PERM);
        int admin_count = 0;
        if (fd >= 0) {
            lock_file_read(fd);
            User tmp;
            while (read(fd, &tmp, sizeof(User)) == sizeof(User)) {
                if (!tmp.is_deleted &&
                    tmp.status == ACCOUNT_ACTIVE &&
                    tmp.role == ROLE_ADMIN)
                    admin_count++;
            }
            unlock_file(fd);
            close(fd);
        }
        if (admin_count <= 1) {
            /* refusing — would lock everyone out */
            send_msg(client_fd, MSG_CHANGE_ROLE_RESP, ERR_PERMISSION, NULL, 0);
            return;
        }
    }

    Role old_role = u.role;
    u.role = req->new_role;
    write_user(&u);

    memset(u.password, 0, sizeof(u.password));
    send_msg(client_fd, MSG_CHANGE_ROLE_RESP, ERR_OK, &u, sizeof(User));

    char log[128];
    snprintf(log, sizeof(log),
             "CHANGE_ROLE user=%u old=%d new=%d",
             u.user_id, old_role, u.role);
    log_txn(log);
}

/* ══════════════════════════════════════════════════════════
   SOFT DELETE USER

   We never physically remove a record because other files
   (bids.dat, auctions.dat) contain foreign keys pointing to
   this user_id. Deleting the record would make those
   references unresolvable — you'd lose the entire audit trail.

   Instead, is_deleted=1 hides them from all queries.
   The record stays on disk forever for audit purposes.
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t target_user_id;
} DeleteUserPayload;

void handle_delete_user(int client_fd, const char *buf) {
    DeleteUserPayload *req = (DeleteUserPayload *)buf;

    User u;
    if (read_user(req->target_user_id, &u) < 0) {
        send_msg(client_fd, MSG_ADD_USER_RESP, ERR_NOT_FOUND, NULL, 0);
        return;
    }
    if (u.role == ROLE_ADMIN) {
        /* never soft-delete an admin — too dangerous */
        send_msg(client_fd, MSG_ADD_USER_RESP, ERR_PERMISSION, NULL, 0);
        return;
    }

    u.is_deleted = 1;
    u.status     = ACCOUNT_INACTIVE;
    write_user(&u);

    send_msg(client_fd, MSG_ADD_USER_RESP, ERR_OK, NULL, 0);

    char log[128];
    snprintf(log, sizeof(log), "SOFT_DELETE user=%u", u.user_id);
    log_txn(log);
}

/* ══════════════════════════════════════════════════════════
   MODIFY USER DETAILS  (admin edits name, email, etc.)
   Does NOT touch password or role — use dedicated endpoints.
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t target_user_id;
    char     new_full_name[MAX_NAME];
} ModifyUserPayload;

void handle_modify_user(int client_fd, const char *buf) {
    ModifyUserPayload *req = (ModifyUserPayload *)buf;

    User u;
    if (read_user(req->target_user_id, &u) < 0) {
        send_msg(client_fd, MSG_ADD_USER_RESP, ERR_NOT_FOUND, NULL, 0);
        return;
    }

    strncpy(u.full_name, req->new_full_name, MAX_NAME - 1);
    write_user(&u);

    memset(u.password, 0, sizeof(u.password));
    send_msg(client_fd, MSG_ADD_USER_RESP, ERR_OK, &u, sizeof(User));

    char log[128];
    snprintf(log, sizeof(log), "MODIFY_USER user=%u", u.user_id);
    log_txn(log);
}
