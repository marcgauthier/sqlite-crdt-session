# sqlite-crdt-session

An **experimental, C11, loadable SQLite extension** that uses SQLite's native Session module to coalesce local SQL writes into **per-column last-writer-wins (LWW) CRDT events**. Application data and CRDT state are stored in the **same SQLite database** (one transaction, one WAL). There is **no sidecar database**, no Rust/C++ and no network transport.

> **Status: prototype, not yet a drop-in cr-sqlite replacement.** It has a pull/apply API and working tests, but does not have all of cr-sqlite's semantics, wire protocol or automatic capture. Run correctness tests and workloads before using it for important data. A three-way comparison benchmark is included; use the official cr-sqlite binary to run it.

## Prerequisites

* SQLite built with `SQLITE_ENABLE_SESSION` and `SQLITE_ENABLE_PREUPDATE_HOOK` and exporting `sqlite3session_*` and `sqlite3changeset_*` functions.
* C11 compiler and SQLite development headers and dynamic library.
* Tables with declared, **non-NULL** primary keys. Session cannot capture rows containing NULL PK fields. No virtual tables.

### Build and test (Linux)

```sh
make
make test
python3 tools/fetch_crsqlite.py  # fetch pinned official v0.16.3 release (requires network)
python3 tests/bench_write.py --crsqlite build/benchmark/crsqlite.so --storage file --rows 10000 --batch 100
```

On Linux, `make` produces `crdtsession.so`. This build links `-lsqlite3`, which must refer to the **same SQLite library used by the embedding process**. A statically linked or otherwise incompatible SQLite host might not be able to load this extension. Use a SQLite CLI or Python sqlite3 configured with extension loading.

## Quick start

Open two databases (A and B) with the **same application schema**, but unique node IDs. Each connection needs the extension loaded; `crdt_init` is idempotent for the same ID.

```sql
.load ./crdtsession
SELECT crdt_init('node-A');
CREATE TABLE items(id TEXT PRIMARY KEY NOT NULL, title TEXT, count INTEGER);

BEGIN;
SELECT crdt_start('items');              -- begin session capture
INSERT INTO items VALUES('1','hello',3);
UPDATE items SET count=4 WHERE id='1'; -- collapsed into the insert
SELECT crdt_flush();                      -- persist metadata before COMMIT
COMMIT;

-- Pull after the receiver's *last seen db_version for this source*:
SELECT db_version, table_name, pk, cid, val, hlc, site_id, seq
  FROM crdt_changes WHERE db_version > 0 ORDER BY db_version;
```

On B (also after calling `crdt_init('node-B')` and creating `items`), transfer these rows using **bound parameters**, ideally within a single transaction:

```sql
BEGIN;
-- Bind the seven source fields: table_name, pk, cid, val, hlc, site_id, seq
SELECT crdt_apply(?, ?, ?, ?, ?, ?, ?);
-- Repeat for each change, in any order (newer LWW cells take precedence).
COMMIT;
```

The `pk` field is an **opaque BLOB**, a type-preserving encoding of one or more primary-key values. It is not a cr-sqlite wire-format PK and should be transferred unchanged. In a replication protocol, the cursor is **per source database**, using `db_version`, not global across nodes.

To track more than one table in the same transaction, call `crdt_track('other_table')` after `crdt_start('items')`. **Every local write transaction must be explicitly wrapped** in `crdt_start` / `crdt_flush`; the extension does not observe other connections or ordinary writes made outside this capture window. Flush must occur **before** committing the application transaction. On failure, roll back and call `crdt_abort()`.

### Existing rows

To initialize CRDT state from rows that existed before installing the extension:

```sql
BEGIN;
SELECT crdt_seed('items');
COMMIT;
```

`crdt_seed` creates an initial event per non-primary-key field (or a row marker for a PK-only table), and refuses to run if CRDT state already exists for that table. Run it on **only one authoritative initial replica**, then synchronize that history to other nodes to avoid competing snapshots.

## SQL API

