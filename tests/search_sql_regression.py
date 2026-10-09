"""Exercise the actual search schema SQL without MFC or a user database.

Run: python tests/search_sql_regression.py
Requires SQLite 3.34+ with FTS5/trigram (Python 3.12+ recommended).
"""

import json
import re
import sqlite3
import tempfile
import threading
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def statements(function):
    """Read concatenated C++ string literals from a SQL-only factory."""
    source = (ROOT / "src/SearchIndexSql.cpp").read_text(encoding="utf-8")
    body = re.search(
        rf"{function}\(\)\s*\{{(.*?)\n\t\}}", source, re.S
    ).group(1)
    body = re.sub(r"//[^\n]*", "", body)
    result, parts = [], []
    for token in re.findall(r'"(?:\\.|[^"\\])*"|[,;]', body):
        if token.startswith('"'):
            parts.append(json.loads(token))
        elif parts:
            result.append("".join(parts))
            parts = []
    return result


SCHEMA = statements("SchemaStatementsUtf8")
CLEANUP = statements("LegacyCleanupStatementsUtf8")
MIGRATE = statements("MigrateLegacyCacheUtf8")[0]

MAIN_SCHEMA = """
CREATE TABLE Main(
    lID INTEGER PRIMARY KEY, mText TEXT, QuickPasteText TEXT,
    bIsGroup INTEGER DEFAULT 0, lParentID INTEGER DEFAULT -1,
    stickyClipOrder REAL DEFAULT -2147483647, clipOrder REAL DEFAULT 0,
    lDontAutoDelete INTEGER DEFAULT 0);
CREATE INDEX Main_TopLevel ON Main(stickyClipOrder DESC, bIsGroup, clipOrder DESC);
CREATE TABLE Data(lID INTEGER PRIMARY KEY, lParentID INTEGER,
                  strClipBoardFormat TEXT, ooData BLOB);
CREATE INDEX Data_ParentId_Format ON Data(lParentID, strClipBoardFormat);
"""


def execute_all(db, sql):
    for statement in sql:
        db.execute(statement)


def create_legacy(db):
    # Historical v1 fixture: intentionally independent of the v2 factory.
    db.executescript("""
        CREATE TABLE MainSearchMeta(version INTEGER NOT NULL);
        INSERT INTO MainSearchMeta VALUES(1);
        CREATE TABLE MainSearchCache(
            clipID INTEGER PRIMARY KEY, description TEXT NOT NULL DEFAULT '',
            quickpaste TEXT NOT NULL DEFAULT '', fulltext TEXT NOT NULL DEFAULT '');
        CREATE VIRTUAL TABLE MainSearchIndex USING fts5(
            description, quickpaste, fulltext, content='MainSearchCache',
            content_rowid='clipID', tokenize='trigram', detail='none');
        CREATE TRIGGER MainSearchCache_ai AFTER INSERT ON MainSearchCache BEGIN
            INSERT INTO MainSearchIndex(rowid, description, quickpaste, fulltext)
            VALUES(new.clipID, new.description, new.quickpaste, new.fulltext);
        END;
        CREATE TRIGGER MainSearchMain_ad AFTER DELETE ON Main BEGIN
            DELETE FROM MainSearchCache WHERE clipID=old.lID;
        END;
    """)


def migrate(db, fail_after_cleanup=False):
    db.execute("BEGIN IMMEDIATE")
    try:
        execute_all(db, SCHEMA)
        db.execute(MIGRATE)
        execute_all(db, CLEANUP)
        if fail_after_cleanup:
            raise RuntimeError("injected failure before commit")
        db.execute("UPDATE MainSearchMeta SET version=2")
        db.commit()
    except Exception:
        db.rollback()
        raise


