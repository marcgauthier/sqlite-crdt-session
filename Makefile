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

# Build crdtsession.so and benchmark against a separately installed cr-sqlite binary.
# Example: make bench-compare CRSQLITE=/tmp/crsqlite.so
CRSQLITE ?= build/benchmark/crsqlite.so
.PHONY: bench-compare bench-fetch
bench-compare: crdtsession.so
	python3 tests/bench_write.py --crsqlite "$(CRSQLITE)" --require-crsqlite --rows 10000 --batch 100 --storage file --repeats 3

bench-fetch:
	python3 tools/fetch_crsqlite.py --output "$(CRSQLITE)"