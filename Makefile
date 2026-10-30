CC ?= cc
CFLAGS ?= -O3 -DNDEBUG -fPIC -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK
LDFLAGS ?= -shared
LDLIBS ?= -lsqlite3

.PHONY: all test clean
all: crdtsession.so

crdtsession.so: src/crdt_session.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test: crdtsession.so
	python3 -m unittest discover -s tests -v

clean:
	rm -f crdtsession.so