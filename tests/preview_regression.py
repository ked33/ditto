"""Verify production preview SQL using synthetic data; no MFC or user DB.

These checks cover format isolation, complete BLOBs and snapshot consistency.
They do not exercise the Windows message loop or the GDI+ decoder.
"""

import json
import re
import sqlite3
import tempfile
import unittest
from contextlib import closing
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/AsyncClipPreview.cpp").read_text(encoding="utf-8")


def production_sql(name, *arguments):
    match = re.search(rf'const TCHAR\* {name} = _T\(("(?:\\.|[^"\\])*")\);', SOURCE)
    assert match, name
    return json.loads(match.group(1)) % arguments


SCHEMA = """
CREATE TABLE Main(lID INTEGER PRIMARY KEY, mText TEXT, lDate INTEGER,
    lastPasteDate INTEGER, lDontAutoDelete INTEGER, QuickPasteText TEXT,
    lShortCut INTEGER, globalShortCut INTEGER, stickyClipOrder REAL,
    stickyClipGroupOrder REAL, lParentID INTEGER);
CREATE TABLE Data(lID INTEGER PRIMARY KEY, lParentID INTEGER,
    strClipBoardFormat TEXT, ooData BLOB);
CREATE INDEX Data_ParentId_Format ON Data(lParentID, strClipBoardFormat);
INSERT INTO Main VALUES(1, 'description', 10, 20, 1, 'quick', 65, 1, 2, 3, 9);
INSERT INTO Main VALUES(9, 'parent', 0, 0, 0, '', 0, 0, 0, 0, -1);
"""


