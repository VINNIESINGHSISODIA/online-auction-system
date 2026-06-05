#ifndef MODELS_H
#define MODELS_H

#include <time.h>
#include <stdint.h>

/* ─── sizes ──────────────────────────────────────────────── */
#define MAX_USERNAME    32
#define MAX_PASSWORD    64   /* stored as SHA-256 hex = 64 chars */
#define MAX_NAME        64
#define MAX_DESC       256
#define MAX_ITEM_NAME   64
#define MAX_FEEDBACK   256
#define MAX_SESSIONS    64   /* max simultaneous logins */

/* ─── roles ──────────────────────────────────────────────── */
typedef enum {
    ROLE_BIDDER     = 0,
    ROLE_AUCTIONEER = 1,
    ROLE_MODERATOR  = 2,
    ROLE_ADMIN      = 3
} Role;

/* ─── account status ─────────────────────────────────────── */
typedef enum {
    ACCOUNT_ACTIVE   = 0,
    ACCOUNT_INACTIVE = 1,
    ACCOUNT_BANNED   = 2
} AccountStatus;

/* ─── auction status ─────────────────────────────────────── */
typedef enum {
    AUCTION_PENDING  = 0,   /* created, not yet open */
    AUCTION_OPEN     = 1,   /* accepting bids        */
    AUCTION_CLOSED   = 2,   /* ended normally        */
    AUCTION_DISPUTED = 3,   /* under review          */
    AUCTION_CANCELLED= 4
} AuctionStatus;

/* ─── dispute status ─────────────────────────────────────── */
typedef enum {
    DISPUTE_OPEN     = 0,
    DISPUTE_RESOLVED = 1,
    DISPUTE_REJECTED = 2
} DisputeStatus;

/* ══════════════════════════════════════════════════════════
   USER  (written to users.dat as fixed-size records)
   record size = sizeof(User)  →  seek by user_id * sizeof(User)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t      user_id;                  /* primary key, 1-based      */
    char          username[MAX_USERNAME];
    char          password[MAX_PASSWORD];   /* SHA-256 hex string        */
    char          full_name[MAX_NAME];
    Role          role;
    AccountStatus status;
    double        wallet_balance;           /* only meaningful for bidders */
    double        wallet_hold;              /* funds locked in active bids */
    time_t        created_at;
    time_t        last_login;
    int           is_deleted;              /* soft delete flag           */
} User;

/* ══════════════════════════════════════════════════════════
   AUCTION  (written to auctions.dat)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t      auction_id;
    char          item_name[MAX_ITEM_NAME];
    char          description[MAX_DESC];
    uint32_t      seller_id;               /* FK → User.user_id          */
    double        start_price;
    double        reserve_price;           /* hidden floor; 0 = no floor */
    double        current_bid;             /* highest bid so far         */
    uint32_t      highest_bidder_id;       /* 0 = no bids yet            */
    AuctionStatus status;
    time_t        start_time;
    time_t        end_time;               /* auction closes at this time */
    int           bid_count;
    int           is_deleted;
} Auction;

/* ══════════════════════════════════════════════════════════
   BID  (appended to bids.dat — append-only log)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t  bid_id;
    uint32_t  auction_id;              /* FK → Auction */
    uint32_t  bidder_id;               /* FK → User    */
    double    amount;
    time_t    placed_at;
    int       is_winning;              /* updated when auction closes  */
} Bid;

/* ══════════════════════════════════════════════════════════
   SESSION  (written to sessions.dat — one slot per user)
   Enforces "one active session per user" rule
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t  user_id;
    int       client_fd;               /* socket fd of the live session */
    time_t    login_time;
    int       is_active;              /* 0 = slot free, 1 = logged in  */
} Session;

/* ══════════════════════════════════════════════════════════
   FEEDBACK  (appended to feedback.dat)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t  feedback_id;
    uint32_t  from_user_id;
    uint32_t  about_auction_id;
    char      message[MAX_FEEDBACK];
    time_t    submitted_at;
    int       reviewed;               /* 0 = pending, 1 = seen by mod  */
} Feedback;

