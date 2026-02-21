PREFIX = $(HOME)/.local

musicplayer: player.c
	$(CC) -Wall -Wextra -o $@ $<

install: musicplayer
	mkdir -p $(PREFIX)/bin
	cp -f musicplayer $(PREFIX)/bin/

test: musicplayer
	bash tests/run.sh

clean:
	rm -f musicplayer

.PHONY: install test clean
