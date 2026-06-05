#ifndef AUCTION_ENGINE_H
#define AUCTION_ENGINE_H

#include "models.h"
#include "auth.h"   /* pulls in extern send_msg declaration */

/*
 * handle_create_auction — auctioneer creates a new listing
 * handle_close_auction  — auctioneer/admin closes it early
 * start_auction_timer   — called once from main() at startup;
 *                         spawns the background timer thread
 *                         that auto-closes expired auctions
 */
void handle_create_auction (int client_fd, const char *buf, User *seller);
void handle_close_auction  (int client_fd, const char *buf, User *requester);
void start_auction_timer   (void);

#endif /* AUCTION_ENGINE_H */