/* ══════════════════════════════════════════════════════════
   DISPUTE  (written to disputes.dat)
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t      dispute_id;
    uint32_t      auction_id;
    uint32_t      raised_by_user_id;
    uint32_t      resolved_by_user_id;  /* 0 = unresolved              */
    char          reason[MAX_DESC];
    char          resolution[MAX_DESC];
    DisputeStatus status;
    time_t        raised_at;
    time_t        resolved_at;
} Dispute;

/* ══════════════════════════════════════════════════════════
   CLIENT ↔ SERVER MESSAGE PROTOCOL
   Every message over the socket has this header, followed
   by a payload of `length` bytes.
   ══════════════════════════════════════════════════════════ */
typedef enum {
    /* auth */
    MSG_LOGIN_REQ = 1, MSG_LOGIN_RESP,
    MSG_LOGOUT_REQ, MSG_LOGOUT_RESP,
    MSG_CHANGE_PASS_REQ, MSG_CHANGE_PASS_RESP,

    /* bidder */
    MSG_VIEW_AUCTIONS_REQ, MSG_VIEW_AUCTIONS_RESP,
    MSG_PLACE_BID_REQ, MSG_PLACE_BID_RESP,
    MSG_VIEW_BALANCE_REQ, MSG_VIEW_BALANCE_RESP,
    MSG_DEPOSIT_REQ, MSG_DEPOSIT_RESP,
    MSG_VIEW_MY_BIDS_REQ, MSG_VIEW_MY_BIDS_RESP,
    MSG_SUBMIT_FEEDBACK_REQ, MSG_SUBMIT_FEEDBACK_RESP,

    /* auctioneer */
    MSG_CREATE_AUCTION_REQ, MSG_CREATE_AUCTION_RESP,
    MSG_CLOSE_AUCTION_REQ, MSG_CLOSE_AUCTION_RESP,

    /* moderator */
    MSG_VIEW_DISPUTES_REQ, MSG_VIEW_DISPUTES_RESP,
    MSG_RESOLVE_DISPUTE_REQ, MSG_RESOLVE_DISPUTE_RESP,
    MSG_VIEW_FEEDBACK_REQ, MSG_VIEW_FEEDBACK_RESP,
    MSG_TOGGLE_ACCOUNT_REQ, MSG_TOGGLE_ACCOUNT_RESP,

    /* admin */
    MSG_ADD_USER_REQ, MSG_ADD_USER_RESP,
    MSG_LIST_USERS_REQ, MSG_LIST_USERS_RESP,
    MSG_CHANGE_ROLE_REQ, MSG_CHANGE_ROLE_RESP,

    /* notifications (server → client, unsolicited) */
    MSG_NOTIFY_OUTBID,
    MSG_NOTIFY_AUCTION_WON,
    MSG_NOTIFY_AUCTION_CLOSED,

    MSG_ERROR                       /* generic error response */
} MessageType;

/* Fixed-size message header sent before every payload */
typedef struct {
    MessageType type;
    uint32_t    length;             /* bytes of payload that follow     */
    int         status;             /* 0 = OK, non-zero = error code    */
} MessageHeader;

/* ─── error codes ────────────────────────────────────────── */
#define ERR_OK              0
#define ERR_AUTH_FAIL       1    /* wrong password                    */
#define ERR_ALREADY_LOGGED  2    /* second login attempt              */
#define ERR_PERMISSION      3    /* wrong role for this action        */
#define ERR_NOT_FOUND       4
#define ERR_BID_TOO_LOW     5
#define ERR_INSUFFICIENT    6    /* wallet balance too low            */
#define ERR_AUCTION_CLOSED  7
#define ERR_ACCOUNT_BANNED  8
#define ERR_INTERNAL        99

#endif /* MODELS_H */