class SearchSchemaTests(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.db.executescript(MAIN_SCHEMA)

    def tearDown(self):
        self.db.close()

    def fresh(self):
        execute_all(self.db, SCHEMA)

    def add_clip(self, clip_id, description, fulltext, quickpaste=""):
        self.db.execute(
            "INSERT INTO Main(lID,mText,QuickPasteText,clipOrder) VALUES(?,?,?,?)",
            (clip_id, description, quickpaste, clip_id),
        )
        self.db.execute(
            "UPDATE MainFullTextCache SET fulltext=? WHERE clipID=?",
            (fulltext, clip_id),
        )

    def matching(self, text):
        return [row[0] for row in self.db.execute(
            "SELECT rowid FROM MainFullTextIndex WHERE fulltext LIKE ? ORDER BY rowid",
            ("%" + text + "%",),
        )]

    def integrity(self):
        self.db.execute(
            "INSERT INTO MainFullTextIndex(MainFullTextIndex,rank) "
            "VALUES('integrity-check',1)"
        )

    def test_schema_separates_description_and_fulltext(self):
        self.fresh()
        self.assertEqual(
            [row[1] for row in self.db.execute("PRAGMA table_info(MainFullTextCache)")],
            ["clipID", "fulltext"],
        )
        self.assertEqual(
            [row[1] for row in self.db.execute("PRAGMA table_info(MainFullTextIndex)")],
            ["fulltext"],
        )
        execute_all(self.db, SCHEMA)  # Reopening must be idempotent.
        self.integrity()

    def test_insert_edit_and_delete_stay_indexed(self):
        self.fresh()
        self.add_clip(1, "description", "oldneedle 剪贴板")
        self.assertEqual(self.matching("oldneedle"), [1])
        self.db.execute("UPDATE MainFullTextCache SET fulltext='newneedle' WHERE clipID=1")
        self.assertEqual(self.matching("oldneedle"), [])
        self.assertEqual(self.matching("newneedle"), [1])
        self.db.execute("DELETE FROM Main WHERE lID=1")
        self.assertEqual(self.matching("newneedle"), [])
        self.assertEqual(self.db.execute("SELECT count(*) FROM MainFullTextCache").fetchone()[0], 0)
        self.integrity()

    def test_description_edit_does_not_reindex_large_fulltext(self):
        self.fresh()
        self.add_clip(1, "before", "large body " * 10000)
        before = self.db.total_changes
        self.db.execute("UPDATE Main SET mText='after',QuickPasteText='alias' WHERE lID=1")
        self.assertEqual(self.db.total_changes - before, 1)
        self.assertEqual(self.matching("large body"), [1])
        self.integrity()

    def test_identical_fulltext_does_not_rewrite_index(self):
        self.fresh()
        self.add_clip(1, "description", "unchanged content")
        # Flush the preceding insert's pending FTS segment before measuring
        # this update; otherwise total_changes includes the earlier write.
        self.db.commit()
        before = self.db.total_changes
        self.db.execute("UPDATE MainFullTextCache SET fulltext=fulltext WHERE clipID=1")
        self.assertEqual(self.db.total_changes - before, 1)
        self.integrity()

    def test_deleting_nontext_format_keeps_text(self):
        self.fresh()
        self.add_clip(1, "description", "retained needle")
        self.db.executemany("INSERT INTO Data VALUES(?,?,?,?)", [
            (1, 1, "PNG", b"image"), (2, 1, "CF_UNICODETEXT", b"text")
        ])
        self.db.execute("DELETE FROM Data WHERE lID=1")
        self.assertEqual(self.matching("needle"), [1])
        self.db.execute("DELETE FROM Data WHERE lID=2")
        self.assertEqual(self.matching("needle"), [])
        self.integrity()

    def prepare_legacy(self):
        create_legacy(self.db)
        body = "中文🙂 repeated text " * 20000 + " unique_suffix_at_end"
        rows = [(1, "描述", "alias", body), (2, "", "", ""), (3, "group", "", "")]
        for clip_id, description, alias, text in rows:
            self.db.execute("INSERT INTO Main(lID,mText,QuickPasteText) VALUES(?,?,?)",
                            (clip_id, description, alias))
            self.db.execute("INSERT INTO MainSearchCache VALUES(?,?,?,?)",
                            (clip_id, description, alias, text))
        self.db.execute("INSERT INTO Data VALUES(1,1,'PNG',?)", (b"preserve raw data",))
        self.db.commit()
        return [(row[0], row[3]) for row in rows]

    def test_upgrade_preserves_complete_content_and_raw_data(self):
        expected = self.prepare_legacy()
        original_main = list(self.db.execute("SELECT * FROM Main"))
        original_data = list(self.db.execute("SELECT * FROM Data"))
        migrate(self.db)
        self.assertEqual(list(self.db.execute("SELECT * FROM MainFullTextCache ORDER BY clipID")), expected)
        self.assertEqual(list(self.db.execute("SELECT * FROM Main")), original_main)
        self.assertEqual(list(self.db.execute("SELECT * FROM Data")), original_data)
        self.assertEqual(self.matching("unique_suffix_at_end"), [1])
        self.assertEqual(self.db.execute("SELECT version FROM MainSearchMeta").fetchone(), (2,))
        self.assertFalse(list(self.db.execute(
            "SELECT name FROM sqlite_schema WHERE name IN ('MainSearchCache','MainSearchIndex')"
        )))
        self.integrity()

    def test_failed_upgrade_restores_old_schema_content_and_triggers(self):
        expected = self.prepare_legacy()
        with self.assertRaisesRegex(RuntimeError, "injected failure"):
            migrate(self.db, fail_after_cleanup=True)
        self.assertEqual(self.db.execute("SELECT version FROM MainSearchMeta").fetchone(), (1,))
        self.assertEqual(list(self.db.execute("SELECT clipID,fulltext FROM MainSearchCache ORDER BY clipID")), expected)
        self.assertFalse(list(self.db.execute("SELECT name FROM sqlite_schema WHERE name='MainFullTextCache'")))
        self.assertEqual(list(self.db.execute(
            "SELECT rowid FROM MainSearchIndex WHERE fulltext LIKE '%unique_suffix_at_end%'"
        )), [(1,)])
        self.db.execute("DELETE FROM Main WHERE lID=1")
        self.assertEqual(self.db.execute("SELECT count(*) FROM MainSearchCache").fetchone(), (2,))

    def test_missing_index_can_be_rebuilt_before_cache_repair(self):
        self.fresh()
        self.add_clip(1, "description", "before repair")
        self.db.execute("DROP TABLE MainFullTextIndex")
        execute_all(self.db, SCHEMA)
        self.db.execute("INSERT INTO MainFullTextIndex(MainFullTextIndex) VALUES('rebuild')")
        self.db.execute("DELETE FROM MainFullTextCache")
        self.db.execute("INSERT INTO MainFullTextCache VALUES(1,'after repair')")
        self.assertEqual(self.matching("after repair"), [1])
        self.integrity()

    def test_short_description_search_never_reads_fulltext_tables(self):
        self.fresh()
        self.add_clip(1, "配置说明", "huge unrelated body " * 10000)
        self.add_clip(2, "unrelated", "配置只在正文")
        touched = set()

        def authorize(action, table, column, database, trigger):
            if action == sqlite3.SQLITE_READ:
                touched.add(table)
            return sqlite3.SQLITE_OK

        self.db.set_authorizer(authorize)
        for text in ("配", "配置", "配置说明"):
            self.assertEqual(list(self.db.execute(
                "SELECT lID FROM Main WHERE mText LIKE ? ORDER BY stickyClipOrder DESC,bIsGroup,clipOrder DESC LIMIT 30",
                ("%" + text + "%",),
            )), [(1,)])
        self.db.set_authorizer(None)
        self.assertEqual(touched, {"Main"})

    def test_combined_search_keeps_index_and_filters_without_duplicates(self):
        self.fresh()
        self.add_clip(1, "needle description", "needle body")
        self.add_clip(2, "other", "needle body")
        self.add_clip(3, "needle description", "body")
        self.db.executemany("INSERT INTO Data VALUES(?,?,?,?)", [
            (1, 1, "PNG", b"x"), (2, 1, "CF_DIB", b"x"), (3, 2, "CF_HDROP", b"x")
        ])
        sql = """
            SELECT Main.lID FROM Main WHERE
            (Main.mText LIKE ? OR Main.lID IN
              (SELECT rowid FROM MainFullTextIndex Search WHERE Search.fulltext<>'' AND Search.fulltext LIKE ?))
            AND Main.lID IN (SELECT lParentID FROM Data WHERE strClipBoardFormat IN ('PNG','CF_DIB'))
            ORDER BY Main.stickyClipOrder DESC,Main.bIsGroup,Main.clipOrder DESC
        """
        args = ("%needle%", "%needle%")
        self.assertEqual(list(self.db.execute(sql, args)), [(1,)])
        plan = list(self.db.execute("EXPLAIN QUERY PLAN " + sql, args))
        self.assertTrue(any("L0" in row[3] for row in plan), plan)

    def test_fulltext_like_matches_scan_for_unicode_and_boolean_queries(self):
        self.fresh()
        self.add_clip(1, "one", "中文🙂 alpha needle")
        self.add_clip(2, "two", "中文🙂 beta needle")
        self.add_clip(3, "three", "英文 gamma")
        for predicate, args in [
            ("fulltext LIKE ?", ("%中%",)),
            ("fulltext LIKE ?", ("%中文%",)),
            ("fulltext LIKE ?", ("%中文🙂%",)),
            ("fulltext LIKE ? AND fulltext NOT LIKE ?", ("%needle%", "%alpha%")),
            ("fulltext LIKE ? OR fulltext LIKE ?", ("%alpha%", "%gamma%")),
        ]:
            indexed = list(self.db.execute(f"SELECT rowid FROM MainFullTextIndex WHERE {predicate} ORDER BY rowid", args))
            scanned = list(self.db.execute(f"SELECT clipID FROM MainFullTextCache WHERE {predicate} ORDER BY clipID", args))
            self.assertEqual(indexed, scanned)

    def test_description_nulls_keep_the_previous_cache_not_semantics(self):
        self.fresh()
        self.add_clip(1, None, "")
        self.add_clip(2, "needle", "")
        self.add_clip(3, "other", "")
        self.assertEqual(list(self.db.execute(
            "SELECT lID FROM Main WHERE COALESCE(mText,'') NOT LIKE '%needle%' ORDER BY lID"
        )), [(1,), (3,)])


class SearchCancellationTests(unittest.TestCase):
    def test_cancelled_reader_does_not_interrupt_writer_and_can_search_again(self):
        with tempfile.TemporaryDirectory(prefix="ditto-search-test-") as directory:
            path = Path(directory) / "search.db"
            writer = sqlite3.connect(path)
            writer.execute("CREATE TABLE writes(value)")
            writer.commit()
            reader = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True, check_same_thread=False)
            started = threading.Event()
            cancelled = threading.Event()
            errors = []

            def progress():
                started.set()
                return int(cancelled.is_set())

            reader.set_progress_handler(progress, 100)

            def search():
                try:
                    reader.execute("""
                        WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100000000)
                        SELECT sum(x) FROM n
                    """).fetchone()
                except sqlite3.OperationalError as error:
                    errors.append(error.sqlite_errorcode)

            thread = threading.Thread(target=search)
            thread.start()
            try:
                self.assertTrue(started.wait(5))
                writer.execute("INSERT INTO writes VALUES('unaffected')")
                writer.commit()
                cancelled.set()
                reader.interrupt()
                thread.join(5)
                self.assertFalse(thread.is_alive())
                self.assertEqual(errors, [sqlite3.SQLITE_INTERRUPT])
                reader.set_progress_handler(None, 0)
                self.assertEqual(reader.execute("SELECT count(*) FROM writes").fetchone(), (1,))
                with self.assertRaises(sqlite3.OperationalError):
                    reader.execute("DELETE FROM writes")
            finally:
                cancelled.set()
                reader.interrupt()
                thread.join()
                reader.close()
                writer.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
