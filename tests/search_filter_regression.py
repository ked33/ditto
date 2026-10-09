"""Check production type-filter SQL against synthetic clips, never user data.

Run with DITTO_ICU_EXTENSION pointing to the newly compiled ICU_Loader.dll.
Without it, only SQL paging, RTF, and source wiring checks run.
"""

import json
import os
import re
import sqlite3
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/QPasteWnd.cpp").read_text(encoding="utf-8-sig")
ICU_EXTENSION = os.environ.get("DITTO_ICU_EXTENSION")
LITERAL = r'"(?:\\.|[^"\\])*"'


def sql_factory(name):
    # Evaluate the string-only C++ factories, including the shared predicate.
    source = (ROOT / "src/SearchIndexSql.cpp").read_text(encoding="utf-8")
    body = re.search(rf"{name}\(\)\s*\{{(.*?)\n\t\}}", source, re.S).group(1)
    body = re.sub(r"^\s*//.*$", "", body, flags=re.M)
    return "".join(sql_factory("WebLinkPredicateUtf8") if token == "WebLinkPredicateUtf8()"
                   else json.loads(token)
                   for token in re.findall(LITERAL + r"|WebLinkPredicateUtf8\(\)", body))


def link_filter():
    return sql_factory("WebLinkFilterUtf8")


def set_link_index(db, enabled=True):
    if enabled:
        db.execute(sql_factory("WebLinkIndexUtf8"))
    else:
        db.execute("DROP INDEX IF EXISTS Data_WebLink_v1")


def rtf_filter():
    return json.loads(re.search(
        rf'case SearchClipboardFormatFilter::RichText:\s*return _T\(({LITERAL})\);', SOURCE
    ).group(1))


def open_db(encoding="UTF-8"):
    db = sqlite3.connect(":memory:")
    db.execute(f"PRAGMA encoding='{encoding}'")
    if ICU_EXTENSION:
        db.enable_load_extension(True)
        db.load_extension(str(Path(ICU_EXTENSION).resolve()), entrypoint="sqlite3_icu_init")
        db.enable_load_extension(False)
    db.executescript("""
        CREATE TABLE Main(lID INTEGER PRIMARY KEY, mText TEXT, QuickPasteText TEXT,
                          lParentID INTEGER DEFAULT -1, starred INTEGER DEFAULT 0);
        CREATE TABLE Data(lParentID INTEGER, strClipBoardFormat TEXT, ooData BLOB);
        CREATE INDEX Data_ParentId_Format ON Data(lParentID, strClipBoardFormat);
        CREATE TABLE MainFullTextCache(clipID INTEGER PRIMARY KEY, fulltext TEXT);
    """)
    return db


def add_clip(db, clip_id, body, description="unrelated", ansi=None, group=-1, starred=0):
    db.execute("INSERT INTO Main VALUES(?,?,?,?,?)", (clip_id, description, "", group, starred))
    db.execute("INSERT INTO MainFullTextCache VALUES(?,?)", (clip_id, body or ""))
    if body is not None:
        db.execute("INSERT INTO Data VALUES(?,?,?)", (clip_id, "CF_UNICODETEXT", (body + "\0").encode("utf-16le")))
    if ansi is not None:
        db.execute("INSERT INTO Data VALUES(?,?,?)", (clip_id, "CF_TEXT", ansi.encode("ascii") + b"\0"))


def matching(db, predicate):
    return [row[0] for row in db.execute("SELECT Main.lID FROM Main WHERE " + predicate + " ORDER BY Main.lID")]


