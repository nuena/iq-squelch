BIN:=iq-squelch
CC=gcc
SRCPATH=.
CFLAGS=-std=c99 -Wextra -Wall -Wno-unused-parameter -Wno-unused-function -I $(SRCPATH)
LDFLAGS=-lm
INSTALL_PREFIX=/usr/local

all: CFLAGS += -O3
all: $(BIN)

debug: CFLAGS += -DDEBUG -g
debug: $(BIN)

profile: CFLAGS += -pg
profile: LDFLAGS += -pg
profile: $(BIN)

$(BIN): $(BIN).o
	@$(CC) \
		-o $(BIN) \
		$(BIN).o \
		$(LDFLAGS)

$(BIN).o: $(BIN).c
	@$(CC) -c -o $(BIN).o $(BIN).c $(CFLAGS)

.PHONY: clean
clean:
	@rm -f *.o $(BIN)

.PHONY: install
install: $(BIN)
	@install $(BIN) $(INSTALL_PREFIX)/bin

.PHONY: uninstall
uninstall:
	@rm $(INSTALL_PREFIX)/bin/$(BIN)
