#ifndef BID_HANDLER_H
#define BID_HANDLER_H

#include "models.h"
#include "auth.h"   /* pulls in extern send_msg declaration */

/*
 * handle_place_bid       — bidder places a bid (critical section)
 * handle_view_auctions   — list all open auctions (all roles)
 * handle_view_my_bids    — bidder sees their own bid history
 * handle_submit_feedback — bidder submits feedback on an auction
 */
void handle_place_bid       (int client_fd, const char *buf, User *bidder);
void handle_view_auctions   (int client_fd);
void handle_view_my_bids    (int client_fd, User *u);
void handle_submit_feedback (int client_fd, const char *buf, User *u);

#endif /* BID_HANDLER_H */