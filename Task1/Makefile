# Makefile
CC = gcc
CFLAGS = -Wall -Wextra -std=c99
TARGET = myprogram
SRC = myprogram.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean
