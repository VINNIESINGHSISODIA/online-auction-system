#ifndef NOTIFY_H
#define NOTIFY_H

#include "../common/models.h"

/* registry — called by auth.c */
void notify_register   (uint32_t user_id, int client_fd);
void notify_unregister (uint32_t user_id);

/* push events — called by bid_handler.c and auction_engine.c */
void notify_outbid          (uint32_t user_id, uint32_t auction_id, double new_highest_bid);
void notify_auction_won     (uint32_t user_id, uint32_t auction_id, double winning_amount);
void notify_auction_closed  (uint32_t user_id, uint32_t auction_id);

#endif