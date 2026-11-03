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
MIGRATE_LAST = sql_starting(MIGRATION, "ALTER TABLE Main ADD COLUMN lastCopyProcess")
ADD_TO_DB = function("src/Clip.cpp", "bool CClip::AddToDB(")
UPDATE_COPY = sql_starting(ADD_TO_DB, "UPDATE Main SET clipOrder = ?, lastCopyProcess")
UPDATE_OTHER = sql_starting(ADD_TO_DB, "UPDATE Main SET clipOrder = ? WHERE")
INSERT_TEMPLATE = joined_sql(CLIP_SOURCE, "INSERT into Main ")
# The existing numeric/description fields still use CString::Format. Substitute
# fixed fixture values, leaving the two real bound source parameters intact.
INSERT = re.sub(r"%lld|%[sdf]", lambda m: "fixture" if m[0] == "%s" else "1", INSERT_TEMPLATE)


def migrate(db, fail_after_first=False):
    columns = {row[1].lower() for row in db.execute(sql_starting(MIGRATION, "PRAGMA"))}
    if {"firstcopyprocess", "lastcopyprocess"} <= columns:
        return
    db.execute(sql_starting(MIGRATION, "SAVEPOINT"))
    try:
        if "firstcopyprocess" not in columns:
            db.execute(MIGRATE_FIRST)
        if fail_after_first:
            raise RuntimeError("injected migration failure")
        if "lastcopyprocess" not in columns:
            db.execute(MIGRATE_LAST)
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

    def insert(self, first="chrome.exe", last="chrome.exe"):
        return self.db.execute(INSERT, (first, last)).lastrowid

    def sources(self, clip_id):
        return self.db.execute(
            "SELECT firstCopyProcess, lastCopyProcess FROM Main WHERE lID=?", (clip_id,)
        ).fetchone()

    def legacy(self):
        self.db.execute("CREATE TABLE Main(lID INTEGER PRIMARY KEY, clipOrder REAL)")
        self.db.execute("INSERT INTO Main VALUES(1, 1)")
        self.db.commit()

    def test_new_copy_initializes_both_sources(self):
        self.create()
        self.assertEqual(self.sources(self.insert()), ("chrome.exe", "chrome.exe"))

    def test_existing_group_insert_uses_unknown_defaults(self):
        self.create()
        clip_id = self.db.execute("INSERT INTO Main(mText, bIsGroup) VALUES('group', 1)").lastrowid
        self.assertEqual(self.sources(clip_id), ("", ""))

    def test_duplicate_only_changes_last_source(self):
        self.create()
        clip_id = self.insert()
        for order, process in enumerate(("notepad.exe", "notepad.exe", "", "code.exe"), 2):
            self.db.execute(UPDATE_COPY, (order, process, clip_id))
            self.assertEqual(self.sources(clip_id), ("chrome.exe", process))
        self.assertEqual(self.db.execute("SELECT count(*) FROM Main").fetchone()[0], 1)

    def test_old_first_source_stays_unknown_after_recapture(self):
        self.legacy()
        migrate(self.db)
        self.assertEqual(self.sources(1), ("", ""))
        self.db.execute(UPDATE_COPY, (2, "notepad.exe", 1))
        self.assertEqual(self.sources(1), ("", "notepad.exe"))

    def test_independent_duplicate_and_clone_keep_their_sources(self):
        self.create()
        first = self.insert()
        second = self.insert("code.exe", "code.exe")
        self.db.execute(UPDATE_COPY, (3, "notepad.exe", first))
        clone = self.insert(*self.sources(first))
        self.assertEqual(self.sources(second), ("code.exe", "code.exe"))
        self.assertEqual(self.sources(clone), ("chrome.exe", "notepad.exe"))

    def test_import_or_sort_preserves_existing_sources(self):
        self.create()
        clip_id = self.insert()
        self.db.execute(UPDATE_OTHER, (3, clip_id))
        self.assertEqual(self.sources(clip_id), ("chrome.exe", "chrome.exe"))
        self.assertEqual(self.sources(self.insert("", "")), ("", ""))

    def test_bound_process_names_handle_unicode_quotes_and_sql_characters(self):
        self.create()
        process = "工具'中文.exe; -- ? %"
        clip_id = self.insert(process, process)
        self.assertEqual(self.sources(clip_id), (process, process))
        self.db.execute(UPDATE_COPY, (2, process + "2", clip_id))
        self.assertEqual(self.sources(clip_id), (process, process + "2"))

    def test_migration_is_idempotent_and_does_not_update_old_rows(self):
        self.legacy()
        before = self.db.total_changes
        migrate(self.db)
        migrate(self.db)
        self.assertEqual(self.db.total_changes, before)
        self.assertEqual(self.sources(1), ("", ""))
        self.assertEqual(len(list(self.db.execute("PRAGMA table_info(Main)"))), 4)

    def test_each_partial_migration_preserves_existing_metadata(self):
        for statement, column in ((MIGRATE_FIRST, "firstCopyProcess"), (MIGRATE_LAST, "lastCopyProcess")):
            with self.subTest(column=column):
                self.db.execute("DROP TABLE IF EXISTS Main")
                self.legacy()
                self.db.execute(statement)
                self.db.execute(f"UPDATE Main SET {column}='existing.exe'")
                migrate(self.db)
                self.assertEqual(self.db.execute(f"SELECT {column} FROM Main").fetchone()[0], "existing.exe")

    def test_migration_rolls_back_and_can_retry(self):
        self.legacy()
        with self.assertRaisesRegex(RuntimeError, "injected"):
            migrate(self.db, fail_after_first=True)
        self.assertEqual(len(list(self.db.execute("PRAGMA table_info(Main)"))), 2)
        migrate(self.db)
        self.assertEqual(self.sources(1), ("", ""))

    def test_source_update_has_no_data_or_fulltext_work(self):
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
        self.db.execute(UPDATE_COPY, (2, "notepad.exe", clip_id))
        self.db.set_authorizer(None)
        self.assertEqual(self.db.total_changes - before, 1)
        self.assertTrue(accesses)
        self.assertTrue(all(table == "Main" and trigger is None for table, _, trigger in accesses), accesses)


