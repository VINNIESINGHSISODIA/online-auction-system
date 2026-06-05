#ifndef DISPUTE_H
#define DISPUTE_H

#include "../common/models.h"

/* bidder */
void handle_file_dispute(int client_fd,
                         const char *buf,
                         User *u);

/* moderator */
void handle_view_disputes(int client_fd);

void handle_resolve_dispute(int client_fd,
                            const char *buf,
                            User *moderator);

void handle_view_feedback(int client_fd);

void handle_toggle_account(int client_fd,
                           const char *buf,
                           User *moderator);

#endif /* DISPUTE_H */