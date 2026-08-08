CC = $(shell command -v clang >/dev/null 2>&1 && echo clang || echo cc)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror -pedantic
LDLIBS += -lsqlite3

TARGET := cron-scheduler
SOURCES := main.c cron.c
HEADERS := cron.h

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(SOURCES) $(LDLIBS) -o $@

clean:
	rm -f $(TARGET)
