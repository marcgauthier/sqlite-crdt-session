"""Non-network smoke checks for benchmark implementation."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

BASE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('bench_write', BASE / 'tests/bench_write.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)
spec2 = importlib.util.spec_from_file_location('fetch_crsqlite', BASE / 'tools/fetch_crsqlite.py')
fetch = importlib.util.module_from_spec(spec2)
spec2.loader.exec_module(fetch)


class BenchmarkTests(unittest.TestCase):
    def test_plain_and_session_same_counts(self):
        result = bench.benchmark(127, 13, 1, 'memory', None, 'memory', 'FULL')
        self.assertEqual([x['engine'] for x in result['results']], ['plain SQLite', 'session CRDT'])
        for result_row in result['results']:
            self.assertEqual(result_row['samples'][0]['row_count'], 127)
            self.assertGreater(result_row['median_rows_per_second'], 0)
        self.assertGreater(result['results'][1]['visible_changes'], 0)

    def test_missing_extension_rejected(self):
        with self.assertRaises(FileNotFoundError):
            bench.benchmark(10, 5, 1, 'memory', Path('/nonexistent/crsqlite.so'), 'memory', 'FULL')

    def test_wal_file_backend(self):
        result = bench.benchmark(31, 10, 1, 'file', None, 'wal', 'FULL')
        self.assertEqual(result['journal_mode'], 'wal')
        self.assertEqual(result['results'][1]['samples'][0]['row_count'], 31)

    def test_asset_selection(self):
        self.assertTrue(fetch.asset_match('crsqlite-linux-x86_64.zip', 'linux', 'x86_64'))
        self.assertTrue(fetch.asset_match('crsqlite-win-x64.zip', 'windows', 'x86_64'))
        self.assertFalse(fetch.asset_match('crsqlite-wasm.zip', 'linux', 'x86_64'))


if __name__ == '__main__':
    unittest.main()