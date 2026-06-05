#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "../common/models.h"

#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT  8080

/* ── global state ────────────────────────────────────────── */
static int    server_fd = -1;
static User   me;
static int    logged_in = 0;
static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

#define PRINT(fmt, ...) do { \
    pthread_mutex_lock(&print_lock); \
    printf(fmt, ##__VA_ARGS__); \
    fflush(stdout); \
    pthread_mutex_unlock(&print_lock); \
} while(0)

/* ══════════════════════════════════════════════════════════
   NETWORK HELPERS
   ══════════════════════════════════════════════════════════ */
static int send_msg(MessageType type, const void *payload, uint32_t length) {
    MessageHeader hdr = { .type = type, .length = length, .status = 0 };
    if (write(server_fd, &hdr, sizeof(hdr)) != sizeof(hdr)) return -1;
    if (length > 0 && payload)
        if (write(server_fd, payload, length) != (ssize_t)length) return -1;
    return 0;
}

static int recv_msg(MessageHeader *hdr_out, void *buf, size_t buf_size) {
    if (read(server_fd, hdr_out, sizeof(MessageHeader)) != sizeof(MessageHeader))
        return -1;
    if (hdr_out->length == 0) return 0;
    uint32_t to_read = hdr_out->length < buf_size
                     ? hdr_out->length : (uint32_t)buf_size;
    ssize_t got = 0; char *ptr = buf;
    while (got < (ssize_t)to_read) {
        ssize_t n = read(server_fd, ptr + got, to_read - got);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

/* ── input helpers ───────────────────────────────────────── */
static void read_line(const char *prompt, char *buf, size_t size) {
    PRINT("%s", prompt);
    if (fgets(buf, (int)size, stdin)) buf[strcspn(buf, "\n")] = '\0';
}
static double read_double(const char *p) {
    char b[64]; read_line(p, b, sizeof(b)); return atof(b);
}
static int read_int(const char *p) {
    char b[32]; read_line(p, b, sizeof(b)); return atoi(b);
}

/* ── display helpers ─────────────────────────────────────── */
static const char *role_str(Role r) {
    switch(r) {
        case ROLE_BIDDER:     return "Bidder";
        case ROLE_AUCTIONEER: return "Auctioneer";
        case ROLE_MODERATOR:  return "Moderator";
        case ROLE_ADMIN:      return "Admin";
        default:              return "?";
    }
}
static void sep(void) { PRINT("─────────────────────────────────────────────\n"); }

static void print_auction(const Auction *a) {
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", localtime(&a->end_time));
    PRINT("  #%-4u %-28s Rs.%-10.2f closes:%s\n",
          a->auction_id, a->item_name, a->current_bid, ts);
}

/* ══════════════════════════════════════════════════════════
   LISTENER THREAD — handles server-push notifications
   Uses select() with 1s timeout so it exits cleanly on logout
   ══════════════════════════════════════════════════════════ */
static void *listener_thread(void *arg) {
    (void)arg;
    char buf[1024];
    MessageHeader hdr;
    while (logged_in) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(server_fd, &rfds);
        struct timeval tv = {1, 0};
        if (select(server_fd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
        memset(buf, 0, sizeof(buf));
        if (recv_msg(&hdr, buf, sizeof(buf)) < 0) break;
        switch (hdr.type) {
            case MSG_NOTIFY_OUTBID: {
                typedef struct { uint32_t aid; double bid; } P;
                P *p = (P*)buf;
                PRINT("\n  ** OUTBID on Auction #%u — new high: Rs.%.2f\n",
                      p->aid, p->bid); break;
            }
            case MSG_NOTIFY_AUCTION_WON: {
                typedef struct { uint32_t aid; double amt; } P;
                P *p = (P*)buf;
                PRINT("\n  ** YOU WON Auction #%u for Rs.%.2f!\n",
                      p->aid, p->amt); break;
            }
            case MSG_NOTIFY_AUCTION_CLOSED: {
                typedef struct { uint32_t aid; } P;
                P *p = (P*)buf;
                PRINT("\n  ** Auction #%u has closed.\n", p->aid); break;
            }
            default: break;
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════
   AUTH
   ══════════════════════════════════════════════════════════ */
static int do_login(void) {
    typedef struct { char u[MAX_USERNAME]; char p[MAX_PASSWORD]; } P;
    P p; memset(&p, 0, sizeof(p));
    read_line("  Username: ", p.u, MAX_USERNAME);
    read_line("  Password: ", p.p, MAX_PASSWORD);
    send_msg(MSG_LOGIN_REQ, &p, sizeof(p));
    MessageHeader hdr; char buf[sizeof(User)];
    if (recv_msg(&hdr, buf, sizeof(buf)) < 0) return 0;
    if (hdr.status == ERR_OK) {
        memcpy(&me, buf, sizeof(User));
        logged_in = 1;
        PRINT("\n  Welcome, %s! [%s]\n", me.full_name, role_str(me.role));
        return 1;
    }
    switch(hdr.status) {
        case ERR_AUTH_FAIL:      PRINT("  Invalid credentials.\n");       break;
        case ERR_ALREADY_LOGGED: PRINT("  Already logged in elsewhere.\n"); break;
        case ERR_ACCOUNT_BANNED: PRINT("  Account inactive/banned.\n");   break;
        default:                 PRINT("  Login failed.\n");
    }
    return 0;
}

static void do_logout(void) {
    send_msg(MSG_LOGOUT_REQ, NULL, 0);
    MessageHeader hdr; char buf[64];
    recv_msg(&hdr, buf, sizeof(buf));
    logged_in = 0;
    PRINT("  Goodbye, %s!\n", me.full_name);
}

static void do_change_password(void) {
    typedef struct { char old[MAX_PASSWORD]; char nw[MAX_PASSWORD]; } P;
    P p; memset(&p, 0, sizeof(p));
    read_line("  Current password: ", p.old, MAX_PASSWORD);
    read_line("  New password:     ", p.nw,  MAX_PASSWORD);
    char confirm[MAX_PASSWORD];
    read_line("  Confirm new:      ", confirm, MAX_PASSWORD);
    if (strncmp(p.nw, confirm, MAX_PASSWORD)) {
        PRINT("  Passwords do not match.\n"); return;
    }
    send_msg(MSG_CHANGE_PASS_REQ, &p, sizeof(p));
    MessageHeader hdr; char buf[64];
    recv_msg(&hdr, buf, sizeof(buf));
    PRINT(hdr.status == ERR_OK ? "  Password changed.\n"
                               : "  Failed — wrong current password?\n");
}

/* ══════════════════════════════════════════════════════════
   SHARED: view auctions (used by all roles)
   ══════════════════════════════════════════════════════════ */
static void view_auctions(void) {
    send_msg(MSG_VIEW_AUCTIONS_REQ, NULL, 0);
    MessageHeader hdr; char buf[256 * sizeof(Auction)];
    if (recv_msg(&hdr, buf, sizeof(buf)) < 0) return;
    int n = hdr.length / sizeof(Auction);
    if (!n) { PRINT("  No open auctions.\n"); return; }
    sep();
    PRINT("  %-6s %-28s %-14s %s\n","ID","Item","Current Bid","Closes");
    sep();
    Auction *list = (Auction*)buf;
    for (int i = 0; i < n; i++) print_auction(&list[i]);
    sep();
}

/* ══════════════════════════════════════════════════════════
   BIDDER
   ══════════════════════════════════════════════════════════ */
static void bidder_place_bid(void) {
    typedef struct { uint32_t aid; double amount; } P;
    P p;
    p.aid    = (uint32_t)read_int("  Auction ID: ");
    p.amount = read_double("  Your bid (Rs.): ");
    send_msg(MSG_PLACE_BID_REQ, &p, sizeof(p));
    MessageHeader hdr; char buf[sizeof(Bid)];
    recv_msg(&hdr, buf, sizeof(buf));
    switch(hdr.status) {
        case ERR_OK:             PRINT("  Bid placed! You are the highest bidder.\n"); break;
        case ERR_BID_TOO_LOW:    PRINT("  Too low — must beat the current highest bid.\n"); break;
        case ERR_INSUFFICIENT:   PRINT("  Insufficient wallet balance.\n"); break;
        case ERR_AUCTION_CLOSED: PRINT("  Auction is not open.\n"); break;
        default:                 PRINT("  Error (%d).\n", hdr.status);
    }
}

static void bidder_balance(void) {
    send_msg(MSG_VIEW_BALANCE_REQ, NULL, 0);
    MessageHeader hdr; char buf[sizeof(User)];
    recv_msg(&hdr, buf, sizeof(buf));
    User *u = (User*)buf;
    PRINT("  Balance: Rs.%.2f  |  On hold: Rs.%.2f  |  Available: Rs.%.2f\n",
          u->wallet_balance, u->wallet_hold,
          u->wallet_balance - u->wallet_hold);
}

static void bidder_deposit(void) {
    typedef struct { double amount; } P;
    P p; p.amount = read_double("  Deposit (Rs.): ");
    if (p.amount <= 0) { PRINT("  Invalid amount.\n"); return; }
    send_msg(MSG_DEPOSIT_REQ, &p, sizeof(p));
    MessageHeader hdr; char buf[sizeof(User)];
    recv_msg(&hdr, buf, sizeof(buf));
    if (hdr.status == ERR_OK)
        PRINT("  Deposit successful. New balance: Rs.%.2f\n",
              ((User*)buf)->wallet_balance);
}

static void bidder_my_bids(void) {
    send_msg(MSG_VIEW_MY_BIDS_REQ, NULL, 0);
    MessageHeader hdr; char buf[256 * sizeof(Bid)];
    recv_msg(&hdr, buf, sizeof(buf));
    int n = hdr.length / sizeof(Bid);
    if (!n) { PRINT("  No bids yet.\n"); return; }
    sep();
    PRINT("  %-8s %-10s %-14s %-20s %s\n",
          "Bid ID","Auction","Amount","Time","Won?");
    sep();
    Bid *list = (Bid*)buf;
    for (int i = 0; i < n; i++) {
        char ts[20];
        strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",localtime(&list[i].placed_at));
        PRINT("  %-8u %-10u Rs.%-11.2f %-20s %s\n",
              list[i].bid_id, list[i].auction_id,
              list[i].amount, ts,
              list[i].is_winning ? "YES" : "No");
    }
    sep();
}

static void bidder_feedback(void) {
    typedef struct { uint32_t aid; char msg[MAX_FEEDBACK]; } P;
    P p; memset(&p,0,sizeof(p));
    p.aid = (uint32_t)read_int("  Auction ID: ");
    read_line("  Feedback: ", p.msg, MAX_FEEDBACK);
    send_msg(MSG_SUBMIT_FEEDBACK_REQ, &p, sizeof(p));
    MessageHeader hdr; char buf[64];
    recv_msg(&hdr, buf, sizeof(buf));
    PRINT(hdr.status==ERR_OK ? "  Feedback submitted.\n" : "  Failed.\n");
}

static void menu_bidder(void) {
    while (logged_in) {
        sep(); PRINT("  BIDDER — %s\n", me.full_name); sep();
        PRINT("  1.View auctions  2.Place bid  3.Balance\n");
        PRINT("  4.Deposit        5.My bids    6.Feedback\n");
        PRINT("  7.Change pass    8.Logout\n"); sep();
        switch(read_int("  Choice: ")) {
            case 1: view_auctions();       break;
            case 2: bidder_place_bid();    break;
            case 3: bidder_balance();      break;
            case 4: bidder_deposit();      break;
            case 5: bidder_my_bids();      break;
            case 6: bidder_feedback();     break;
            case 7: do_change_password();  break;
            case 8: do_logout(); return;
            default: PRINT("  Invalid.\n");
        }
    }
}

/* ══════════════════════════════════════════════════════════
   AUCTIONEER
   ══════════════════════════════════════════════════════════ */
static void auctioneer_create(void) {
    typedef struct {
        char item[MAX_ITEM_NAME]; char desc[MAX_DESC];
        double start; double reserve; uint32_t dur;
    } P;
    P p; memset(&p,0,sizeof(p));
    read_line("  Item name:         ", p.item, MAX_ITEM_NAME);
    read_line("  Description:       ", p.desc, MAX_DESC);
    p.start   = read_double("  Start price (Rs.): ");
    p.reserve = read_double("  Reserve (Rs.,0=none): ");
    p.dur     = (uint32_t)read_int("  Duration (seconds): ");
    send_msg(MSG_CREATE_AUCTION_REQ, &p, sizeof(p));
    MessageHeader hdr; char buf[sizeof(Auction)];
    recv_msg(&hdr, buf, sizeof(buf));
    if (hdr.status==ERR_OK)
        PRINT("  Auction #%u created.\n", ((Auction*)buf)->auction_id);
    else PRINT("  Failed.\n");
}

static void auctioneer_close(void) {
    typedef struct { uint32_t aid; } P;
    P p; p.aid=(uint32_t)read_int("  Auction ID: ");
    send_msg(MSG_CLOSE_AUCTION_REQ, &p, sizeof(p));
    MessageHeader hdr; char buf[sizeof(Auction)];
    recv_msg(&hdr, buf, sizeof(buf));
    switch(hdr.status) {
        case ERR_OK:             PRINT("  Auction closed and settled.\n"); break;
        case ERR_PERMISSION:     PRINT("  Not your auction.\n"); break;
        case ERR_AUCTION_CLOSED: PRINT("  Already closed.\n"); break;
        default:                 PRINT("  Error.\n");
    }
}

static void menu_auctioneer(void) {
    while (logged_in) {
        sep(); PRINT("  AUCTIONEER — %s\n", me.full_name); sep();
        PRINT("  1.View auctions  2.Create auction  3.Close auction\n");
        PRINT("  4.Change pass    5.Logout\n"); sep();
        switch(read_int("  Choice: ")) {
            case 1: view_auctions();      break;
            case 2: auctioneer_create();  break;
            case 3: auctioneer_close();   break;
            case 4: do_change_password(); break;
            case 5: do_logout(); return;
            default: PRINT("  Invalid.\n");
        }
    }
}

/* ══════════════════════════════════════════════════════════
   MODERATOR
   ══════════════════════════════════════════════════════════ */
static void mod_disputes(void) {
    send_msg(MSG_VIEW_DISPUTES_REQ, NULL, 0);
    MessageHeader hdr; char buf[128*sizeof(Dispute)];
    recv_msg(&hdr, buf, sizeof(buf));
    int n = hdr.length/sizeof(Dispute);
    if (!n) { PRINT("  No open disputes.\n"); return; }
    sep();
    Dispute *list=(Dispute*)buf;
    for(int i=0;i<n;i++)
        PRINT("  #%u | Auction#%u | user%u | %s\n",
              list[i].dispute_id, list[i].auction_id,
              list[i].raised_by_user_id, list[i].reason);
    sep();
}

static void mod_resolve(void) {
    typedef struct {
        uint32_t did; DisputeStatus st; char res[MAX_DESC];
    } P;
    P p; memset(&p,0,sizeof(p));
    p.did=(uint32_t)read_int("  Dispute ID: ");
    PRINT("  1.Resolve(refund)  2.Reject\n");
    p.st=(read_int("  Choice: ")==1)?DISPUTE_RESOLVED:DISPUTE_REJECTED;
    read_line("  Notes: ", p.res, MAX_DESC);
    send_msg(MSG_RESOLVE_DISPUTE_REQ, &p, sizeof(p));
    MessageHeader hdr; char buf[sizeof(Dispute)];
    recv_msg(&hdr, buf, sizeof(buf));
    PRINT(hdr.status==ERR_OK ? "  Resolved.\n" : "  Failed.\n");
}

static void mod_feedback(void) {
    send_msg(MSG_VIEW_FEEDBACK_REQ, NULL, 0);
    MessageHeader hdr; char buf[256*sizeof(Feedback)];
    recv_msg(&hdr, buf, sizeof(buf));
    int n = hdr.length/sizeof(Feedback);
    if (!n) { PRINT("  No unreviewed feedback.\n"); return; }
    sep();
    Feedback *list=(Feedback*)buf;
    for(int i=0;i<n;i++)
        PRINT("  #%u | Auction#%u | user%u | \"%s\"\n",
              list[i].feedback_id, list[i].about_auction_id,
              list[i].from_user_id, list[i].message);
    sep();
}

static void mod_toggle(void) {
    typedef struct { uint32_t uid; AccountStatus st; } P;
    P p;
    p.uid=(uint32_t)read_int("  User ID: ");
    PRINT("  1.Activate  2.Deactivate  3.Ban\n");
    int c=read_int("  Choice: ");
    p.st=(c==1)?ACCOUNT_ACTIVE:(c==2)?ACCOUNT_INACTIVE:ACCOUNT_BANNED;
    send_msg(MSG_TOGGLE_ACCOUNT_REQ, &p, sizeof(p));
    MessageHeader hdr; char buf[sizeof(User)];
    recv_msg(&hdr, buf, sizeof(buf));
    switch(hdr.status) {
        case ERR_OK:         PRINT("  Status updated.\n"); break;
        case ERR_PERMISSION: PRINT("  Cannot modify higher-rank user.\n"); break;
        default:             PRINT("  Error.\n");
    }
}

static void menu_moderator(void) {
    while (logged_in) {
        sep(); PRINT("  MODERATOR — %s\n", me.full_name); sep();
        PRINT("  1.Disputes  2.Resolve  3.Feedback  4.Toggle account\n");
        PRINT("  5.Change pass  6.Logout\n"); sep();
        switch(read_int("  Choice: ")) {
            case 1: mod_disputes();       break;
            case 2: mod_resolve();        break;
            case 3: mod_feedback();       break;
            case 4: mod_toggle();         break;
            case 5: do_change_password(); break;
            case 6: do_logout(); return;
            default: PRINT("  Invalid.\n");
        }
    }
}

/* ══════════════════════════════════════════════════════════
   ADMIN
   ══════════════════════════════════════════════════════════ */
static void admin_add_user(void) {
    typedef struct {
        char u[MAX_USERNAME]; char p[MAX_PASSWORD];
        char n[MAX_NAME]; Role role;
    } P;
    P p; memset(&p,0,sizeof(p));
    read_line("  Username:  ", p.u, MAX_USERNAME);
    read_line("  Password:  ", p.p, MAX_PASSWORD);
    read_line("  Full name: ", p.n, MAX_NAME);
    PRINT("  0=Bidder 1=Auctioneer 2=Moderator 3=Admin\n");
    p.role=(Role)read_int("  Role: ");
    send_msg(MSG_ADD_USER_REQ, &p, sizeof(p));
    MessageHeader hdr; char buf[sizeof(User)];
    recv_msg(&hdr, buf, sizeof(buf));
    if (hdr.status==ERR_OK)
        PRINT("  User '%s' created (id=%u).\n",
              ((User*)buf)->username, ((User*)buf)->user_id);
    else PRINT("  Failed — username may exist.\n");
}

static void admin_list_users(void) {
    send_msg(MSG_LIST_USERS_REQ, NULL, 0);
    MessageHeader hdr; char buf[512*sizeof(User)];
    recv_msg(&hdr, buf, sizeof(buf));
    int n = hdr.length/sizeof(User);
    if (!n) { PRINT("  No users.\n"); return; }
    sep();
    PRINT("  %-5s %-18s %-14s %-12s %-10s %s\n",
          "ID","Username","Name","Role","Status","Balance");
    sep();
    User *list=(User*)buf;
    for(int i=0;i<n;i++) {
        const char *st = list[i].status==ACCOUNT_ACTIVE?"Active":
                         list[i].status==ACCOUNT_INACTIVE?"Inactive":"Banned";
        PRINT("  %-5u %-18s %-14s %-12s %-10s Rs.%.2f\n",
              list[i].user_id, list[i].username, list[i].full_name,
              role_str(list[i].role), st, list[i].wallet_balance);
    }
    sep();
}

static void admin_change_role(void) {
    typedef struct { uint32_t uid; Role role; } P;
    P p;
    p.uid=(uint32_t)read_int("  User ID: ");
    PRINT("  0=Bidder 1=Auctioneer 2=Moderator 3=Admin\n");
    p.role=(Role)read_int("  New role: ");
    send_msg(MSG_CHANGE_ROLE_REQ, &p, sizeof(p));
    MessageHeader hdr; char buf[sizeof(User)];
    recv_msg(&hdr, buf, sizeof(buf));
    switch(hdr.status) {
        case ERR_OK:         PRINT("  Role updated.\n"); break;
        case ERR_PERMISSION: PRINT("  Cannot demote last admin.\n"); break;
        default:             PRINT("  Error.\n");
    }
}

static void menu_admin(void) {
    while (logged_in) {
        sep(); PRINT("  ADMIN — %s\n", me.full_name); sep();
        PRINT("  1.Add user  2.List users  3.Change role\n");
        PRINT("  4.Auctions  5.Change pass  6.Logout\n"); sep();
        switch(read_int("  Choice: ")) {
            case 1: admin_add_user();     break;
            case 2: admin_list_users();   break;
            case 3: admin_change_role();  break;
            case 4: view_auctions();      break;
            case 5: do_change_password(); break;
            case 6: do_logout(); return;
            default: PRINT("  Invalid.\n");
        }
    }
}

/* ══════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════ */
int main(void) {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

    if (connect(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect — is the server running?"); return 1;
    }

    PRINT("\n  ====================================\n");
    PRINT("     Online Auction System v1.0\n");
    PRINT("  ====================================\n\n");

    while (1) {
        PRINT("  1. Login\n  2. Exit\n");
        if (read_int("  Choice: ") != 1) break;
        if (!do_login()) continue;

        /* listener thread: handles push notifications concurrently */
        pthread_t tid;
        pthread_create(&tid, NULL, listener_thread, NULL);
        pthread_detach(tid);

        switch(me.role) {
            case ROLE_BIDDER:     menu_bidder();     break;
            case ROLE_AUCTIONEER: menu_auctioneer(); break;
            case ROLE_MODERATOR:  menu_moderator();  break;
            case ROLE_ADMIN:      menu_admin();      break;
        }
        /* logged_in=0 here; listener exits on next select() timeout */
    }

    close(server_fd);
    PRINT("  Goodbye!\n");
    return 0;
}
