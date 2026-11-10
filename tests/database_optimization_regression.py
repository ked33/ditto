"""Validate database upgrades, lookup indexes and atomic saves using synthetic data.

Run: python -B tests/database_optimization_regression.py
These SQLite/state checks do not replace the MFC build or Windows threading tests.
"""

import json
import re
import sqlite3
import tempfile
import unittest
from pathlib import Path

from search_sql_regression import SCHEMA, execute_all


ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def function(path, signature):
    return source(path).split(signature, 1)[1].split("\n}", 1)[0]


def strings(text):
    # Adjacent _T string literals form a single production SQL statement.
    pattern = r'(?:_T\("(?:\\.|[^"\\])*"\)\s*)+'
    return ["".join(json.loads(part) for part in re.findall(
        r'_T\(("(?:\\.|[^"\\])*")\)', group
    )) for group in re.findall(pattern, text)]


MIGRATION = strings(function("src/DatabaseUtilities.cpp", "static void EnsureStickyOrderMigration("))
LOOKUP_INDEXES = strings(function("src/DatabaseUtilities.cpp", "static void EnsureLookupIndexes("))
TRANSACTION = source("src/sqlite/CppSQLite3.cpp").split("CppSQLite3Transaction::CppSQLite3Transaction", 1)[1]
SAVEPOINT = next(sql for sql in strings(TRANSACTION) if sql.startswith("SAVEPOINT "))
ROLLBACK = next(sql for sql in strings(TRANSACTION) if sql.startswith("ROLLBACK TO "))
RELEASE = next(sql for sql in strings(TRANSACTION) if sql.startswith("RELEASE "))
BEGIN = next(sql for sql in strings(TRANSACTION) if sql.startswith("BEGIN "))
COMMIT = next(sql for sql in strings(TRANSACTION) if sql.startswith("COMMIT"))
FULL_ROLLBACK = next(sql for sql in strings(TRANSACTION) if sql == "ROLLBACK;")


def migrate(db, fail_before_marker=False):
    db.execute(MIGRATION[0])
    if db.execute(MIGRATION[1]).fetchone()[0]:
        return
    outermost = not db.in_transaction
    db.execute(BEGIN if outermost else SAVEPOINT)
    try:
        if not db.execute(MIGRATION[2]).fetchone()[0]:
            db.execute(MIGRATION[3])
            db.execute(MIGRATION[4])
        if fail_before_marker:
            raise RuntimeError("injected migration failure")
        db.execute(MIGRATION[5])
        db.execute(COMMIT if outermost else RELEASE)
    except Exception:
        db.execute(FULL_ROLLBACK if outermost else ROLLBACK)
        if not outermost:
            db.execute(RELEASE)
        raise


