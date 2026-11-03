"""Exercise production copy-source SQL and statically check capture/UI wiring.

Run: python -B tests/copy_source_regression.py
Uses only in-memory databases. Does not replace the MFC build or Windows UI tests.
"""

import json
import re
import sqlite3
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from search_static_checks import code_characters
from search_sql_regression import SCHEMA, execute_all


ROOT = Path(__file__).resolve().parents[1]


def source(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


def function(name, signature):
    return source(name).split(signature, 1)[1].split("\n}", 1)[0]


def literals(text):
    return [json.loads(value) for value in re.findall(r'_T\(("(?:\\.|[^"\\])*")\)', text)]


def sql_starting(text, prefix):
    return next(value for value in literals(text) if value.startswith(prefix))


def joined_sql(text, prefix):
    start = text.index('_T("' + prefix)
    parts = re.match(r'(?:_T\("(?:\\.|[^"\\])*"\)\s*)+', text[start:]).group(0)
    return "".join(literals(parts))


DB_SOURCE = source("src/DatabaseUtilities.cpp")
CLIP_SOURCE = source("src/Clip.cpp")
CREATE_MAIN = joined_sql(DB_SOURCE, "CREATE TABLE Main(")
MIGRATION = function("src/DatabaseUtilities.cpp", "static void EnsureCopySourceColumns(")
MIGRATE_FIRST = sql_starting(MIGRATION, "ALTER TABLE Main ADD COLUMN firstCopyProcess")
ADD_TO_DB = function("src/Clip.cpp", "bool CClip::AddToDB(")
UPDATE_ORDER = sql_starting(ADD_TO_DB, "UPDATE Main SET clipOrder = ? WHERE")
INSERT_TEMPLATE = joined_sql(CLIP_SOURCE, "INSERT into Main ")
# The existing numeric/description fields still use CString::Format. Substitute
# fixed fixture values, leaving the real bound source parameter intact.
INSERT = re.sub(r"%lld|%[sdf]", lambda m: "fixture" if m[0] == "%s" else "1", INSERT_TEMPLATE)


def migrate(db, fail_after_first=False):
    columns = {row[1].lower() for row in db.execute(sql_starting(MIGRATION, "PRAGMA"))}
    if "firstcopyprocess" in columns:
        return
    db.execute(sql_starting(MIGRATION, "SAVEPOINT"))
    try:
        if "firstcopyprocess" not in columns:
            db.execute(MIGRATE_FIRST)
        if fail_after_first:
            raise RuntimeError("injected migration failure")
        db.execute(sql_starting(MIGRATION, "RELEASE"))
    except Exception:
        db.execute(sql_starting(MIGRATION, "ROLLBACK"))
        db.execute(sql_starting(MIGRATION, "RELEASE"))
        raise


class CopySourceSqlTests(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.addCleanup(self.db.close)

    def create(self):
        self.db.execute(CREATE_MAIN)

    def insert(self, process="chrome.exe"):
        return self.db.execute(INSERT, (process,)).lastrowid

    def sources(self, clip_id):
        return self.db.execute(
            "SELECT firstCopyProcess FROM Main WHERE lID=?", (clip_id,)
        ).fetchone()[0]

    def legacy(self):
        self.db.execute("CREATE TABLE Main(lID INTEGER PRIMARY KEY, clipOrder REAL)")
        self.db.execute("INSERT INTO Main VALUES(1, 1)")
        self.db.commit()

    def test_new_copy_initializes_source(self):
        self.create()
        self.assertEqual(self.sources(self.insert()), "chrome.exe")
        self.assertNotIn("lastCopyProcess", CREATE_MAIN)

    def test_existing_group_insert_uses_unknown_defaults(self):
        self.create()
        clip_id = self.db.execute("INSERT INTO Main(mText, bIsGroup) VALUES('group', 1)").lastrowid
        self.assertEqual(self.sources(clip_id), "")

    def test_duplicate_preserves_original_source(self):
        self.create()
        clip_id = self.insert()
        for order in range(2, 6):
            self.db.execute(UPDATE_ORDER, (order, clip_id))
            self.assertEqual(self.sources(clip_id), "chrome.exe")
        self.assertEqual(self.db.execute("SELECT count(*) FROM Main").fetchone()[0], 1)

    def test_old_source_stays_unknown_after_recapture(self):
        self.legacy()
        migrate(self.db)
        self.assertEqual(self.sources(1), "")
        self.db.execute(UPDATE_ORDER, (2, 1))
        self.assertEqual(self.sources(1), "")

    def test_independent_duplicate_and_clone_keep_their_sources(self):
        self.create()
        first = self.insert()
        second = self.insert("code.exe")
        self.db.execute(UPDATE_ORDER, (3, first))
        clone = self.insert(self.sources(first))
        self.assertEqual(self.sources(second), "code.exe")
        self.assertEqual(self.sources(clone), "chrome.exe")

    def test_import_or_sort_preserves_existing_sources(self):
        self.create()
        clip_id = self.insert()
        self.db.execute(UPDATE_ORDER, (3, clip_id))
        self.assertEqual(self.sources(clip_id), "chrome.exe")
        self.assertEqual(self.sources(self.insert("")), "")

    def test_bound_process_names_handle_unicode_quotes_and_sql_characters(self):
        self.create()
        process = "工具'中文.exe; -- ? %"
        clip_id = self.insert(process)
        self.assertEqual(self.sources(clip_id), process)
        self.db.execute(UPDATE_ORDER, (2, clip_id))
        self.assertEqual(self.sources(clip_id), process)

    def test_migration_is_idempotent_and_does_not_update_old_rows(self):
        self.legacy()
        before = self.db.total_changes
        migrate(self.db)
        migrate(self.db)
        self.assertEqual(self.db.total_changes, before)
        self.assertEqual(self.sources(1), "")
        self.assertEqual(len(list(self.db.execute("PRAGMA table_info(Main)"))), 3)

    def test_previous_two_source_database_needs_no_rewrite(self):
        self.legacy()
        self.db.execute(MIGRATE_FIRST)
        self.db.execute("ALTER TABLE Main ADD COLUMN lastCopyProcess TEXT NOT NULL DEFAULT ''")
        self.db.execute("UPDATE Main SET firstCopyProcess='original.exe', lastCopyProcess='latest.exe'")
        before = self.db.total_changes
        migrate(self.db)
        self.assertEqual(self.db.total_changes, before)
        self.db.execute(UPDATE_ORDER, (3, 1))
        self.assertEqual(self.sources(1), "original.exe")
        self.assertEqual(self.db.execute("SELECT lastCopyProcess FROM Main").fetchone()[0], "latest.exe")

    def test_migration_rolls_back_and_can_retry(self):
        self.legacy()
        with self.assertRaisesRegex(RuntimeError, "injected"):
            migrate(self.db, fail_after_first=True)
        self.assertEqual(len(list(self.db.execute("PRAGMA table_info(Main)"))), 2)
        migrate(self.db)
        self.assertEqual(self.sources(1), "")

    def test_duplicate_update_has_no_source_data_or_fulltext_work(self):
        self.create()
        self.db.execute("CREATE TABLE Data(lParentID INTEGER, strClipBoardFormat TEXT, ooData BLOB)")
        execute_all(self.db, [sql for sql in SCHEMA if "MainFullTextLiteral" not in sql])
        clip_id = self.insert()
        self.db.execute("UPDATE MainFullTextCache SET fulltext='unchanged text' WHERE clipID=?", (clip_id,))
        accesses = []

        def authorize(action, table, column, database, trigger):
            if action in (sqlite3.SQLITE_READ, sqlite3.SQLITE_UPDATE, sqlite3.SQLITE_INSERT, sqlite3.SQLITE_DELETE):
                accesses.append((table, column, trigger))
            return sqlite3.SQLITE_OK

        before = self.db.total_changes
        self.db.set_authorizer(authorize)
        self.db.execute(UPDATE_ORDER, (2, clip_id))
        self.db.set_authorizer(None)
        self.assertEqual(self.db.total_changes - before, 1)
        self.assertTrue(accesses)
        self.assertTrue(all(table == "Main" and trigger is None for table, _, trigger in accesses), accesses)
        self.assertFalse(any("CopyProcess" in (column or "") for _, column, _ in accesses))


class CopySourceStaticChecks(unittest.TestCase):
    def test_changed_cpp_delimiters(self):
        for stem in ("Clip", "ClipboardViewer", "CopyThread", "Misc", "CopyProperties"):
            for extension in ("h", "cpp"):
                self.check_delimiters(f"src/{stem}.{extension}")
        for name in ("src/MainFrm.cpp", "src/DatabaseUtilities.cpp"):
            self.check_delimiters(name)
        self.check_delimiters("src/ClipboardSource.h")
        self.check_delimiters("tests/clipboard_source_smoke.cpp")

    def check_delimiters(self, name):
        stack = []
        for char in code_characters(source(name)):
            if char in "({[":
                stack.append(char)
            elif char in ")}]":
                self.assertTrue(stack, name)
                self.assertEqual(stack.pop(), "({["[")}]".index(char)], name)
        self.assertFalse(stack, name)

    def test_capture_and_retry_pass_the_same_event_snapshot(self):
        thread = source("src/CopyThread.cpp")
        call = "LoadFromClipboard(pSupportedTypes, true, activeWindow, activeWindowTitle, sourceProcess, sourceContext)"
        self.assertEqual(thread.count(call), 2)
        capture = function("src/Clip.cpp", "int CClip::LoadFromClipboard(")
        self.assertIn("sourceMatches = sourceContext.IsCurrent()", capture)
        self.assertIn("sourceStillMatches = sourceMatches && sourceContext.IsCurrent()", capture)
        self.assertLess(capture.index("sourceStillMatches ="), capture.index("bool calledOnCopyScript"))
        self.assertLess(capture.rindex("return -1;"), capture.index("m_copyProcess = sourceProcess;"))
        viewer = function("src/ClipboardViewer.cpp", "bool CClipboardViewer::ValidActiveWnd(")
        self.assertLess(viewer.index("m_copySourceProcess = m_activeWindow"), viewer.index("::GetForegroundWindow()"))
        self.assertEqual(viewer.count("GetClipboardSourceProcessName("), 1)
        helper = function("src/Misc.cpp", "CString GetClipboardSourceProcessName(")
        self.assertIn("GetProcessName(source.window, source.processId)", helper)
        self.assertIn("GetClipboardSourceProcessName(sourceContext, false)", source("src/MainFrm.cpp"))
        timer = function("src/ClipboardViewer.cpp", "void CClipboardViewer::OnTimer(")
        self.assertLess(timer.index("m_copySourceContext = ClipboardSourceContext()"), timer.index("m_pHandler->OnClipboardChange("))
        self.assertLess(timer.index("if (!m_hasPendingCopy)"), timer.index("m_pHandler->OnClipboardChange("))
        draw = function("src/ClipboardViewer.cpp", "void CClipboardViewer::OnDrawClipboard(")
        self.assertLess(draw.index("m_hasPendingCopy = false;"), draw.index("ValidActiveWnd()"))

    def test_import_clone_and_property_save_do_not_recapture(self):
        self.assertIn("m_copyProcess.Empty();", function("src/Clip.cpp", "void CClip::Clear("))
        self.assertIn("m_copyProcess = clip.m_copyProcess;", function("src/Clip.cpp", "const CClip& CClip::operator=("))
        modify = function("src/Clip.cpp", "bool CClip::ModifyMainTable(")
        self.assertNotIn("CopyProcess", modify)
        self.assertNotIn("CopyProcess", source("Shared/IClip.h"))
        self.assertNotIn("CopyProcess", source("src/Clip_ImportExport.cpp"))
        self.assertNotIn("lastCopyProcess", CLIP_SOURCE)

    def test_source_resources_are_unique_readonly_and_translated(self):
        header = source("resource.h")
        ids = dict(re.findall(r"#define\s+(\w+)\s+(\d+)", header))
        names = ("IDC_STATIC_COPY_PROCESS", "IDC_COPY_PROCESS")
        for name in names:
            self.assertEqual(sum(value == ids[name] for value in ids.values()), 1, name)
        dialog = source("CP_Main.rc").split("IDD_COPY_PROPERTIES DIALOGEX", 1)[1].split("\nEND", 1)[0]
        self.assertNotIn("Last Copy Source", dialog)
        self.assertNotIn("First Copy Source", dialog)
        for name in names[1::2]:
            line = next(line for line in dialog.splitlines() if name + "," in line)
            self.assertIn("ES_READONLY", line)
            self.assertIn(f"m_Resize.AddControl({name}, DR_SizeWidth)", source("src/CopyProperties.cpp"))
        for language in ("English", "Chinese Simplified"):
            root = ET.fromstring(source(f"Debug/Language/{language}.xml"))
            for name in names[::2]:
                item = root.find(f"Ditto_Clip_Properties/Item[@ID='{ids[name]}']")
                self.assertIsNotNone(item)
                self.assertTrue(item.text)
            for key in ("CopySourceUnknown", "CopySourceNotApplicable"):
                self.assertTrue(root.find(f"Ditto_String_Table/Item[@ID='{key}']").text)

    def test_context_header_is_registered(self):
        namespace = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
        for name in ("CP_Main.vcxproj", "CP_Main.vcxproj.filters"):
            project = ET.fromstring(source(name))
            headers = {node.attrib["Include"] for node in project.findall(".//m:ClInclude", namespace)}
            self.assertIn("src\\ClipboardSource.h", headers)


if __name__ == "__main__":
    unittest.main(verbosity=2)