class FilterWiringTests(unittest.TestCase):
    def test_exact_prefixes_and_all_search_paths(self):
        for token, length, kind in [("!!img", 5, "Image"), ("!!file", 6, "File"),
                                    ("!!link", 6, "Link"), ("!!rtf", 5, "RichText")]:
            self.assertRegex(SOURCE, rf'search.Left\({length}\).CompareNoCase\(_T\("{token}"\)\) == 0 &&\s*'
                             rf'HasPrefixBoundary\(search, {length}\)\)\s*\{{\s*search = search.Mid\({length}\);\s*'
                             rf'search.TrimLeft\(\);\s*return SearchClipboardFormatFilter::{kind};')
        self.assertIn("text.GetLength() == prefixLength ||", SOURCE)
        self.assertIn("_istspace(text[prefixLength]) != 0", SOURCE)
        self.assertEqual(SOURCE.count("AppendAndSearchFilter(strFilter, clipboardFormatFilterSql);"), 2)
        self.assertIn("strFilter = clipboardFormatFilterSql;", SOURCE)
        self.assertLess(SOURCE.index("ExtractSearchClipboardFormatFilter(csSQLSearch)"), SOURCE.index("bool bQuickPastePrefixSearch"))
        self.assertIn("SearchIndexSql::WebLinkFilterUtf8().c_str()", SOURCE)
        self.assertIn("SearchIndexSql::WebLinkIndexUtf8()", (ROOT / "src/SearchIndex.cpp").read_text())

    def test_rtf_is_a_format_not_a_description_or_html(self):
        db = open_db()
        try:
            for i in range(1, 6):
                add_clip(db, i, "Rich Text Format demo.rtf", description="RTF demo.rtf")
            db.executemany("INSERT INTO Data VALUES(?,?,?)", [
                (1, "Rich Text Format", b"rtf"),
                (1, "Rich Text Format", b"duplicate"),
                (2, "HTML Format", b"html"),
                (3, "CF_HDROP", b"demo.rtf"),
                (4, "Rich Text Format", b"rtf"),
                (4, "HTML Format", b"html"),
            ])
            self.assertEqual(matching(db, rtf_filter()), [1, 4])
        finally:
            db.close()


class LinkPagingTests(unittest.TestCase):
    def test_pages_only_classify_candidates_up_to_requested_range(self):
        # Count predicate work, not elapsed time: this must pass on slow CI too.
        # The ASCII stand-ins measure query execution only; LinkFilterTests
        # separately verify real ICU matching and clipboard decoding.
        for ready in (False, True):
            db = open_db()
            calls = []

            def count_match(_pattern, text):
                calls.append(text)
                return text.startswith("https://")

            db.create_function("regexp", 2, count_match, deterministic=True)
            db.create_function("ditto_clipboard_text", 2, lambda blob, fmt:
                               blob.decode("utf-16le" if fmt == "CF_UNICODETEXT" else "ascii").rstrip("\0"), deterministic=True)
            try:
                for clip_id in range(1, 4001):
                    # Include both Unicode and ANSI-only clips, plus groups.
                    body = f"https://example.com/{clip_id}"
                    add_clip(db, clip_id, body if clip_id % 2 else None,
                             ansi=body, group=clip_id % 3)
                db.executescript("""
                    ALTER TABLE Main ADD stickyClipOrder REAL DEFAULT 0;
                    ALTER TABLE Main ADD bIsGroup INTEGER DEFAULT 0;
                    ALTER TABLE Main ADD clipOrder REAL DEFAULT 0;
                    UPDATE Main SET clipOrder=lID;
                    CREATE INDEX Main_TopLevel ON Main(stickyClipOrder DESC, bIsGroup ASC, clipOrder DESC);
                    CREATE INDEX Main_InGroup2 ON Main(lParentID ASC, stickyClipOrder DESC, bIsGroup ASC, clipOrder DESC);
                """)
                set_link_index(db, ready)
                for group in (None, 1):
                    expected = [i for i in range(4000, 0, -1) if group is None or i % 3 == group]
                    for offset in (0, 60, 600):
                        calls.clear()
                        sql = "SELECT Main.lID FROM Main WHERE " + link_filter()
                        if group is not None:
                            sql += f" AND Main.lParentID = {group}"
                        sql += " ORDER BY Main.stickyClipOrder DESC, Main.bIsGroup ASC, Main.clipOrder DESC"
                        sql += f" LIMIT 30 OFFSET {offset}"
                        with self.subTest(index_ready=ready, group=group, offset=offset):
                            self.assertEqual([r[0] for r in db.execute(sql)], expected[offset:offset + 30])
                            if ready:
                                self.assertEqual(len(calls), 0, "Indexed paging reclassified text")
                                plan = " ".join(r[3] for r in db.execute("EXPLAIN QUERY PLAN " + sql))
                                self.assertIn("Data_WebLink_v1", plan)
                            else:
                                self.assertLessEqual(len(calls), 2 * (offset + 30))
                if ready:
                    calls.clear()
                    self.assertEqual(db.execute("SELECT COUNT(*) FROM Main WHERE " + link_filter()).fetchone()[0], 4000)
                    self.assertEqual(calls, [], "Indexed count reclassified text")
            finally:
                db.close()


