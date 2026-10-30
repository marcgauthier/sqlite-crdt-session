"""Microbenchmark only; NOT a comparison to cr-sqlite.
Run: python3 tests/bench_write.py --rows 10000 --batch 100
"""
import argparse
import sqlite3
import sys
import time
from pathlib import Path

EXTENSION = str(Path(__file__).resolve().parents[1] / 'crdtsession.so')


def run(rows, batch, crdt):
    db = sqlite3.connect(':memory:')
    db.execute('PRAGMA journal_mode=MEMORY')
    db.execute('CREATE TABLE entries(id TEXT PRIMARY KEY, payload TEXT)')
    if crdt:
        db.enable_load_extension(True)
        db.load_extension(EXTENSION)
        db.execute("SELECT crdt_init('benchmark-node')")
    db.commit()
    start=time.perf_counter()
    for offset in range(0,rows,batch):
        db.execute('BEGIN')
        if crdt:
            db.execute("SELECT crdt_start('entries')")
        db.executemany('INSERT INTO entries VALUES (?,?)', ((f'k{i}',f'v{i}') for i in range(offset,min(offset+batch,rows))))
        if crdt:
            db.execute('SELECT crdt_flush()')
        db.commit()
    elapsed=time.perf_counter()-start
    metadata=db.execute('SELECT count(*) FROM _crdt_log').fetchone()[0] if crdt else 0
    db.close()
    return elapsed, metadata


if __name__=='__main__':
    ap=argparse.ArgumentParser()
    ap.add_argument('--rows',type=int,default=10000)
    ap.add_argument('--batch',type=int,default=100)
    args=ap.parse_args()
    for crdt in (False,True):
        secs, changes=run(args.rows,args.batch,crdt)
        print(f'{"session CRDT" if crdt else "plain SQLite":<18} {args.rows/secs:>11,.0f} rows/sec  elapsed={secs:.3f}s  log_entries={changes:,}')
    print('Note: different durability settings than WAL-on-disk; do not use as cr-sqlite comparison.')