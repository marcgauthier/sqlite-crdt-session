"""Reproducible SQLite write-throughput comparison.

Same SQL schema and transaction sizes for plain SQLite, sqlite-crdt-session,
and the official cr-sqlite loadable extension. Extension setup is NOT timed.

Examples:
  python3 tools/fetch_crsqlite.py
  python3 tests/bench_write.py --crsqlite build/benchmark/crsqlite.so --rows 10000 --batch 100
  python3 tests/bench_write.py --crsqlite build/benchmark/crsqlite.so --storage file --rows 10000 --batch 100

If --crsqlite is omitted, only the local baseline + Session runs are measured.
The cr-sqlite loadable extension is never silently substituted or simulated.
"""
from __future__ import annotations

import argparse
import json
import platform
import sqlite3
import statistics
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SESSION_EXTENSION = ROOT / 'crdtsession.so'
ENGINES = ('plain SQLite', 'session CRDT', 'cr-sqlite')


def connect_engine(engine: str, path: str, crsqlite: Path | None, journal: str, synchronous: str):
    db = sqlite3.connect(path)
    try:
        # cr-sqlite requires loading before creating/using CRR tables.
        if engine != 'plain SQLite':
            extension = SESSION_EXTENSION if engine == 'session CRDT' else crsqlite
            if extension is None or not extension.is_file():
                raise FileNotFoundError(f'{engine} extension missing: {extension}')
            db.enable_load_extension(True)
            try:
                db.load_extension(str(extension.resolve()))
            finally:
                db.enable_load_extension(False)
        actual = db.execute(f'PRAGMA journal_mode={journal}').fetchone()[0].lower()
        if actual != journal.lower():
            raise RuntimeError(f'requested journal_mode={journal} but SQLite enabled {actual}')
        db.execute(f'PRAGMA synchronous={synchronous}')
        db.execute('CREATE TABLE entries(id TEXT PRIMARY KEY NOT NULL, payload TEXT)')
        if engine == 'session CRDT':
            db.execute("SELECT crdt_init('benchmark-node')")
        elif engine == 'cr-sqlite':
            db.execute("SELECT crsql_as_crr('entries')")
        db.commit()
        return db
    except Exception:
        db.close()
        raise


def run(rows: int, batch: int, engine: str, path: str, crsqlite: Path | None,
        journal: str, synchronous: str) -> dict:
    db = connect_engine(engine, path, crsqlite, journal, synchronous)
    try:
        start = time.perf_counter()
        for offset in range(0, rows, batch):
            db.execute('BEGIN')
            if engine == 'session CRDT':
                db.execute("SELECT crdt_start('entries')")
            db.executemany('INSERT INTO entries(id,payload) VALUES (?,?)',
                           ((f'k{i}', f'v{i}') for i in range(offset, min(offset + batch, rows))))
            if engine == 'session CRDT':
                db.execute('SELECT crdt_flush()')
            db.commit()
        elapsed = time.perf_counter() - start
        actual_rows = db.execute('SELECT count(*) FROM entries').fetchone()[0]
        if actual_rows != rows:
            raise RuntimeError(f'{engine}: expected {rows} rows, got {actual_rows}')
        if engine == 'session CRDT':
            changes = db.execute('SELECT count(*) FROM crdt_changes').fetchone()[0]
        elif engine == 'cr-sqlite':
            changes = db.execute('SELECT count(*) FROM crsql_changes').fetchone()[0]
        else:
            changes = 0
        # Validate not merely inserting application rows without CRDT metadata.
        if engine != 'plain SQLite' and changes == 0 and rows > 0:
            raise RuntimeError(f'{engine} emitted no visible CRDT changes')
        return {'engine': engine, 'elapsed_seconds': elapsed, 'rows_per_second': rows / elapsed,
                'visible_changes': changes, 'row_count': actual_rows}
    finally:
        if engine == 'cr-sqlite':
            try:
                db.execute('SELECT crsql_finalize()')
            except sqlite3.Error:
                pass
        db.close()


