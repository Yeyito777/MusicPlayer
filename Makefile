PREFIX = $(HOME)/.local

musicplayer: player.c
	$(CC) -Wall -Wextra -o $@ $<

install: musicplayer
	mkdir -p $(PREFIX)/bin
	cp -f musicplayer $(PREFIX)/bin/

clean:
	rm -f musicplayer

.PHONY: install clean
