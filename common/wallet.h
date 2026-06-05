#ifndef WALLET_H
#define WALLET_H

#include "../common/models.h"
#include "server.h"

/* core wallet operations — called internally by other modules */
int  place_hold    (uint32_t user_id, double amount);
int  release_hold  (uint32_t user_id, double amount);
int  settle_payment(uint32_t user_id, double amount);
int  credit_wallet (uint32_t user_id, double amount);

/* called from server dispatch */
void handle_deposit      (int client_fd, const char *buf, User *u);
void handle_view_balance (int client_fd, User *u);

#endif