class DatabaseOptimizationTests(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:", isolation_level=None)
        self.addCleanup(self.db.close)
        self.db.executescript("""
            CREATE TABLE Main(lID INTEGER PRIMARY KEY, mText TEXT, QuickPasteText TEXT,
                bIsGroup INTEGER DEFAULT 0, lParentID INTEGER DEFAULT -1,
                stickyClipOrder REAL, stickyClipGroupOrder REAL, clipOrder REAL DEFAULT 0,
                lDontAutoDelete INTEGER DEFAULT 0, lShortCut INTEGER DEFAULT 0,
                lastPasteDate INTEGER DEFAULT 0);
            CREATE TABLE Data(lID INTEGER PRIMARY KEY, lParentID INTEGER,
                strClipBoardFormat TEXT, ooData BLOB);
            CREATE TABLE MainDeletes(clipID INTEGER, modifiedDate);
        """)

    def test_old_sticky_values_migrate_once_and_preserve_nonzero_positions(self):
        self.db.executemany("INSERT INTO Main(lID,stickyClipOrder,stickyClipGroupOrder) VALUES(?,?,?)",
                            [(1, None, 0), (2, 0, None), (3, 4, -7)])
        migrate(self.db)
        self.assertEqual(self.db.execute(
            "SELECT stickyClipOrder,stickyClipGroupOrder FROM Main ORDER BY lID"
        ).fetchall(), [(-2147483647, -2147483647), (-2147483647, -2147483647), (4, -7)])
        before = self.db.total_changes
        trace = []
        self.db.set_trace_callback(trace.append)
        migrate(self.db)
        self.assertEqual(self.db.total_changes, before)
        self.assertFalse(any(sql.upper().startswith("UPDATE MAIN") for sql in trace))

    def test_current_database_does_not_rewrite_legitimate_zero_sort_positions(self):
        self.db.execute("CREATE INDEX Main_TopLevel ON Main(stickyClipOrder, bIsGroup, clipOrder)")
        self.db.execute("INSERT INTO Main(lID,stickyClipOrder,stickyClipGroupOrder) VALUES(1,0,0)")
        migrate(self.db)
        self.assertEqual(self.db.execute("SELECT stickyClipOrder,stickyClipGroupOrder FROM Main").fetchone(), (0, 0))
        self.db.execute("DROP INDEX Main_TopLevel")
        migrate(self.db)
        self.assertEqual(self.db.execute("SELECT stickyClipOrder FROM Main").fetchone()[0], 0)

    def test_failed_migration_rolls_back_values_and_can_retry(self):
        self.db.execute("INSERT INTO Main(lID,stickyClipOrder,stickyClipGroupOrder) VALUES(1,0,NULL)")
        with self.assertRaisesRegex(RuntimeError, "injected"):
            migrate(self.db, True)
        self.assertEqual(self.db.execute("SELECT stickyClipOrder,stickyClipGroupOrder FROM Main").fetchone(), (0, None))
        self.assertEqual(self.db.execute("SELECT COUNT(*) FROM DittoSchemaMigrations").fetchone()[0], 0)
        migrate(self.db)
        self.assertEqual(self.db.execute("SELECT COUNT(*) FROM DittoSchemaMigrations").fetchone()[0], 1)

    def test_format_and_delete_lookups_use_covering_indexes(self):
        execute_all(self.db, LOOKUP_INDEXES)
        execute_all(self.db, LOOKUP_INDEXES)
        for fmt in ("CF_HDROP", "CF_DIB", "PNG", "Rich Text Format"):
            plan = self.db.execute("EXPLAIN QUERY PLAN SELECT lParentID FROM Data WHERE strClipBoardFormat=?", (fmt,)).fetchall()
            self.assertTrue(any("SEARCH Data USING COVERING INDEX Data_Format_ParentId" in row[3] for row in plan), plan)
        plan = self.db.execute("EXPLAIN QUERY PLAN DELETE FROM MainDeletes WHERE clipID=17").fetchall()
        self.assertTrue(any("SEARCH MainDeletes USING INDEX MainDeletes_ClipId" in row[3] for row in plan), plan)
        self.db.executemany("INSERT INTO Data(lParentID,strClipBoardFormat) VALUES(?,?)",
                            [(1, "PNG"), (1, "PNG"), (2, "png"), (3, "CF_HDROP")])
        self.assertEqual(self.db.execute("SELECT DISTINCT lParentID FROM Data WHERE strClipBoardFormat='PNG'").fetchall(), [(1,)])

    def save_clip(self, fail_format=False, fail_index=False):
        outermost = not self.db.in_transaction
        self.db.execute(BEGIN if outermost else SAVEPOINT)
        try:
            self.db.execute("INSERT INTO Main(lID,mText) VALUES(1,'description')")
            self.db.execute("INSERT INTO Data(lParentID,strClipBoardFormat,ooData) VALUES(1,'CF_UNICODETEXT',?)", (b"t\0\0\0",))
            self.db.execute("INSERT INTO Data(lParentID,strClipBoardFormat,ooData) VALUES(1,?,?)",
                            ("reject" if fail_format else "Rich Text Format", b"{rtf}"))
            self.db.execute("UPDATE MainFullTextCache SET fulltext=? WHERE clipID=1", ("reject" if fail_index else "text",))
            self.db.execute(COMMIT if outermost else RELEASE)
        except sqlite3.DatabaseError:
            self.db.execute(FULL_ROLLBACK if outermost else ROLLBACK)
            if not outermost:
                self.db.execute(RELEASE)
            raise

    def prepare_save_failures(self):
        execute_all(self.db, [sql for sql in SCHEMA if "MainFullTextLiteral" not in sql])
        self.db.executescript("""
            CREATE TRIGGER RejectFormat BEFORE INSERT ON Data WHEN new.strClipBoardFormat='reject'
            BEGIN SELECT RAISE(ABORT,'format failure'); END;
            CREATE TRIGGER RejectIndex BEFORE UPDATE OF fulltext ON MainFullTextCache WHEN new.fulltext='reject'
            BEGIN SELECT RAISE(ABORT,'index failure'); END;
        """)

    def test_format_or_index_failure_leaves_no_partial_clip(self):
        self.prepare_save_failures()
        for options in ({"fail_format": True}, {"fail_index": True}):
            with self.assertRaises(sqlite3.DatabaseError):
                self.save_clip(**options)
            for table in ("Main", "Data", "MainFullTextCache"):
                self.assertEqual(self.db.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0], 0, table)
        self.save_clip()
        self.assertEqual(self.db.execute("SELECT COUNT(*) FROM Data").fetchone()[0], 2)
        self.assertEqual(self.db.execute("SELECT fulltext FROM MainFullTextCache").fetchone()[0], "text")

    def test_nested_savepoint_preserves_outer_work_and_outer_rollback(self):
        self.prepare_save_failures()
        self.db.execute(SAVEPOINT)
        self.db.execute("INSERT INTO Main(lID,mText) VALUES(9,'outer')")
        with self.assertRaises(sqlite3.DatabaseError):
            self.save_clip(fail_format=True)
        self.assertEqual(self.db.execute("SELECT lID FROM Main").fetchall(), [(9,)])
        self.save_clip()
        self.db.execute(ROLLBACK)
        self.db.execute(RELEASE)
        self.assertEqual(self.db.execute("SELECT COUNT(*) FROM Main").fetchone()[0], 0)

    def test_retention_filters_after_top_history_and_bounds_batches(self):
        body = function("src/DatabaseUtilities.cpp", "BOOL RemoveOldEntries(")
        query = next(sql for sql in strings(body) if sql.startswith("SELECT lID FROM (SELECT"))
        self.db.executemany("INSERT INTO Main(lID,clipOrder,stickyClipOrder,stickyClipGroupOrder,lShortCut) VALUES(?,?, -2147483647,-2147483647,?)",
                            [(i, i, 1 if i % 4 == 0 else 0) for i in range(1, 101)])
        actual = [row[0] for row in self.db.execute(query.replace("%d", "10"))]
        expected = [i for i in range(90, 0, -1) if i % 4 != 0][:25]
        self.assertEqual(actual, expected)
        self.db.execute("UPDATE Main SET lShortCut=NULL,lDontAutoDelete=NULL,lParentID=NULL WHERE lID=89")
        self.assertIn(89, [row[0] for row in self.db.execute(query.replace("%d", "10"))])

    def test_outer_transaction_reserves_writer_before_duplicate_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / "synthetic.db")
            first = sqlite3.connect(path, isolation_level=None, timeout=0)
            second = sqlite3.connect(path, isolation_level=None, timeout=0)
            try:
                first.execute("CREATE TABLE Main(id INTEGER PRIMARY KEY)")
                first.execute(BEGIN)
                first.execute("SELECT COUNT(*) FROM Main").fetchone()
                with self.assertRaisesRegex(sqlite3.OperationalError, "locked"):
                    second.execute(BEGIN)
                first.execute(FULL_ROLLBACK)
                second.execute(BEGIN)
                second.execute(COMMIT)
            finally:
                first.close()
                second.close()

    def test_transaction_wiring_and_literal_newlines(self):
        clip = function("src/Clip.cpp", "bool CClip::AddToDB(")
        self.assertLess(clip.index("CppSQLite3Transaction transaction"), clip.index("FindDuplicate()"))
        self.assertGreater(clip.index("m_LastAddedCRC ="), clip.rindex("transaction.commit()"))
        self.assertNotIn("m_LastAddedCRC =", function("src/Clip.cpp", "bool CClip::AddToMainTable("))
        self.assertIn("!SearchIndex::UpdateClipFullText", function("src/Clip.cpp", "bool CClip::AddToDataTable("))
        self.assertIn("sqlite3_mutex_enter(m_mutex)", TRANSACTION)
        self.assertIn("m_connectionLock(db.m_connectionMutex)", TRANSACTION)
        # Catch accidental physical newlines in ordinary C++ strings. Ignore
        # comments first so quoted prose in comments cannot hide a real error.
        for path in ("src/sqlite/CppSQLite3.cpp", "src/DatabaseUtilities.cpp", "src/MainFrmThread.cpp"):
            text = source(path)
            tokens = re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', text, re.S)
            for token in tokens:
                if token[0].startswith('"'):
                    self.assertNotIn("\n", token[0], f"physical newline in C++ string: {path}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
