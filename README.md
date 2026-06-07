# Online Auction System

A multi-user, real-time auction platform built in C using POSIX sockets, threads, and flat-file storage. Supports four roles — Bidder, Auctioneer, Moderator, and Admin — communicating over TCP with a custom binary protocol.

---

## Architecture

```
Client (client.c)
       |
   TCP Socket (port 8080)
       |
Server (server.c)
       |
   Thread Pool (8 workers)
       |
  ┌────────────────────────────────┐
  │  auth.c       wallet.c        │
  │  bid_handler.c auction_engine.c│
  │  dispute.c    admin.c         │
  │  notify.c                     │
  └────────────────────────────────┘
       |
   Flat Files (data/)
       |
  users.dat  auctions.dat  bids.dat
  sessions.dat  disputes.dat
  feedback.dat  txn.log
```

The server uses a **thread pool** (8 worker threads) with a condition-variable-based work queue. The main thread accepts connections and enqueues them; workers handle all client communication.

---

## Roles

| Role        | Capabilities |
|-------------|-------------|
| Bidder      | Deposit, bid, view auctions, file disputes, submit feedback |
| Auctioneer  | Create auctions, close auctions manually |
| Moderator   | View/resolve disputes, view feedback, toggle accounts |
| Admin       | Add users, list users, change roles, all auctioneer actions |

---

## Features

- **Authentication** — SHA-256 password hashing via OpenSSL, one active session per user enforced with mutex-protected session file
- **Bidding** — Per-auction semaphore ensures only one bid lands at a time; concurrent bids on different auctions run in parallel
- **Wallet** — Hold/release/settle model: funds are frozen on bid, released on outbid, settled on win
- **Auto-close** — Dedicated timer thread wakes every second and closes expired auctions automatically
- **Push notifications** — Server pushes outbid, won, and closed events to connected clients without polling
- **Disputes** — Bidders can dispute closed auctions; moderators can refund or reject
- **ACID settlement** — Write-ahead log entry written before every wallet change
- **Soft delete** — Users are never physically removed; `is_deleted` flag preserves audit trail

---

## Project Structure

```
online-auction-system/
├── common/
│   ├── models.h          # All structs, enums, message protocol
│   └── file_io.h         # CRUD functions for all flat files (pthread-mutex locked)
├── server/
│   ├── server.c          # TCP server, thread pool, message dispatch
│   ├── auth.c/h          # Login, logout, password change
│   ├── auction_engine.c/h# Create/close auctions, timer thread
│   ├── bid_handler.c/h   # Place bid, view auctions, view bids
│   ├── wallet.c/h        # Hold, release, settle, deposit
│   ├── dispute.c/h       # File/resolve disputes, feedback, toggle account
│   ├── admin.c/h         # Add/list/modify users, change roles
│   └── notify.c/h        # Push notification registry
├── client/
│   └── client.c          # Terminal UI, listener thread for notifications
├── seed_admin.c           # One-time admin account creator
├── Makefile
└── README.md
```

---

## Build and Run

**Requirements:** GCC, OpenSSL, POSIX-compatible OS (Linux or macOS)

```bash
# Build everything
make

# Create initial admin account (run once)
make seed

# Terminal 1 — start server
./auction_server

# Terminal 2 — start client
./auction_client
```

**Default admin credentials:**
```
Username: admin
Password: admin123
```

---

## Concurrency Design

| Mechanism | Purpose |
|-----------|---------|
| Thread pool + condition variable | Serve multiple clients simultaneously |
| Per-auction semaphore | Serialize bids on the same auction |
| Per-file pthread mutex | Prevent file I/O races between threads |
| Per-user wallet mutex (64 buckets) | Prevent wallet corruption under concurrent bids |
| notify registry mutex | Safe concurrent push notification delivery |

---

## Message Protocol

Every client-server message uses a fixed header:

```c
typedef struct {
    MessageType type;    // request/response type
    uint32_t    length;  // payload bytes that follow
    int         status;  // ERR_OK=0 or error code
} MessageHeader;
```

Followed by a typed payload struct. The client has a dedicated listener thread that reads unsolicited `MSG_NOTIFY_*` messages pushed by the server, using `pthread_mutex_trylock` to avoid racing with the main request/response thread.

---

## Data Storage

All data is stored as fixed-size binary records in flat files under `data/`. Fixed-size records allow O(1) seek by ID:

```
offset = (id - 1) * sizeof(RecordType)
```

| File | Contents |
|------|----------|
| users.dat | User records (fixed-size, seekable by user_id) |
| auctions.dat | Auction records |
| bids.dat | Bid log (append-only) |
| sessions.dat | Active session slots |
| disputes.dat | Dispute records |
| feedback.dat | Feedback log (append-only) |
| txn.log | Human-readable audit log |

---

## Testing Scenarios Verified

- Admin creates users of all roles
- Auctioneer creates auction with timer
- Multiple bidders compete; outbid notifications delivered in real time
- Auction auto-closes; winner notified; wallet settled
- Bidder files dispute; moderator resolves with refund; wallet restored
- Moderator views feedback; toggles account status
- Concurrent bidding on same auction with correct winner selection
- Stale session handling on reconnect
