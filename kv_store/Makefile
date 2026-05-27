CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2

all: server client

server: server.c kvstore.c
	$(CC) $(CFLAGS) -o server server.c kvstore.c

client: client.c
	$(CC) $(CFLAGS) -o client client.c

clean:
	rm -f server client server.log /tmp/kvstore.sock
