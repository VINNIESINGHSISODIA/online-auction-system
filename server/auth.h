#ifndef AUTH_H
#define AUTH_H

#include "models.h"

/*
 * send_msg is defined in server.c but called by all modules.
 * Declaring it extern here means every file that includes
 * auth.h (which most do) gets the declaration for free.
 */
extern int send_msg(int fd, MessageType type, int status,
                    const void *payload, uint32_t length);

/* called from server.c dispatch loop */
int  handle_login           (int client_fd, const char *buf, User *out_user);
void handle_logout          (int client_fd, User *u);
void handle_change_password (int client_fd, const char *buf, User *u);

#endif /* AUTH_H */