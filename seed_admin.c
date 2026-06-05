/*
 * seed_admin.c
 * Run once after first build to create the initial admin account.
 * Usage: gcc -o seed_admin seed_admin.c && ./seed_admin
 *
 * Why needed?
 *   The system has no sign-up flow — only admins can create users.
 *   But you need at least one admin to create other admins.
 *   This chicken-and-egg problem is solved by this one-time seed.
 *   Real systems solve it the same way (Django's createsuperuser,
 *   Rails' db:seed, etc.)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../auction_system/common/models.h"

static void sha256_hex(const char *input, char out[65]) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "echo -n '%s' | openssl dgst -sha256 | awk '{print $2}'",
             input);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    fgets(out, 65, fp);
    pclose(fp);
    out[strcspn(out, "\n")] = '\0';
}

int main(void) {
    mkdir("data", 0755);

    User admin;
    memset(&admin, 0, sizeof(admin));
    admin.user_id        = 1;
    admin.role           = ROLE_ADMIN;
    admin.status         = ACCOUNT_ACTIVE;
    admin.wallet_balance = 0.0;
    admin.wallet_hold    = 0.0;
    admin.created_at     = time(NULL);
    admin.is_deleted     = 0;
    strncpy(admin.username,  "admin",         MAX_USERNAME - 1);
    strncpy(admin.full_name, "System Admin",  MAX_NAME - 1);
    sha256_hex("admin123", admin.password);

    int fd = open("data/users.dat",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    write(fd, &admin, sizeof(User));
    close(fd);

    printf("Admin created: username=admin password=admin123\n");
    printf("Login and use the Admin menu to create other users.\n");
    return 0;
}
