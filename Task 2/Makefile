CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2
TARGET  = filelock

.PHONY: all clean

all: $(TARGET)

$(TARGET): filelock.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET) *.o stats.txt myfile myfile.lck result.txt
