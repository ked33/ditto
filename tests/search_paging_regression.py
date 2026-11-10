"""Check result-ID projection, pagination and separate readers with synthetic data.

The SELECT template is read from QPasteWnd.cpp. These tests do not open Ditto's
clipboard database or claim to exercise the Windows message loop.
"""
import json
import re
import sqlite3
import tempfile
import threading
import unittest
from contextlib import closing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/QPasteWnd.cpp").read_text(encoding="utf-8-sig")
START = SOURCE.index('sql.Format(_T("SELECT %s Main.lID, Main.mText, ')
END = SOURCE.index("IsDistinct, dataJoin, strFilter, csSort);", START)
TEMPLATE = "".join(json.loads(token) for token in re.findall(r'"[^"]*"', SOURCE[START:END]))
TOP_ORDER = "Main.stickyClipOrder DESC, Main.bIsGroup, Main.clipOrder DESC"
GROUP_ORDER = "Main.stickyClipGroupOrder DESC, Main.bIsGroup, Main.clipGroupOrder DESC"


def query(where="1", order=TOP_ORDER, join="", distinct=""):
    return TEMPLATE % (distinct, join, where, order)


def create_db(path=":memory:"):
    db = sqlite3.connect(path)
    db.executescript("""
        CREATE TABLE Main(lID INTEGER PRIMARY KEY, mText TEXT, lParentID INTEGER,
            lDontAutoDelete INTEGER, lShortCut INTEGER, bIsGroup INTEGER,
            QuickPasteText TEXT, clipOrder REAL, clipGroupOrder REAL,
            stickyClipOrder REAL, stickyClipGroupOrder REAL, lDate INTEGER,
            lastPasteDate INTEGER);
        CREATE TABLE Data(lParentID INTEGER, strClipBoardFormat TEXT, ooData BLOB);
        CREATE INDEX Data_Format_Parent ON Data(strClipBoardFormat,lParentID);
    """)
    db.executemany("INSERT INTO Main VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", (
        (i, "description " + str(i), i % 7, i % 5 == 0, 0, i % 19 == 0,
         "", i * 1.25, -i * 1.25, 5 if i % 31 == 0 else -2147483647,
         7 if i % 23 == 0 else -2147483647, i, i)
        for i in range(1, 12001)))
    db.executemany("INSERT INTO Data VALUES(?,?,?)", (
        (i, fmt, b"body") for i in range(1, 12001)
        for fmt in (("CF_HDROP", "CF_UNICODETEXT") if i % 11 == 0 else ("CF_UNICODETEXT",))))
    db.commit()
    return db


class PagingTests(unittest.TestCase):
    def setUp(self):
        self.db = create_db()

    def tearDown(self):
        self.db.close()

    def check_selection(self, sql):
        expected = [row[0] for row in self.db.execute(sql)]
        ids = [row[0] for row in self.db.execute("SELECT lID FROM (" + sql + ")")]
        self.assertEqual(ids, expected)
        if ids:
            indexes = sorted(set((0, len(ids) // 2, len(ids) - 1)))
            self.assertTrue(all(ids[row] > 0 for row in indexes))
            for row in indexes:
                page = self.db.execute(sql + " LIMIT 17 OFFSET ?", (row,)).fetchall()
                self.assertEqual([item[0] for item in page], ids[row:row + 17])
        return ids

    def test_deep_jump_and_select_all_keep_every_real_id(self):
        ids = self.check_selection(query())
        self.assertEqual(len(ids), 12000)
        self.assertEqual(len(set(ids)), 12000)

    def test_groups_sticky_order_and_format_filter(self):
        predicate = "Main.lParentID=3 AND Main.lID IN (SELECT lParentID FROM Data WHERE strClipBoardFormat='CF_HDROP')"
        self.check_selection(query(predicate, GROUP_ORDER))
        self.check_selection(query("Main.lDontAutoDelete>0"))

    def test_distinct_fallback_join_does_not_duplicate_selected_ids(self):
        sql = query("1", join="JOIN Data ON Main.lID=Data.lParentID", distinct="DISTINCT")
        ids = self.check_selection(sql)
        self.assertEqual(len(ids), 12000)

    def test_projection_does_not_read_clipboard_payloads(self):
        def authorize(action, table, column, database, trigger):
            if action == sqlite3.SQLITE_READ and table == "Data" and column == "ooData":
                return sqlite3.SQLITE_DENY
            return sqlite3.SQLITE_OK
        self.db.set_authorizer(authorize)
        self.check_selection(query("Main.lID IN (SELECT lParentID FROM Data WHERE strClipBoardFormat='CF_HDROP')"))

    def test_short_first_page_has_exact_total_even_with_no_results(self):
        for maximum in (0, 1, 16, 17, 18):
            sql = query("Main.lID<=" + str(maximum))
            page = self.db.execute(sql + " LIMIT 17").fetchall()
            total = self.db.execute("SELECT COUNT(*) FROM (" + sql + ")").fetchone()[0]
            if len(page) < 17:
                self.assertEqual(len(page), total)
            else:
                self.assertGreaterEqual(total, 17)

    def test_reorder_and_delete_refresh_positions_without_changing_ids(self):
        before = self.check_selection(query())
        selected = {before[0], before[6000], before[-1]}
        self.db.execute("UPDATE Main SET stickyClipOrder=99 WHERE lID=?", (before[-1],))
        after = self.check_selection(query())
        self.assertEqual(after[0], before[-1])
        self.assertEqual({value for value in after if value in selected}, selected)
        self.db.execute("DELETE FROM Main WHERE lID=?", (before[6000],))
        remaining = self.check_selection(query())
        self.assertEqual({value for value in remaining if value in selected}, selected - {before[6000]})

    def test_background_copy_invalidates_cached_selection_positions(self):
        sql = query()
        before = [row[0] for row in self.db.execute(sql)]
        anchors = {row: before[row] for row in (0, 16, 11999)}
        self.db.execute("UPDATE Main SET stickyClipOrder=1000 WHERE lID=?", (before[6000],))
        after = [row[0] for row in self.db.execute("SELECT lID FROM (" + sql + ")")]
        self.assertTrue(any(after[row] != clip_id for row, clip_id in anchors.items()))
        self.assertNotEqual(after, before)
        # Refreshing the view establishes a new consistent mapping; keeping the
        # old cached first row would duplicate a clip in this selected range.
        self.assertEqual(len(set(after)), len(after))

    def test_slow_count_connection_does_not_serialize_another_reader(self):
        with tempfile.TemporaryDirectory() as temp:
            path = str(Path(temp) / "readers.db")
            db = create_db(path)
            db.close()
            entered, release = threading.Event(), threading.Event()
            failures = []
            def count():
                try:
                    with closing(sqlite3.connect(path)) as reader:
                        def slow(value):
                            if value == 1:
                                entered.set()
                                if not release.wait(5):
                                    raise RuntimeError("count reader was not released")
                            return 1
                        reader.create_function("slow", 1, slow)
                        self.assertEqual(reader.execute("SELECT COUNT(*) FROM Main WHERE slow(lID)").fetchone()[0], 12000)
                except BaseException as error:
                    failures.append(error)
            thread = threading.Thread(target=count)
            thread.start()
            try:
                self.assertTrue(entered.wait(5))
                with closing(sqlite3.connect(path, timeout=1)) as reader:
                    self.assertEqual(len(reader.execute(query() + " LIMIT 17 OFFSET 1000").fetchall()), 17)
            finally:
                release.set()
                thread.join(5)
            self.assertFalse(thread.is_alive())
            self.assertEqual(failures, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
