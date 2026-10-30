"""Behavioral tests for the Session-backed CRDT extension (no Python dependencies)."""
import os
import sqlite3
import tempfile
import unittest
from pathlib import Path

EXTENSION = str(Path(__file__).resolve().parents[1] / 'crdtsession.so')


def new_db(node, ddl='CREATE TABLE items(id TEXT PRIMARY KEY, title TEXT, count INTEGER, raw BLOB)'):
    c = sqlite3.connect(':memory:')
    c.enable_load_extension(True)
    c.load_extension(EXTENSION)
    c.execute('SELECT crdt_init(?)', (node,))
    c.execute(ddl)
    c.commit()
    return c


def local(c, table, sql, parameters=()):
    c.execute('BEGIN')
    try:
        c.execute('SELECT crdt_start(?)', (table,))
        c.execute(sql, parameters)
        n = c.execute('SELECT crdt_flush()').fetchone()[0]
        c.commit()
        return n
    except Exception:
        c.rollback()
        c.execute('SELECT crdt_abort()')
        raise


def sync(src, dst, cursor=0):
    changes = src.execute('SELECT db_version,table_name,pk,cid,val,hlc,site_id,seq FROM crdt_changes WHERE db_version>? ORDER BY db_version', (cursor,)).fetchall()
    for ver, table, pk, cid, val, hlc, site, seq in changes:
        dst.execute('SELECT crdt_apply(?,?,?,?,?,?,?)', (table, pk, cid, val, hlc, site, seq))
        cursor = ver
    dst.commit()
    return cursor


