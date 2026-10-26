CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local

k380-link: k380-link.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs libsystemd)

install: k380-link
	install -D -m 0755 k380-link $(DESTDIR)$(PREFIX)/sbin/k380-link
	install -D -m 0644 k380-link.service $(DESTDIR)/etc/systemd/system/k380-link.service

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/sbin/k380-link $(DESTDIR)/etc/systemd/system/k380-link.service

clean:
	rm -f k380-link

.PHONY: install uninstall clean
