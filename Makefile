# ══════════════════════════════════════════════════════════
#  Online Auction System — Makefile
#  Directory layout:
#    common/   → models.h, file_io.h, *.h
#    server/   → *.c, *.h
#    client/   → client.c
#    data/     → runtime flat files (auto-created)
# ══════════════════════════════════════════════════════════

CC     = gcc
CFLAGS = -Wall -Wextra -g -pthread -D_POSIX_C_SOURCE=200809L \
         -I. -Icommon -Iserver

SERVER_SRCS = server/server.c       \
              server/auth.c         \
              server/auction_engine.c \
              server/bid_handler.c  \
              server/wallet.c       \
              server/dispute.c      \
              server/admin.c        \
              server/notify.c

CLIENT_SRCS = client/client.c

SERVER_BIN  = auction_server
CLIENT_BIN  = auction_client

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