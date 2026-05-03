CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDLIBS  := -lmosquitto -lm

PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin
UNITDIR := /etc/systemd/system

TARGET  := canary-mqtt-publisher
SRCS    := main.c

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDLIBS)

install: $(TARGET)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)
	install -m 644 $(TARGET).service $(UNITDIR)/$(TARGET).service
	systemctl daemon-reload

clean:
	rm -f $(TARGET)

.PHONY: install clean