class SessionCRDTTests(unittest.TestCase):
    def setUp(self):
        self.a, self.b = new_db('node-A'), new_db('node-B')

    def tearDown(self):
        self.a.close()
        self.b.close()

    def test_insert_remote_roundtrip_and_types(self):
        self.assertEqual(local(self.a, 'items', 'INSERT INTO items VALUES (?,?,?,?)', ('row1', 'first', 5, b'\x00\xff')), 3)
        sync(self.a, self.b)
        self.assertEqual(self.b.execute('SELECT * FROM items').fetchall(), [('row1', 'first', 5, b'\x00\xff')])
        self.assertEqual(self.b.execute('SELECT COUNT(*) FROM crdt_changes').fetchone()[0], 3)
        sync(self.a, self.b)
        self.assertEqual(self.b.execute('SELECT COUNT(*) FROM crdt_changes').fetchone()[0], 3)

    def test_coalescing_and_column_level_merge(self):
        self.a.execute('BEGIN')
        self.a.execute("SELECT crdt_start('items')")
        self.a.execute("INSERT INTO items(id,title,count) VALUES ('r','one',1)")
        self.a.execute("UPDATE items SET title='two' WHERE id='r'")
        self.a.execute("UPDATE items SET count=9 WHERE id='r'")
        self.assertEqual(self.a.execute('SELECT crdt_flush()').fetchone()[0], 3)
        self.a.commit()
        sync(self.a, self.b)
        self.assertEqual(self.b.execute('SELECT title,count FROM items').fetchone(), ('two', 9))
        self.assertEqual(local(self.a,'items',"UPDATE items SET title='latest' WHERE id='r'"),1)
        self.assertEqual(local(self.b,'items',"UPDATE items SET count=12 WHERE id='r'"),1)
        sync(self.a,self.b)
        sync(self.b,self.a)
        sync(self.a,self.b)
        self.assertEqual(self.a.execute('SELECT title,count FROM items').fetchone(),('latest',12))
        self.assertEqual(self.b.execute('SELECT title,count FROM items').fetchone(),('latest',12))

    def test_delete_tombstone_prevents_stale_reinsertion(self):
        local(self.a,'items',"INSERT INTO items(id,title) VALUES ('x','v')")
        sync(self.a,self.b)
        old = self.a.execute("SELECT * FROM crdt_changes WHERE cid='title'").fetchone()
        local(self.a,'items',"DELETE FROM items WHERE id='x'")
        sync(self.a,self.b)
        self.assertEqual(self.b.execute('SELECT COUNT(*) FROM items').fetchone()[0],0)
        self.b.execute('SELECT crdt_apply(?,?,?,?,?,?,?)',(old[3],old[4],old[5],old[6],old[7],old[1],old[2]))
        self.assertEqual(self.b.execute('SELECT COUNT(*) FROM items').fetchone()[0],0)

    def test_rollback_of_unflushed_and_flushed_metadata(self):
        self.a.execute('BEGIN')
        self.a.execute("SELECT crdt_start('items')")
        self.a.execute("INSERT INTO items(id) VALUES ('rolled')")
        self.a.execute('SELECT crdt_flush()')
        self.a.rollback()
        self.assertEqual(self.a.execute('SELECT count(*) FROM items').fetchone()[0],0)
        self.assertEqual(self.a.execute('SELECT count(*) FROM crdt_changes').fetchone()[0],0)
        self.assertEqual(local(self.a,'items',"INSERT INTO items(id) VALUES ('valid')"),3)
        self.assertEqual(self.a.execute('SELECT min(seq) FROM crdt_changes').fetchone()[0],1)

    def test_composite_keys_and_binary_keys(self):
        a=new_db('a2','CREATE TABLE pairs(x INTEGER, y BLOB, v TEXT, PRIMARY KEY(x,y)) WITHOUT ROWID')
        b=new_db('b2','CREATE TABLE pairs(x INTEGER, y BLOB, v TEXT, PRIMARY KEY(x,y)) WITHOUT ROWID')
        try:
            local(a,'pairs',"INSERT INTO pairs VALUES (?,?,?)",(-5,b'\x00\xff','ok'))
            sync(a,b)
            self.assertEqual(b.execute('SELECT * FROM pairs').fetchone(),(-5,b'\x00\xff','ok'))
            local(a,'pairs',"DELETE FROM pairs WHERE x=-5")
            sync(a,b)
            self.assertEqual(b.execute('SELECT count(*) FROM pairs').fetchone()[0],0)
        finally:
            a.close();b.close()

    def test_out_of_order_delete_rebuilds_only_newer_columns(self):
        local(self.a,'items',"INSERT INTO items VALUES ('out','old',1,NULL)")
        sync(self.a,self.b)
        key=self.a.execute("SELECT pk FROM crdt_changes WHERE cid='title' LIMIT 1").fetchone()[0]
        t0=max(x[0] for x in self.a.execute('SELECT hlc FROM crdt_changes'))
        # Newer field arrives before the earlier delete. Existing stale count
        # must be cleared but newer title must remain.
        self.b.execute('SELECT crdt_apply(?,?,?,?,?,?,?)',('items',key,'title','new',t0+20,'outside',1))
        self.b.execute('SELECT crdt_apply(?,?,?,?,?,?,?)',('items',key,'__crdt_delete',None,t0+10,'outside',2))
        self.assertEqual(self.b.execute("SELECT title,count FROM items WHERE id='out'").fetchone(),('new',None))
        self.b.commit()

    def test_site_id_cannot_be_changed(self):
        with self.assertRaises(sqlite3.DatabaseError):
            self.a.execute("SELECT crdt_init('node-C')")
        self.assertEqual(self.a.execute('SELECT crdt_site_id()').fetchone()[0],'node-A')

    def test_multi_table_capture_and_pk_required(self):
        self.a.execute('CREATE TABLE notes(k INTEGER PRIMARY KEY, val TEXT)')
        self.a.execute('CREATE TABLE no_pk(a TEXT)')
        self.a.commit()
        self.a.execute('BEGIN')
        self.a.execute("SELECT crdt_start('items')")
        self.a.execute("SELECT crdt_track('notes')")
        self.a.execute("INSERT INTO items(id) VALUES ('foo')")
        self.a.execute("INSERT INTO notes VALUES (2,'bar')")
        self.a.execute('SELECT crdt_flush()')
        self.a.commit()
        self.assertEqual(self.a.execute('SELECT DISTINCT table_name FROM crdt_changes ORDER BY table_name').fetchall(), [('items',),('notes',)])
        self.a.execute('BEGIN')
        with self.assertRaises(sqlite3.DatabaseError):
            self.a.execute("SELECT crdt_start('no_pk')")
        self.a.rollback()


if __name__=='__main__':
    unittest.main()