def benchmark(rows: int, batch: int, repeats: int, storage: str, crsqlite: Path | None,
              journal: str, synchronous: str) -> dict:
    engines = ENGINES if crsqlite else ENGINES[:2]
    data = {engine: [] for engine in engines}
    with tempfile.TemporaryDirectory(prefix='crdt-benchmark-') as tempdir:
        for repeat in range(repeats):
            # Rotate execution order to reduce systematic first-run bias.
            order = list(engines[repeat % len(engines):] + engines[:repeat % len(engines)])
            for engine in order:
                safe_name = engine.replace(' ', '_').replace('-', '_')
                path = ':memory:' if storage == 'memory' else str(Path(tempdir) / f'{safe_name}_{repeat}.sqlite')
                data[engine].append(run(rows, batch, engine, path, crsqlite, journal, synchronous))
    return {
        'sqlite_version': sqlite3.sqlite_version,
        'python_version': platform.python_version(),
        'platform': platform.platform(),
        'workload': 'insert',
        'rows': rows, 'batch': batch, 'repeats': repeats, 'storage': storage,
        'journal_mode': journal, 'synchronous': synchronous,
        'crsqlite_extension': str(crsqlite.resolve()) if crsqlite else None,
        'results': [
            {'engine': engine,
             'median_rows_per_second': statistics.median(x['rows_per_second'] for x in data[engine]),
             'median_elapsed_seconds': statistics.median(x['elapsed_seconds'] for x in data[engine]),
             'visible_changes': data[engine][-1]['visible_changes'],
             'samples': data[engine]}
            for engine in engines
        ],
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rows', type=int, default=10000)
    ap.add_argument('--batch', type=int, default=100)
    ap.add_argument('--repeats', type=int, default=3)
    ap.add_argument('--crsqlite', type=Path, help='Path to official crsqlite.so/.dylib/.dll')
    ap.add_argument('--require-crsqlite', action='store_true', help='Fail if cr-sqlite is not provided')
    ap.add_argument('--storage', choices=['memory', 'file'], default='memory')
    ap.add_argument('--journal', choices=['memory', 'delete', 'wal'], default=None)
    ap.add_argument('--synchronous', choices=['OFF', 'NORMAL', 'FULL', 'EXTRA'], default='FULL')
    ap.add_argument('--json', type=Path, help='Save exact run configuration + per-run results as JSON')
    args = ap.parse_args()
    if min(args.rows, args.batch, args.repeats) <= 0:
        ap.error('--rows, --batch and --repeats must be positive')
    if args.require_crsqlite and not args.crsqlite:
        ap.error('--require-crsqlite needs --crsqlite PATH')
    journal = args.journal or ('memory' if args.storage == 'memory' else 'wal')
    if args.storage == 'memory' and journal == 'wal':
        ap.error('WAL is not available for an in-memory database; use --storage file')
    if not args.crsqlite:
        print('cr-sqlite not supplied; for a three-way comparison pass --crsqlite PATH', file=sys.stderr)
    try:
        result = benchmark(args.rows, args.batch, args.repeats, args.storage, args.crsqlite,
                           journal, args.synchronous)
    except (OSError, sqlite3.Error, RuntimeError) as exc:
        print(f'Benchmark failed: {exc}', file=sys.stderr)
        return 1
    print(f"SQLite {result['sqlite_version']} | {args.rows:,} inserts | {args.batch} / tx | "
          f"{args.repeats} repeats | {args.storage} | journal={journal} sync={args.synchronous}")
    print(f"{'Engine':<18} {'Median rows/s':>16} {'Median sec':>13} {'CRDT change rows':>17}")
    for r in result['results']:
        print(f"{r['engine']:<18} {r['median_rows_per_second']:>16,.0f} "
              f"{r['median_elapsed_seconds']:>13.4f} {r['visible_changes']:>17,}")
    session = next(r for r in result['results'] if r['engine'] == 'session CRDT')
    crsql = next((r for r in result['results'] if r['engine'] == 'cr-sqlite'), None)
    if crsql:
        print(f"Session / cr-sqlite throughput: {session['median_rows_per_second']/crsql['median_rows_per_second']:.2f}x")
    print('Setup excluded; CRDT change counts are implementation-specific, not directly comparable.')
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        print('JSON:', args.json)
    return 0


if __name__ == '__main__':
    sys.exit(main())