@unittest.skipUnless(ICU_EXTENSION, "requires compiled ICU_Loader.dll")
class LinkFilterTests(unittest.TestCase):
    def test_whole_web_link_and_boundaries(self):
        accepted = [
            "https://example.com", "http://example.com/a?x=1&y=2#part",
            "www.example.com/path", "HTTPS://EXAMPLE.COM", "WWW.Example.COM",
            " \t\r\nhttps://example.com/a%20b\r\n ", "https://例子.测试/中文😀",
            "http://localhost:8080/", "http://127.0.0.1:8000/a", "https://[::1]:443/a",
            "https://user:pass@example.com/a", "https://example.com/O'Reilly",
        ]
        rejected = [
            "", " ", "https://", "www.", "example.com", "mailto:a@example.com",
            "ftp://example.com", "请访问 https://example.com", "https://example.com 后续说明",
            "https://example.com\nhttps://other.com", "https://example.com/a b",
            '<a href="https://example.com">link</a>', "https:///path", "https://?q=a",
            "https://-invalid.com", "https://example..com", "https://example.com:abc",
            '"https://example.com"', "[site](https://example.com)",
        ]
        db = open_db()
        try:
            for i, body in enumerate(accepted + rejected, 1):
                add_clip(db, i, body)
            for ready in (False, True):
                set_link_index(db, ready)
                with self.subTest(index_ready=ready):
                    self.assertEqual(matching(db, link_filter()), list(range(1, len(accepted) + 1)))
        finally:
            db.close()

    def test_full_body_wins_over_description_and_ansi(self):
        db = open_db()
        try:
            add_clip(db, 1, "https://example.com/" + "x" * 2000 + " extra text", "https://example.com/")
            add_clip(db, 2, "ordinary text", "https://example.com", ansi="https://example.com")
            add_clip(db, 3, "https://example.com", "not a link", ansi="ordinary text")
            add_clip(db, 4, None, "not a link", ansi="https://ansi.example.com")
            add_clip(db, 5, None, "https://example.com")
            add_clip(db, 6, "https://example.com/" + "x" * 5000, "short description")
            db.execute("INSERT INTO Data VALUES(5, 'HTML Format', ?)", (b"https://example.com",))
            for ready in (False, True):
                set_link_index(db, ready)
                with self.subTest(index_ready=ready):
                    self.assertEqual(matching(db, link_filter()), [3, 4, 6])
        finally:
            db.close()

    def test_link_index_tracks_format_changes_and_rollbacks(self):
        db = open_db()
        try:
            set_link_index(db)
            add_clip(db, 1, "ordinary text", ansi="https://ansi.example.com")
            add_clip(db, 2, "https://unicode.example.com")
            self.assertEqual(matching(db, link_filter()), [2])
            db.execute("DELETE FROM Data WHERE lParentID=1 AND strClipBoardFormat='CF_UNICODETEXT'")
            self.assertEqual(matching(db, link_filter()), [1, 2])
            db.execute("UPDATE Data SET ooData=? WHERE lParentID=2",
                       (("ordinary text" + chr(0)).encode("utf-16le"),))
            self.assertEqual(matching(db, link_filter()), [1])
            db.commit()
            db.execute("UPDATE Data SET strClipBoardFormat='HTML Format' WHERE lParentID=1")
            self.assertEqual(matching(db, link_filter()), [])
            db.rollback()
            self.assertEqual(matching(db, link_filter()), [1])
            db.execute("INSERT INTO Data SELECT * FROM Data WHERE lParentID=1")
            self.assertEqual(matching(db, link_filter()), [1])
            db.execute("UPDATE Data SET lParentID=2 WHERE lParentID=1")
            self.assertEqual(matching(db, link_filter()), [])  # Unicode still wins.
            db.execute("DELETE FROM Data WHERE strClipBoardFormat='CF_UNICODETEXT'")
            self.assertEqual(matching(db, link_filter()), [2])
            db.execute("DELETE FROM Data")
            self.assertEqual(matching(db, link_filter()), [])
            self.assertEqual(db.execute("PRAGMA integrity_check").fetchone(), ("ok",))
        finally:
            db.close()

    def test_index_creation_rollback_reopen_and_repair(self):
        db = open_db()
        try:
            add_clip(db, 1, "https://example.com")
            db.commit()
            db.execute("BEGIN IMMEDIATE")
            set_link_index(db)
            db.rollback()
            self.assertIsNone(db.execute("SELECT name FROM sqlite_master WHERE name='Data_WebLink_v1'").fetchone())
            self.assertEqual(matching(db, link_filter()), [1])
            set_link_index(db)
            db.commit()
            with tempfile.TemporaryDirectory(prefix="ditto-link-index-test-") as folder:
                path = Path(folder) / "index.db"
                disk = sqlite3.connect(path)
                try:
                    db.backup(disk)
                finally:
                    disk.close()
                disk = sqlite3.connect(path)
                try:
                    # Match CppSQLite3DB::open: a fallback regexp is registered
                    # before loading ICU and its deterministic functions.
                    disk.create_function("regexp", 2, lambda pattern, text: 0)
                    disk.enable_load_extension(True)
                    disk.load_extension(str(Path(ICU_EXTENSION).resolve()), entrypoint="sqlite3_icu_init")
                    disk.enable_load_extension(False)
                    self.assertEqual(matching(disk, link_filter()), [1])
                    disk.execute("DROP INDEX Data_WebLink_v1")
                    set_link_index(disk)
                    self.assertEqual(matching(disk, link_filter()), [1])
                    self.assertEqual(disk.execute("PRAGMA integrity_check").fetchone(), ("ok",))
                finally:
                    disk.close()
        finally:
            db.close()

    def test_clipboard_decoding_is_independent_of_database_encoding(self):
        for encoding in ("UTF-8", "UTF-16le"):
            db = open_db(encoding)
            try:
                self.assertEqual(db.execute("PRAGMA encoding").fetchone()[0].lower(), encoding.lower())
                cases = [
                    (("中文😀\0padding").encode("utf-16le"), "CF_UNICODETEXT", "中文😀"),
                    ("https://example.com".encode("utf-16le"), "CF_UNICODETEXT", "https://example.com"),
                    (b"\0\0", "CF_UNICODETEXT", ""),
                    (b"x", "CF_UNICODETEXT", None),
                    (b"https://example.com\0padding", "CF_TEXT", "https://example.com"),
                    (b"hello", "CF_TEXT", "hello"),
                    (b"\0", "CF_TEXT", ""),
                    (None, "CF_TEXT", None),
                    (b"binary", "PNG", None),
                    (b"binary", None, None),
                ]
                for blob, fmt, expected in cases:
                    with self.subTest(encoding=encoding, format=fmt, blob=blob):
                        self.assertEqual(db.execute("SELECT ditto_clipboard_text(?,?)", (blob, fmt)).fetchone()[0], expected)
                add_clip(db, 1, "https://例子.测试/路径")
                add_clip(db, 2, None, ansi="https://ansi.example.com")
                set_link_index(db)
                self.assertEqual(matching(db, link_filter()), [1, 2])
            finally:
                db.close()

    def test_combined_keywords_groups_and_selection_count(self):
        db = open_db()
        try:
            add_clip(db, 1, "https://example.com/alpha", "needle", group=7, starred=1)
            add_clip(db, 2, "ordinary text", "needle", group=7, starred=1)
            add_clip(db, 3, "https://example.com/alpha", "needle", group=8, starred=1)
            add_clip(db, 4, "https://example.com/alpha", "needle", group=7, starred=0)
            add_clip(db, 5, "https://example.com/alpha", "other", group=7, starred=1)
            db.execute("UPDATE Main SET QuickPasteText='needle' WHERE lID=5")
            for ready in (False, True):
                set_link_index(db, ready)
                predicate = "(Main.mText LIKE '%needle%' OR Main.QuickPasteText LIKE '%needle%') AND " + link_filter()
                predicate += " AND Main.lParentID=7 AND Main.starred=1"
                self.assertEqual(matching(db, predicate), [1, 5])
                self.assertEqual(db.execute("SELECT COUNT(Main.lID) FROM Main WHERE " + predicate).fetchone()[0], 2)
                self.assertEqual(list(db.execute("SELECT Main.lID FROM Main WHERE " + predicate + " ORDER BY Main.lID LIMIT 1 OFFSET 1")), [(5,)])
        finally:
            db.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
