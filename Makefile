# ══════════════════════════════════════════════════════════
#  Online Auction System — Makefile (flat layout)
#  All .c and .h files live in the same directory.
#
#  Usage:
#    make              → build server + client
#    make server       → build server only
#    make client       → build client only
#    make seed         → create first admin account
#    make run_server   → build + start server
#    make run_client   → build + start client
#    make clean        → remove binaries
# ══════════════════════════════════════════════════════════

CC     = gcc
CFLAGS = -Wall -Wextra -g -pthread -D_POSIX_C_SOURCE=200809L

# ── all server .c files (everything except client.c and seed) ──
SERVER_SRCS = server.c auth.c auctionengine.c bidhandler.c \
              wallet.c dispute.c admin.c notify.c

CLIENT_SRCS = client.c

SERVER_BIN  = auction_server
CLIENT_BIN  = auction_client

# ── default: build both ────────────────────────────────────
all: data $(SERVER_BIN) $(CLIENT_BIN)

data:
	mkdir -p data

$(SERVER_BIN): $(SERVER_SRCS)
	$(CC) $(CFLAGS) -o $@ $^
	@echo "Built: $(SERVER_BIN)"

$(CLIENT_BIN): $(CLIENT_SRCS)
	$(CC) $(CFLAGS) -o $@ $^
	@echo "Built: $(CLIENT_BIN)"

server: data $(SERVER_BIN)
client: data $(CLIENT_BIN)

# ── seed: compile and run the admin seeder ────────────────
seed: data
	$(CC) $(CFLAGS) -o seed_admin seed_admin.c
	./seed_admin

run_server: server
	./$(SERVER_BIN)

run_client: client
	./$(CLIENT_BIN)

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN) seed_admin
	@echo "Cleaned."

.PHONY: all server client seed run_server run_client clean
