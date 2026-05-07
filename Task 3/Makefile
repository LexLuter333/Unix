CC = gcc
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE
TARGET = myinit

all: $(TARGET)

$(TARGET): myinit.c
	$(CC) $(CFLAGS) -o $(TARGET) myinit.c

clean:
	rm -f $(TARGET)

.PHONY: all clean