class CopySourceStaticChecks(unittest.TestCase):
    def test_changed_cpp_delimiters(self):
        for stem in ("Clip", "ClipboardViewer", "CopyThread", "Misc", "CopyProperties"):
            for extension in ("h", "cpp"):
                self.check_delimiters(f"src/{stem}.{extension}")
        for name in ("src/MainFrm.cpp", "src/DatabaseUtilities.cpp"):
            self.check_delimiters(name)

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
        call = "LoadFromClipboard(pSupportedTypes, true, activeWindow, activeWindowTitle, sourceProcess, sourceSequence)"
        self.assertEqual(thread.count(call), 2)
        capture = function("src/Clip.cpp", "int CClip::LoadFromClipboard(")
        self.assertIn("sourceSequence != 0 && sourceSequence == ::GetClipboardSequenceNumber()", capture)
        self.assertIn("sourceMatches && sourceSequence == ::GetClipboardSequenceNumber()", capture)
        self.assertLess(capture.rindex("return -1;"), capture.index("m_hasCopySource = true;"))
        viewer = function("src/ClipboardViewer.cpp", "bool CClipboardViewer::ValidActiveWnd(")
        self.assertLess(viewer.index("m_copySourceProcess = m_activeWindow"), viewer.index("::GetForegroundWindow()"))
        self.assertEqual(viewer.count("GetClipboardOwnerProcessName("), 1)
        helper = function("src/Misc.cpp", "CString GetClipboardOwnerProcessName(")
        self.assertNotIn("GetForegroundWindow", helper)
        self.assertIn("GetProcessName(owner, ownerProcessId)", helper)
        timer = function("src/ClipboardViewer.cpp", "void CClipboardViewer::OnTimer(")
        self.assertLess(timer.index("m_copySourceSequence = 0"), timer.index("m_pHandler->OnClipboardChange("))
        self.assertLess(timer.index("if (!m_hasPendingCopy)"), timer.index("m_pHandler->OnClipboardChange("))
        draw = function("src/ClipboardViewer.cpp", "void CClipboardViewer::OnDrawClipboard(")
        self.assertLess(draw.index("m_hasPendingCopy = false;"), draw.index("ValidActiveWnd()"))

    def test_import_clone_and_property_save_do_not_recapture(self):
        for signature in ("void CClip::Clear(", "const CClip& CClip::operator=(", "BOOL CClip::LoadMainTable("):
            self.assertIn("m_hasCopySource = false;", function("src/Clip.cpp", signature))
        modify = function("src/Clip.cpp", "bool CClip::ModifyMainTable(")
        self.assertNotIn("CopyProcess", modify)
        self.assertNotIn("CopyProcess", source("Shared/IClip.h"))
        self.assertNotIn("CopyProcess", source("src/Clip_ImportExport.cpp"))

    def test_source_resources_are_unique_readonly_and_translated(self):
        header = source("resource.h")
        ids = dict(re.findall(r"#define\s+(\w+)\s+(\d+)", header))
        names = ("IDC_STATIC_FIRST_COPY_PROCESS", "IDC_FIRST_COPY_PROCESS", "IDC_STATIC_LAST_COPY_PROCESS", "IDC_LAST_COPY_PROCESS")
        for name in names:
            self.assertEqual(sum(value == ids[name] for value in ids.values()), 1, name)
        dialog = source("CP_Main.rc").split("IDD_COPY_PROPERTIES DIALOGEX", 1)[1].split("\nEND", 1)[0]
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