class PreviewSqlTests(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.db.executescript(SCHEMA)
        self.unicode = "完整文本\0仍保留后面的内容\0".encode("utf-16le")
        self.db.executemany("INSERT INTO Data VALUES(?,?,?,?)", [
            (1, 1, "CF_UNICODETEXT", self.unicode),
            (2, 1, "Rich Text Format", b"{\\rtf1 body}\0"),
            (3, 1, "HTML Format", b"<html>large alternate</html>\0"),
            (4, 1, "PNG", b"large image" * 10000),
            (5, 9, "CF_UNICODETEXT", b"other clip"),
        ])

    def tearDown(self):
        self.db.close()

    def test_metadata_and_formats_never_read_clipboard_blobs(self):
        def deny_blob(action, table, column, *_):
            if action == sqlite3.SQLITE_READ and table == "Data" and column == "ooData":
                return sqlite3.SQLITE_DENY
            return sqlite3.SQLITE_OK
        self.db.set_authorizer(deny_blob)
        metadata = self.db.execute(production_sql("PreviewMetadataSql", 1)).fetchone()
        self.assertEqual(metadata, ("description", 10, 20, 1, "quick", 65, 1, 2, 3, 9))
        formats = {row[0] for row in self.db.execute(production_sql("PreviewFormatsSql", 1))}
        self.assertEqual(formats, {"CF_UNICODETEXT", "Rich Text Format", "HTML Format", "PNG"})

    def test_requested_view_reads_only_its_clip_and_format(self):
        rows = self.db.execute(production_sql("PreviewDataSql", 1, "Rich Text Format")).fetchall()
        self.assertEqual(rows, [(b"{\\rtf1 body}\0",)])
        self.assertEqual(self.db.execute(production_sql("PreviewDataSql", 9, "PNG")).fetchall(), [])

    def test_duplicate_format_rows_choose_one_deterministic_body(self):
        self.db.execute("INSERT INTO Data VALUES(6,1,'CF_UNICODETEXT',?)", (b"later duplicate",))
        rows = self.db.execute(production_sql("PreviewDataSql", 1, "CF_UNICODETEXT")).fetchall()
        self.assertEqual(rows, [(self.unicode,)])

    def test_full_text_blob_keeps_nuls_unicode_and_article_tail(self):
        text = ("long article 中文 " * 100000 + "\0last words\0").encode("utf-16le")
        self.db.execute("UPDATE Data SET ooData=? WHERE lID=1", (text,))
        actual = self.db.execute(production_sql("PreviewDataSql", 1, "CF_UNICODETEXT")).fetchone()[0]
        self.assertEqual(actual, text)

    def test_folder_and_missing_metadata(self):
        self.assertEqual(self.db.execute(production_sql("PreviewFolderSql", 9)).fetchone(), ("parent", -1))
        self.assertIsNone(self.db.execute(production_sql("PreviewMetadataSql", 999)).fetchone())

    def test_snapshot_keeps_metadata_and_body_from_same_revision(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "preview-test.db"
            with closing(sqlite3.connect(path)) as writer:
                writer.execute("PRAGMA journal_mode=WAL")
                writer.executescript(SCHEMA)
                writer.execute("INSERT INTO Data VALUES(1,1,'CF_UNICODETEXT',?)", (self.unicode,))
                writer.commit()
                reader = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)
                try:
                    reader.execute("BEGIN")
                    old = reader.execute(production_sql("PreviewMetadataSql", 1)).fetchone()
                    writer.execute("UPDATE Main SET mText='updated' WHERE lID=1")
                    writer.execute("UPDATE Data SET ooData=? WHERE lID=1", (b"updated body",))
                    writer.commit()
                    self.assertEqual(old[0], "description")
                    self.assertEqual(reader.execute(production_sql("PreviewDataSql", 1, "CF_UNICODETEXT")).fetchone()[0], self.unicode)
                    reader.commit()
                    self.assertEqual(reader.execute(production_sql("PreviewMetadataSql", 1)).fetchone()[0], "updated")
                finally:
                    reader.close()


class PreviewWiringTests(unittest.TestCase):
    """Static checks for callers that cannot be exercised without Windows UI."""

    @staticmethod
    def read(name):
        return (ROOT / "src" / name).read_text(encoding="utf-8-sig")

    def test_synchronous_tooltip_callers_keep_format_switching(self):
        tooltip = self.read("ToolTipEx.cpp")
        deletion = self.read("DeleteClipData.cpp")
        self.assertIn("new CToolTipEx", deletion)
        self.assertNotIn("BeginPreview(", deletion)
        helper = tooltip.split("bool CToolTipEx::HasPreviewFormat(", 1)[1].split("\n}", 1)[0]
        for loaded_value in ("!m_csText.IsEmpty()", "!m_csRTF.IsEmpty()", "!m_html.IsEmpty()",
                             "m_imageViewer.m_pGdiplusBitmap != nullptr"):
            self.assertIn(loaded_value, helper)
        self.assertIn("IsWindow(m_previewOwner)", helper)
        # All menu/command-update checks use the same async/synchronous policy.
        self.assertNotIn("if (!m_hasPreview", tooltip)
        for fmt in ("Text", "RichText", "Html", "Image"):
            self.assertIn(f"if (!HasPreviewFormat(ClipPreviewFormat::{fmt}))", tooltip)

    def test_persistent_preview_preservation_is_explicit_and_database_scoped(self):
        header = self.read("QListCtrl.h")
        listing = self.read("QListCtrl.cpp")
        self.assertIn("CancelPendingPreview(bool preservePersistent = false)", header)
        cancellation = listing.split("void CQListCtrl::CancelPendingPreview(", 1)[1].split("\n}", 1)[0]
        for guard in ("preservePersistent", "VALID_TOOLTIP", "IsWindowVisible(m_toolTipHwnd)",
                      "GetShowPersistant()", "m_previewDatabasePath == CGetSetOptions::GetDBPath()"):
            self.assertIn(guard, cancellation)
        self.assertLess(cancellation.index("return;"), cancellation.index("m_preview.Cancel()"))
        destruction = listing.split("void CQListCtrl::OnDestroy()", 1)[1].split("\n}", 1)[0]
        self.assertIn("CancelPendingPreview();", destruction)
        self.assertIn("m_preview.Stop();", destruction)

    def test_read_errors_complete_only_the_matching_request(self):
        self.assertEqual(SOURCE.count(
            "::PostMessage(request.notifyWindow, WM_CLIP_PREVIEW_READY, request.generation, 1);"
        ), 2)
        handler = self.read("QListCtrl.cpp").split("LRESULT CQListCtrl::OnPreviewReady(", 1)[1].split("\n}", 1)[0]
        failure = handler.split("auto result =", 1)[0]
        self.assertIn("static_cast<UINT_PTR>(failedGeneration) == m_previewGeneration", failure)
        self.assertIn("m_previewGeneration = 0", failure)


if __name__ == "__main__":
    unittest.main(verbosity=2)
