PREFIX ?= /usr/local
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS += -Iinclude
LDFLAGS ?=
LDLIBS += -lbluetooth -lm -lpthread

BINARIES = aml005_ambient aml005_sync_clock
COMMON_OBJ = src/aml005.o

all: $(BINARIES)

aml005_ambient: src/ambient_daemon.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

aml005_sync_clock: src/sync_clock.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c include/aml005.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 aml005_ambient $(DESTDIR)$(PREFIX)/bin/aml005_ambient
	install -m 0755 aml005_sync_clock $(DESTDIR)$(PREFIX)/bin/aml005_sync_clock
	install -d $(DESTDIR)/etc
	install -m 0644 aml005.conf.example $(DESTDIR)/etc/aml005.conf
	install -d $(DESTDIR)/etc/systemd/system
	install -m 0644 systemd/aml005-ambient.service $(DESTDIR)/etc/systemd/system/
	install -m 0644 systemd/aml005-sync-clock.service $(DESTDIR)/etc/systemd/system/
	install -m 0644 systemd/aml005-sync-clock.timer $(DESTDIR)/etc/systemd/system/

clean:
	rm -f src/*.o $(BINARIES)

.PHONY: all install clean
