"""Single-file WAL durability, snapshot, ordering and replicated merge tests."""
import sqlite3
import tempfile
import unittest
from pathlib import Path

EXTENSION = str(Path(__file__).resolve().parents[1] / 'crdtsession.so')


def connect(path, node):
    db = sqlite3.connect(path)
    db.enable_load_extension(True)
    db.load_extension(EXTENSION)
    db.execute('PRAGMA journal_mode=WAL')
    db.execute('SELECT crdt_init(?)', (node,))
    return db


def copy_changes(src, dst, after=0):
    changes = src.execute('SELECT db_version,table_name,pk,cid,val,hlc,site_id,seq FROM crdt_changes WHERE db_version>? ORDER BY db_version', (after,)).fetchall()
    dst.execute('BEGIN')
    for version, table, pk, cid, val, hlc, site, seq in changes:
        dst.execute('SELECT crdt_apply(?,?,?,?,?,?,?)', (table, pk, cid, val, hlc, site, seq))
        after = version
    dst.commit()
    return after


class PersistenceTests(unittest.TestCase):
    def test_wal_reopen_one_db_file(self):
        with tempfile.TemporaryDirectory() as temp:
            db_path = str(Path(temp) / 'replica.sqlite')
            db = connect(db_path, 'A')
            db.execute('CREATE TABLE stuff(id INTEGER PRIMARY KEY, name TEXT)')
            db.commit()
            db.execute('BEGIN')
            db.execute("SELECT crdt_start('stuff')")
            db.execute("INSERT INTO stuff VALUES(1,'sample')")
            db.execute('SELECT crdt_flush()')
            db.commit()
            original = db.execute('SELECT db_version,pk,val,site_id FROM crdt_changes').fetchall()
            db.close()
            self.assertFalse(Path(db_path + '.crdt').exists())
            db = connect(db_path, 'A')
            self.assertEqual(db.execute('SELECT name FROM stuff').fetchone()[0], 'sample')
            self.assertEqual(db.execute('SELECT db_version,pk,val,site_id FROM crdt_changes').fetchall(), original)
            db.close()

    def test_seed_existing_rows_then_sync(self):
        left = sqlite3.connect(':memory:')
        left.execute('CREATE TABLE entries(k TEXT PRIMARY KEY, text TEXT, n INTEGER)')
        left.executemany('INSERT INTO entries VALUES(?,?,?)', [('a', 'hello', 4), ('b', 'world', 5)])
        left.commit()
        left.enable_load_extension(True)
        left.load_extension(EXTENSION)
        left.execute("SELECT crdt_init('original')")
        left.execute('BEGIN')
        self.assertEqual(left.execute("SELECT crdt_seed('entries')").fetchone()[0], 4)
        left.commit()
        with self.assertRaises(sqlite3.DatabaseError):
            left.execute('BEGIN')
            try:
                left.execute("SELECT crdt_seed('entries')")
            finally:
                left.rollback()
        right = connect(':memory:', 'replica')
        right.execute('CREATE TABLE entries(k TEXT PRIMARY KEY, text TEXT, n INTEGER)')
        right.commit()
        copy_changes(left, right)
        self.assertEqual(left.execute('SELECT * FROM entries ORDER BY k').fetchall(), right.execute('SELECT * FROM entries ORDER BY k').fetchall())
        left.close()
        right.close()

    def test_changes_can_be_forwarded_and_replayed_without_duplicates(self):
        a, b, c = [connect(':memory:', label) for label in ('a', 'b', 'c')]
        for db in (a, b, c):
            db.execute('CREATE TABLE x(id TEXT PRIMARY KEY, a TEXT, b INTEGER)')
            db.commit()
        a.execute('BEGIN')
        a.execute("SELECT crdt_start('x')")
        a.execute("INSERT INTO x VALUES('key','initial',100)")
        a.execute('SELECT crdt_flush()')
        a.commit()
        copy_changes(a, b)
        copy_changes(b, c)
        copy_changes(a, c)
        self.assertEqual(c.execute('SELECT * FROM x').fetchall(), [('key', 'initial', 100)])
        self.assertEqual(c.execute('SELECT COUNT(*) FROM crdt_changes').fetchone()[0], 2)
        for db in (a, b, c):
            db.close()

    def test_conflict_resolution_independent_of_delivery_order(self):
        a, b = [connect(':memory:', label) for label in ('a', 'b')]
        for db in (a, b):
            db.execute('CREATE TABLE x(id TEXT PRIMARY KEY, title TEXT)')
            db.commit()
        a.execute('BEGIN')
        a.execute("SELECT crdt_start('x')")
        a.execute("INSERT INTO x VALUES('r','start')")
        a.execute('SELECT crdt_flush()')
        a.commit()
        copy_changes(a, b)
        key = a.execute("SELECT pk FROM crdt_changes WHERE cid='title'").fetchone()[0]
        timestamp = a.execute('SELECT MAX(hlc) FROM crdt_changes').fetchone()[0]
        # Two different events with identical logical clocks, tie-break by node ID.
        args1 = ('x', key, 'title', 'first', timestamp + 50, 'node-a', 1)
        args2 = ('x', key, 'title', 'second', timestamp + 50, 'node-z', 1)
        a.execute('SELECT crdt_apply(?,?,?,?,?,?,?)', args1)
        a.execute('SELECT crdt_apply(?,?,?,?,?,?,?)', args2)
        b.execute('SELECT crdt_apply(?,?,?,?,?,?,?)', args2)
        b.execute('SELECT crdt_apply(?,?,?,?,?,?,?)', args1)
        self.assertEqual(a.execute('SELECT title FROM x').fetchone()[0], 'second')
        self.assertEqual(b.execute('SELECT title FROM x').fetchone()[0], 'second')
        a.close()
        b.close()

    def test_pk_only_and_null_column(self):
        a, b = [connect(':memory:', label) for label in ('a', 'b')]
        for db in (a, b):
            db.execute('CREATE TABLE tags(tag TEXT PRIMARY KEY) WITHOUT ROWID')
            db.execute('CREATE TABLE data(id TEXT PRIMARY KEY, payload BLOB)')
            db.commit()
        a.execute('BEGIN')
        a.execute("SELECT crdt_start('tags')")
        a.execute("SELECT crdt_track('data')")
        a.execute("INSERT INTO tags VALUES('simple')")
        a.execute("INSERT INTO data VALUES('a',NULL)")
        a.execute('SELECT crdt_flush()')
        a.commit()
        copy_changes(a, b)
        self.assertEqual(b.execute('SELECT tag FROM tags').fetchall(), [('simple',)])
        self.assertEqual(b.execute('SELECT * FROM data').fetchall(), [('a', None)])
        a.close()
        b.close()


if __name__ == '__main__':
    unittest.main()