| Function / object | Purpose |
|---|---|
| `crdt_init(node_id)` | Create `_crdt_*` tables and views inside `main`; requires a persistent unique node ID; rejects a changed ID for the same database. |
| `crdt_start(table_name)` | Start a new Session capture, **inside** a user transaction. |
| `crdt_track(table_name)` | Add another application table to the active Session. |
| `crdt_flush()` | Coalesce changed rows, write per-column clocks/events, and close the Session; must be within the **same** transaction as the writes. Returns event count. |
| `crdt_abort()` | Discard the current Session object (does **not** roll back the user's SQL transaction). |
| `crdt_seed(table_name)` | Publish initial existing rows; refuses when the table has CRDT state. |
| `crdt_apply(table,pk,cid,val,hlc,site_id,seq)` | Apply one remote event (idempotent by origin ID + sequence). Returns 1 when accepted, 0 for stale/replay. Use inside a transaction for batches. |
| `crdt_changes` | SQL view for incremental replication, ordered by `db_version`. |
| `crsql_changes` | **Read-only** column-name compatibility view. **Not** cr-sqlite binary/protocol compatible or insertable. |
| `crdt_clock()`, `crdt_site_id()`, `crdt_storage()` | Local metadata introspection. Storage always returns `main`. |

### Internal tables

* `_crdt_config` — node ID, last HLC and local sequence.
* `_crdt_cells` — winning `(table, PK, column)` clock and origin.
* `_crdt_log` — append-only replication events and local `db_version` cursor.

Metadata and application writes share SQLite's normal rollback and WAL semantics. The extension uses an HLC packed as **Unix milliseconds in the high bits plus a 16-bit logical counter**; ties are resolved lexicographically by node ID. Deletions use `__crdt_delete` events; `__crdt_row` denotes a PK-only row's existence. A remote row delete removes older fields, preserving newer fields when those were already merged.

## Current limitations (important)

* **Manual transaction capture.** Unlike cr-sqlite triggers, writes are **not** automatically tracked; an omitted `crdt_start`/`crdt_flush` silently bypasses replication. This is not currently a drop-in API replacement.
* The native SQLite Session module needs PKs with no NULL values and does not capture virtual tables.
* The prototype applies remote cells individually with ordinary SQL `INSERT ... ON CONFLICT DO UPDATE`. Tables with required non-PK `NOT NULL` columns lacking defaults, inter-column constraints, foreign keys, and certain unique indexes may reject partially materialized remote rows or out-of-order changes. Full semantic parity requires **row buffering/materialization**.
* Default per-column LWW is not an incrementing counter, set/list CRDT, or full operation-based CRDT; concurrent updates to the same cell choose a winner. Reinsert/delete edge cases and advanced schema migrations need more testing.
* `crsql_changes` mimics **some column names only**. Do **not** feed it into cr-sqlite's wire ingestion: `pk` and clock representation differ; `crdt_apply` is the supported receiver.
* No network discovery, QUIC, peer acknowledgment, pruning/garbage collection, encryption, schema migration or automated peer-cursor management.
* Logs are append-only and grow until a future safe compaction protocol is implemented.
* No cr-sqlite speedup claim is established yet. The included benchmark measures all three engines when supplied with a real official cr-sqlite binary; review measured runs before drawing conclusions.

## Performance approach

1. SQLite Session coalesces repeated row writes before event generation.
2. Local flush reuses prepared metadata statements and allocates logical versions in a tight loop.
3. Every event is committed in the **same** database WAL as the application write, so no two-file durability protocol is needed.
4. The replication log has an increasing `db_version` index and per-origin deduplication (`UNIQUE(site_id, seq)`).

Next milestones are benchmarking against an actual cr-sqlite build with equal durability settings, batching remote apply with cached prepared statements, row materialization for constrained schemas, fuzz/property tests, and accurate deletion/insert semantics under adversarial operation permutations.

## License

No license has been selected yet; permission to reuse or redistribute this source must be agreed by the repository owner.

## Three-way write benchmark: plain SQLite vs Session CRDT vs cr-sqlite

`tests/bench_write.py` executes **identical inserts** into `entries(id TEXT PRIMARY KEY NOT NULL, payload TEXT)` with the same transaction size and connection settings. It benchmarks the following configurations:

* **Plain SQLite** (no CRDT metadata).
* **sqlite-crdt-session** (`crdt_start`/`crdt_flush` inside each transaction).
* **cr-sqlite** (official loadable extension; one-time `crsql_as_crr('entries')` setup).

All three use one connection, prepared inserts via Python `executemany`, and the same journal/synchronous pragmas. Extension loading, table creation, registration, row-count verification, and change-count queries are **excluded from the timed write path**. The result contains median throughput, median elapsed time, and all individual samples. Different CRDT internal event counts are shown for information only; they are **not** equivalent history semantics.

For Linux x86-64 (downloads official cr-sqlite release `v0.16.3`):

```sh
make
make bench-fetch
make bench-compare
# Alternatively, provide your own official binary:
make bench-compare CRSQLITE=/path/to/crsqlite.so
```

For results in both memory and persistent WAL with machine-readable JSON:

```sh
python3 tests/bench_write.py --crsqlite build/benchmark/crsqlite.so --require-crsqlite \
  --storage memory --rows 10000 --batch 100 --repeats 3 --json benchmark-memory.json
python3 tests/bench_write.py --crsqlite build/benchmark/crsqlite.so --require-crsqlite \
  --storage file --rows 10000 --batch 100 --repeats 3 --json benchmark-wal.json
```

`--storage memory` defaults to `journal_mode=MEMORY`; `--storage file` defaults to `journal_mode=WAL`, and both default to `synchronous=FULL`. Run with `--batch 1`, `--batch 100`, or `--batch 1000` to compare transaction batching. For benchmarking without a downloaded cr-sqlite binary, omit `--crsqlite` to measure just plain SQLite and the Session prototype; the script clearly reports this as a **two-engine** run. Use `--require-crsqlite` in automation to prohibit accidental omission.

GitHub Actions includes a separate `benchmark-crsqlite` job, downloading the pinned release and running both workloads. It uploads `benchmark-memory.json` and `benchmark-wal.json` as artifacts. The cr-sqlite binary is only a **benchmark dependency**, not a dependency of the C11 extension. The extension has **not** been demonstrated faster than cr-sqlite merely by adding this benchmark.