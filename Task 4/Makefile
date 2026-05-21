CC=gcc
CFLAGS=-std=c99 -Wall -Wextra -O2
LDFLAGS=
TARGETS=brownian_server brownian_client test_client

all: $(TARGETS)

brownian_server: server.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

brownian_client: client.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_client: test_client.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS) *.o server.log result.txt server_stdout.log *.sock logs/*.log logs/*.out logs/*.txt
