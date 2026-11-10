// DatabaseUtilites.cpp: implementation of the CDatabaseUtilites class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "CP_Main.h"
#include "DatabaseUtilities.h"
#include "ProcessPaste.h"
#include "SearchIndex.h"
#include <io.h>
#include "Path.h"
#include "zlib.h"
#include "..\Shared\TextConvert.h"
using namespace nsPath;

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////


BOOL CreateBackup(CString csPath)
{
	CString csOriginal;
	int count = 0;
	// create a backup of the existing database
	do
	{
		count++;
		csOriginal = csPath + StrF(_T(".%03d"), count);
		// in case of some weird infinite loop
		if (count > 50)
		{
			ASSERT(0);
			return FALSE;
		}
	} while (!::CopyFile(csPath, csOriginal, TRUE));

	return TRUE;
}

CString GetDBName()
{
	return CGetSetOptions::GetDBPath();
}

CString GetOLDDefaultDBName()
{
	CString csDefaultPath;
	LPMALLOC pMalloc;

	if (SUCCEEDED(::SHGetMalloc(&pMalloc)))
	{
		LPITEMIDLIST pidlPrograms;

		SHGetSpecialFolderLocation(NULL, CSIDL_APPDATA, &pidlPrograms);

		TCHAR string[MAX_PATH];
		SHGetPathFromIDList(pidlPrograms, string);

		pMalloc->Free(pidlPrograms);
		pMalloc->Release();

		csDefaultPath = string;
		csDefaultPath += "\\Ditto\\";

		csDefaultPath += "DittoDB.mdb";
	}

	return csDefaultPath;
}

CString GetDefaultDBName()
{
	CString csDefaultPath = _T("c:\\program files\\Ditto\\");

	//If portable then default to the running path
	if (CGetSetOptions::GetIsPortableDitto())
	{
		csDefaultPath.Empty();
	}
	else
	{
		csDefaultPath = CGetSetOptions::GetAppDataPath();
	}

	CString csTempName = csDefaultPath + "Ditto.db";
	int i = 1;
	while (FileExists(csTempName))
	{
		csTempName.Format(_T("%sDitto_%d.db"), csDefaultPath, i);
		i++;
	}
	csDefaultPath = csTempName;

	return csDefaultPath;
}

BOOL CheckDBExists(CString csDBPath)
{
	CPath path(csDBPath);

	//If this is the first time running this version then convert the old database to the new db
	if (csDBPath.IsEmpty())
	{
		csDBPath = GetDefaultDBName();
		CGetSetOptions::SetDBPath(csDBPath);
	}

	BOOL bRet = FALSE;
	if (FileExists(csDBPath) == FALSE)
	{
		//if the database is on a shared drive, network share or anything other than C:\ than don't create a new db
		//Ditto will wait until that drive is available
		int len = 0;
		auto rootType = path.GetRootType(&len);
		auto driveLetter = path.GetDriveLetter();

		if (rootType == ERootType::rtServerShare ||
			((rootType == ERootType::rtDriveCur || rootType == rtDriveRoot) && driveLetter >= 'A' && driveLetter != 'C'))
		{
			return FALSE;
		}

		//first try and create create a db at the same path that was selectd
		bRet = CreateDB(csDBPath);

		//if that didn't work then go back to the default location
		if (FileExists(csDBPath) == FALSE)
		{
			csDBPath = GetDefaultDBName();

			nsPath::CPath FullPath(csDBPath);
			CString csPath = FullPath.GetPath().GetStr();
			if (csPath.IsEmpty() == false && FileExists(csDBPath) == FALSE)
			{
				CreateDirectory(csPath, NULL);
			}

			CGetSetOptions::SetDBPath(csDBPath);

			bRet = CreateDB(csDBPath);
		}
	}
	else
	{
		if (ValidDB(csDBPath) == FALSE)
		{
			//Db existed but was bad
			CString csMarkAsBad;

			csMarkAsBad = csDBPath;
			csMarkAsBad.Replace(_T("."), _T("_BAD."));

			CString csPath = GetDefaultDBName();

			CString cs;
			cs.Format(_T("%s \"%s\",\n")
				_T("%s \"%s\",\n")
				_T("%s,\n")
				_T("\"%s\""),
				theApp.m_Language.GetString("Database_Format", "Unrecognized Database Format"),
				csDBPath,
				theApp.m_Language.GetString("File_Renamed", "the file will be renamed"),
				csMarkAsBad,
				theApp.m_Language.GetString("New_Database", "and a new database will be created"),
				csPath);

			AfxMessageBox(cs);

			CFile::Rename(csDBPath, csMarkAsBad);

			csDBPath = csPath;

			bRet = CreateDB(csDBPath);

			CGetSetOptions::SetDBPath(csDBPath);
		}
		else
		{
			bRet = TRUE;
		}
	}

	if (bRet)
	{
		bRet = OpenDatabase(csDBPath);
	}

	return bRet;
}

BOOL IsDatabaseOpen()
{
	return theApp.m_db.IsDatabaseOpen();
}

BOOL OpenDatabase(CString dbPath)
{
	try
	{
		CPath path(dbPath);

		int len = 0;
		auto rootType = path.GetRootType(&len);
		auto driveLetter = path.GetDriveLetter();

		theApp.m_databaseOnNetworkShare = false;
		if (rootType == ERootType::rtServerShare ||
			((rootType == ERootType::rtDriveCur || rootType == rtDriveRoot) && driveLetter >= 'A' && driveLetter != 'C'))
		{
			theApp.m_databaseOnNetworkShare = true;
		}


		if (!theApp.m_db.close())
			return FALSE;
		theApp.m_db.open(dbPath);

		theApp.m_db.setBusyTimeout(CGetSetOptions::GetDbTimeout());
		theApp.m_db.SetRegexCaseInsensitive(CGetSetOptions::GetRegexCaseInsensitive());
		if (SearchIndex::EnsureCurrent(theApp.m_db) == false)
		{
			Log(_T("SearchIndex - initialization failed, falling back to legacy search path"));
		}

		return TRUE;
	}
	CATCH_SQLITE_EXCEPTION

		return FALSE;
}

void ReOrderStickyClips(int parentID, CppSQLite3DB& db)
{
	try
	{
		Log(StrF(_T("Start of ReOrderStickyClips, ParentId %d"), parentID));

		//groups where created with 0 in these fields, fix them up if they are 0
		if (parentID == -1)
		{
			db.execDMLEx(_T("Update Main Set stickyClipOrder = -(2147483647) where bIsGroup = 1 AND stickyClipOrder = 0"));
			db.execDMLEx(_T("Update Main Set stickyClipGroupOrder = -(2147483647) where bIsGroup = 1 AND stickyClipGroupOrder = 0"));
		}

		CppSQLite3Query qGroup = db.execQueryEx(_T("SELECT lID, mText FROM Main WHERE bIsGroup = 1 AND lParentID = %d"), parentID);

		if (qGroup.eof() == false)
		{
			while (!qGroup.eof())
			{
				//Get all sticky clips at the top level or group
				CString sql = StrF(_T("SELECT lID FROM Main WHERE stickyClipOrder <> -(2147483647) AND lParentID = %d ORDER BY stickyClipOrder DESC"), parentID);
				if (parentID > -1)
				{
					sql = StrF(_T("SELECT lID FROM Main WHERE stickyClipGroupOrder <> -(2147483647) AND lParentID = %d ORDER BY stickyClipGroupOrder DESC"), parentID);
				}

				CppSQLite3Query qSticky = db.execQueryEx(sql);

				int order = 1;

				if (qSticky.eof() == false)
				{
					while (!qSticky.eof())
					{
						//set the new order
						if (parentID > -1)
						{
							db.execDMLEx(_T("Update Main Set stickyClipGroupOrder = %d where lID = %d"), order, qSticky.getIntField(_T("lID")));
						}
						else
						{
							db.execDMLEx(_T("Update Main Set stickyClipOrder = %d where lID = %d"), order, qSticky.getIntField(_T("lID")));
						}

						qSticky.nextRow();
						order--;
					}
				}

				ReOrderStickyClips(qGroup.getIntField(_T("lID")), db);

				qGroup.nextRow();
			}
		}

		Log(StrF(_T("End of ReOrderStickyClips, ParentId %d"), parentID));
	}
	CATCH_SQLITE_EXCEPTION
}

static void EnsureCopySourceColumns(CppSQLite3DB& db)
{
	bool hasFirstCopyProcess = false;
	{
		CppSQLite3Query columns = db.execQuery(_T("PRAGMA table_info(Main);"));
		while (!columns.eof())
		{
			CString name = columns.getStringField(_T("name"));
			hasFirstCopyProcess |= name.CompareNoCase(_T("firstCopyProcess")) == 0;
			columns.nextRow();
		}
	}
	if (hasFirstCopyProcess)
	{
		return;
	}

	// Constant defaults preserve old records as unknown without rewriting them.
	db.execDML(_T("SAVEPOINT CopySourceMigration;"));
	try
	{
		if (!hasFirstCopyProcess)
		{
			db.execDML(_T("ALTER TABLE Main ADD COLUMN firstCopyProcess TEXT NOT NULL DEFAULT '';"));
		}
		db.execDML(_T("RELEASE CopySourceMigration;"));
	}
	catch (...)
	{
		db.execDML(_T("ROLLBACK TO CopySourceMigration;"));
		db.execDML(_T("RELEASE CopySourceMigration;"));
		throw;
	}
}

static void EnsureStickyOrderMigration(CppSQLite3DB& db)
{
	db.execDML(_T("CREATE TABLE IF NOT EXISTS DittoSchemaMigrations(name TEXT PRIMARY KEY NOT NULL);"));
	if (db.execScalar(_T("SELECT EXISTS(SELECT 1 FROM DittoSchemaMigrations WHERE name = 'sticky-order-v1');")))
		return;

	CppSQLite3Transaction transaction(db);
	// Current sorting indexes, or the historical index, prove the old data
	// conversion already ran. In particular, preserve valid zero sort positions.
	const bool alreadyConverted = db.execScalar(_T("SELECT EXISTS(SELECT 1 FROM sqlite_master ")
		_T("WHERE type = 'index' AND name IN ('Main_NoGroup', 'Main_TopLevel', 'Main_InGroup2'));")) != 0;
	if (!alreadyConverted)
	{
		db.execDML(_T("UPDATE Main SET stickyClipOrder = -(2147483647) WHERE stickyClipOrder IS NULL OR stickyClipOrder = 0;"));
		db.execDML(_T("UPDATE Main SET stickyClipGroupOrder = -(2147483647) WHERE stickyClipGroupOrder IS NULL OR stickyClipGroupOrder = 0;"));
	}
	db.execDML(_T("INSERT INTO DittoSchemaMigrations(name) VALUES('sticky-order-v1');"));
	transaction.commit();
}

static void EnsureLookupIndexes(CppSQLite3DB& db)
{
	// Keep the parent-first index for loading all formats of one clip. Format
	// filters use BINARY equality and need the opposite column order as well.
	db.execDML(_T("CREATE INDEX IF NOT EXISTS Data_ParentId_Format ON Data(lParentID COLLATE BINARY ASC, strClipBoardFormat COLLATE NOCASE ASC);"));
	db.execDML(_T("CREATE INDEX IF NOT EXISTS Data_Format_ParentId ON Data(strClipBoardFormat COLLATE BINARY, lParentID);"));
	db.execDML(_T("CREATE INDEX IF NOT EXISTS MainDeletes_ClipId ON MainDeletes(clipID);"));
}

BOOL ValidDB(CString csPath, BOOL bUpgrade)
{
	try
	{
		CppSQLite3DB db;
		db.open(csPath);

		db.execQuery(_T("SELECT lID, lDate, mText, lShortCut, lDontAutoDelete, ")
			_T("CRC, bIsGroup, lParentID, QuickPasteText ")
			_T("FROM Main"));

		db.execQuery(_T("SELECT lID, lParentID, strClipBoardFormat, ooData FROM Data"));

		db.execQuery(_T("SELECT lID, TypeText FROM Types"));

		try
		{
			db.execDML(_T("DROP TRIGGER delete_data_trigger"));
		}
		catch (CppSQLite3Exception& e)
		{
			e.errorCode();
		}

		try
		{
			db.execDML(_T("DROP TRIGGER delete_copy_buffer_trigger"));
		}
		catch (CppSQLite3Exception& e)
		{
			e.errorCode();
		}

		//This was added later so try to add each time and catch the exception here
		try
		{
			db.execDML(_T("CREATE TRIGGER delete_data_trigger BEFORE DELETE ON Main FOR EACH ROW\n")
				_T("BEGIN\n")
				_T("INSERT INTO MainDeletes VALUES(old.lID, datetime('now'));\n")
				_T("END\n"));
		}
		catch (CppSQLite3Exception& e)
		{
			e.errorCode();
		}

		//This was added later so try to add each time and catch the exception here
		try
		{
			db.execQuery(_T("SELECT lID, lClipID, lCopyBuffer FROM CopyBuffers"));
		}
		catch (CppSQLite3Exception& e)
		{
			e.errorCode();

			db.execDML(_T("CREATE TABLE CopyBuffers(")
				_T("lID INTEGER PRIMARY KEY AUTOINCREMENT, ")
				_T("lClipID INTEGER,")
				_T("lCopyBuffer INTEGER)"));
		}

		//This was added later so try to add each time and catch the exception here
		try
		{
			db.execQuery(_T("SELECT clipId FROM MainDeletes"));
		}
		catch (CppSQLite3Exception& e)
		{
			e.errorCode();

			db.execDML(_T("CREATE TABLE MainDeletes(")
				_T("clipID INTEGER,")
				_T("modifiedDate)"));

			db.execDML(_T("CREATE TRIGGER MainDeletes_delete_data_trigger BEFORE DELETE ON MainDeletes FOR EACH ROW\n")
				_T("BEGIN\n")
				_T("DELETE FROM CopyBuffers WHERE lClipID = old.clipID;\n")
				_T("DELETE FROM Data WHERE lParentID = old.clipID;\n")
				_T("END\n"));
		}

		try
		{
			db.execDML(_T("CREATE INDEX Main_ParentId on Main(lParentID DESC)"));
			db.execDML(_T("CREATE INDEX Main_IsGroup on Main(bIsGroup DESC)"));
			db.execDML(_T("CREATE INDEX Main_ShortCut on Main(lShortCut DESC)"));
		}
		catch (CppSQLite3Exception& e)
		{
			e.errorCode();
		}

		try
		{
			db.execQuery(_T("SELECT clipOrder, clipGroupOrder FROM Main"));
		}
		catch (CppSQLite3Exception& e)
		{
			db.execDML(_T("ALTER TABLE Main ADD clipOrder REAL"));
			db.execDML(_T("ALTER TABLE Main ADD clipGroupOrder REAL"));

			db.execDML(_T("Update Main set clipOrder = lDate, clipGroupOrder = lDate"));

			db.execDML(_T("CREATE INDEX Main_ClipOrder on Main(clipOrder DESC)"));
			db.execDML(_T("CREATE INDEX Main_ClipGroupOrder on Main(clipGroupOrder DESC)"));

			db.execDML(_T("DROP INDEX Main_Date"));

			e.errorCode();
		}

		try
		{
			db.execQuery(_T("SELECT globalShortCut FROM Main"));
		}
		catch (CppSQLite3Exception& e)
		{
			db.execDML(_T("ALTER TABLE Main ADD globalShortCut INTEGER"));

			e.errorCode();
		}

		try
		{
			db.execQuery(_T("SELECT lastPasteDate FROM Main"));
		}
		catch (CppSQLite3Exception& e)
		{
			db.execDML(_T("ALTER TABLE Main ADD lastPasteDate INTEGER"));
			db.execDML(_T("Update Main set lastPasteDate = lDate"));
			db.execDMLEx(_T("Update Main set lastPasteDate = %d where lastPasteDate <= 0"), (int)CTime::GetCurrentTime().GetTime());

			e.errorCode();
		}

		try
		{
			db.execQuery(_T("SELECT stickyClipOrder FROM Main"));
		}
		catch (CppSQLite3Exception& e)
		{
			db.execDML(_T("ALTER TABLE Main ADD stickyClipOrder REAL"));
			db.execDML(_T("ALTER TABLE Main ADD stickyClipGroupOrder REAL"));

			e.errorCode();
		}

		EnsureStickyOrderMigration(db);
		EnsureLookupIndexes(db);

		try
		{
			db.execQuery(_T("SELECT MoveToGroupShortCut FROM Main"));
			db.execQuery(_T("SELECT GlobalMoveToGroupShortCut FROM Main"));
		}
		catch (CppSQLite3Exception& e)
		{
			db.execDML(_T("ALTER TABLE Main ADD MoveToGroupShortCut INTEGER"));
			db.execDML(_T("ALTER TABLE Main ADD GlobalMoveToGroupShortCut INTEGER"));

			e.errorCode();
		}

		EnsureCopySourceColumns(db);

		db.execDML(_T("DROP INDEX IF EXISTS Main_NoGroup"));
		db.execDML(_T("DROP INDEX IF EXISTS Main_InGroup"));
		db.execDML(_T("DROP INDEX IF EXISTS Main_ShortCut"));

		db.execDML(_T("CREATE INDEX IF NOT EXISTS Main_TopLevelParentID ON Main(lParentId ASC, stickyClipOrder DESC, bIsGroup ASC, clipOrder DESC);"));
		db.execDML(_T("CREATE INDEX IF NOT EXISTS Main_TopLevel ON Main(stickyClipOrder DESC, bIsGroup ASC, clipOrder DESC);"));
		db.execDML(_T("CREATE INDEX IF NOT EXISTS Main_InGroup2 ON Main(lParentId ASC, stickyClipGroupOrder DESC, bIsGroup ASC, clipGroupOrder DESC);"));

		db.execDML(_T("CREATE INDEX IF NOT EXISTS Main_ShortCut2 on Main(lShortCut DESC, globalShortCut DESC)"));
		db.execDML(_T("CREATE INDEX IF NOT EXISTS Main_MoveToGroup on Main(MoveToGroupShortCut DESC, GlobalMoveToGroupShortCut DESC)"));
		db.execDML(_T("CREATE INDEX IF NOT EXISTS Main_CRC on Main(CRC ASC)"));
	}
	CATCH_SQLITE_EXCEPTION_AND_RETURN(FALSE)

		return TRUE;
}

BOOL BackupDB(CString dbPath, CString backupPath)
{
	CRect r = DefaultMonitorRect();
	CPopup status((r.right - 500), r.bottom - 100, ::GetForegroundWindow());

	CString msg = theApp.m_Language.GetString("BackupDbMsg", "Backing up database");

	status.Show(StrF(_T("Ditto - %s - %s"), msg, backupPath));

	BOOL ret = FALSE;

	Log(StrF(_T("Start backing up db, from: %s to %s"), dbPath, backupPath));

	CString errorMessage = _T("");

	try
	{
		CFile file;
		CFileException ex;
		if (file.Open(dbPath, CFile::modeRead | CFile::typeBinary | CFile::shareDenyNone, &ex))
		{
			ULONGLONG fileSize = file.GetLength();
			ULONGLONG totalReadSize = 0;
			int percentageComplete = 0;
			UINT readBytes = 0;
			char* pBuffer = new char[65536];
			if (pBuffer != NULL)
			{
				gzFile f = gzopen(CTextConvert::UnicodeToAnsi(backupPath), "w");

				if (f != NULL)
				{
					do
					{
						readBytes = file.Read(pBuffer, 65536);
						gzwrite(f, pBuffer, readBytes);
						totalReadSize += readBytes;

						int percent = (int)((totalReadSize * 100) / fileSize);
						if (percent != percentageComplete)
						{
							percentageComplete = percent;
							Log(StrF(_T("backing up db percent done: %d"), percentageComplete));

							status.Show(StrF(_T("Ditto - %02d%% %s - %s"), percentageComplete, msg, backupPath));
						}

					} while (readBytes >= 65536);

					gzclose(f);

					ret = TRUE;
				}
			}

			file.Close();
		}
		else
		{
			TCHAR szCause[255];
			ex.GetErrorMessage(szCause, 255);
			errorMessage = szCause;
		}
	}
	catch (...)
	{

	}

	if (errorMessage != _T(""))
	{
		CString cs;
		cs.Format(_T("Restore ERROR: %s"), errorMessage);
		::SendMessage(theApp.m_MainhWnd, WM_SHOW_ERROR_MSG, (WPARAM)cs.GetBuffer(cs.GetLength()), 0);
		cs.ReleaseBuffer();
	}

	Log(StrF(_T("Done restoring db, from: %s, errors: %s"), backupPath, errorMessage));

	return ret;
}

BOOL RestoreDB(CString backupPath)
{
	CRect r = DefaultMonitorRect();
	CPopup status((r.right - 500), r.bottom - 100, ::GetForegroundWindow());

	CString msg = theApp.m_Language.GetString("RestoreDbMsg", "Restoring database");
	status.Show(StrF(_T("Ditto - %s - %s"), msg, backupPath));

	BOOL ret = FALSE;

	Log(StrF(_T("Start restoring db, from: %s"), backupPath));

	CString errorMessage = _T("");

	using namespace nsPath;
	CPath backupPathPath(backupPath);

	CString tempPath = CGetSetOptions::GetPath(PATH_RESTORE_TEMP);

	tempPath += backupPathPath.GetName();

	try
	{
		gzFile f = gzopen(CTextConvert::UnicodeToAnsi(backupPath), "r");
		if (f != NULL)
		{
			CFile file;
			CFileException ex;
			if (file.Open(tempPath, CFile::bufferWrite | CFile::modeCreate, &ex))
			{
				ULONGLONG totalReadSize = 0;
				int readBytes = 0;
				char* pBuffer = new char[65536];
				int percentageComplete = 0;

				do
				{
					readBytes = gzread(f, pBuffer, 65536);
					file.Write(pBuffer, readBytes);

					totalReadSize += readBytes;

					Log(StrF(_T("restoring db uncompressed bytes read: %d"), readBytes));

				} while (readBytes >= 65536);

				file.Close();
			}
			else
			{
				//errorMessage.Format(_T("Failed to open temp file %s, exception: %s"), tempPath, ex.GetErrorMessage());
			}

			gzclose(f);

			if (ValidDB(tempPath, true))
			{
				CString defaultDbPath = GetDefaultDBName();
				CPath defaultDbPathPath(defaultDbPath);

				CString path = defaultDbPathPath.GetPath();

				backupPathPath.RenameExtension(_T("db"));
				CString newFullPath = path + backupPathPath.GetName();

				int i = 1;
				while (FileExists(newFullPath))
				{
					newFullPath.Format(_T("%s%s_%d.db"), path, backupPathPath.GetTitle(), i);
					i++;
				}

				if (MoveFile(tempPath, newFullPath))
				{
					CGetSetOptions::SetDBPath(newFullPath);
					OpenDatabase(newFullPath);

					ret = TRUE;
				}
				else
				{
					errorMessage.Format(_T("Failed to copy file %s to %s"), tempPath, newFullPath);
				}
			}
			else
			{
				errorMessage.Format(_T("Unpacked database is not a valid Ditto database"));
			}
		}
		else
		{
			errorMessage.Format(_T("Failed to open file %s"), tempPath);
		}
	}
	catch (CFileException* pEx)
	{
		TCHAR cause[255];
		pEx->GetErrorMessage(cause, 255);
		errorMessage.Format(_T("Exception: %s"), cause);
	}
	catch (...)
	{
		errorMessage.Format(_T("Exception: ... catch"));
	}

	if (errorMessage != _T(""))
	{
		CString cs;
		cs.Format(_T("Restore ERROR: %s"), errorMessage);
		::SendMessage(theApp.m_MainhWnd, WM_SHOW_ERROR_MSG, (WPARAM)cs.GetBuffer(cs.GetLength()), 0);
		cs.ReleaseBuffer();
	}

	Log(StrF(_T("Done restoring db, from: %s, error: %s"), backupPath, errorMessage));

	theApp.RefreshView();

	return ret;
}

BOOL CreateDB(CString csFile)
{
	try
	{
		CppSQLite3DB db;
		db.open(csFile);

		db.execDML(_T("PRAGMA auto_vacuum = 1"));

		db.execDML(_T("CREATE TABLE Main(")
			_T("lID INTEGER PRIMARY KEY AUTOINCREMENT, ")
			_T("lDate INTEGER, ")
			_T("mText TEXT, ")
			_T("lShortCut INTEGER, ")
			_T("lDontAutoDelete INTEGER, ")
			_T("CRC INTEGER, ")
			_T("bIsGroup INTEGER, ")
			_T("lParentID INTEGER, ")
			_T("QuickPasteText TEXT, ")
			_T("clipOrder REAL, ")
			_T("clipGroupOrder REAL, ")
			_T("globalShortCut INTEGER, ")
			_T("lastPasteDate INTEGER, ")
			_T("stickyClipOrder REAL, ")
			_T("stickyClipGroupOrder REAL, ")
			_T("MoveToGroupShortCut INTEGER, ")
			_T("GlobalMoveToGroupShortCut INTEGER, ")
			_T("firstCopyProcess TEXT NOT NULL DEFAULT '');"));

		db.execDML(_T("CREATE TABLE Data(")
			_T("lID INTEGER PRIMARY KEY AUTOINCREMENT, ")
			_T("lParentID INTEGER, ")
			_T("strClipBoardFormat TEXT, ")
			_T("ooData BLOB);"));

		db.execDML(_T("CREATE TABLE Types(")
			_T("lID INTEGER PRIMARY KEY AUTOINCREMENT, ")
			_T("TypeText TEXT);"));

		db.execDML(_T("CREATE UNIQUE INDEX Main_ID on Main(lID ASC)"));
		db.execDML(_T("CREATE UNIQUE INDEX Data_ID on Data(lID ASC)"));
		db.execDML(_T("CREATE INDEX Main_ClipOrder on Main(clipOrder DESC)"));
		db.execDML(_T("CREATE INDEX Main_ClipGroupOrder on Main(clipGroupOrder DESC)"));
		db.execDML(_T("CREATE INDEX Main_ParentId on Main(lParentID DESC)"));
		db.execDML(_T("CREATE INDEX Main_IsGroup on Main(bIsGroup DESC)"));

		db.execDML(_T("CREATE TRIGGER delete_data_trigger BEFORE DELETE ON Main FOR EACH ROW\n")
			_T("BEGIN\n")
			_T("INSERT INTO MainDeletes VALUES(old.lID, datetime('now'));\n")
			_T("END\n"));

		db.execDML(_T("CREATE TABLE CopyBuffers(")
			_T("lID INTEGER PRIMARY KEY AUTOINCREMENT, ")
			_T("lClipID INTEGER, ")
			_T("lCopyBuffer INTEGER)"));

		db.execDML(_T("CREATE TABLE MainDeletes(")
			_T("clipID INTEGER,")
			_T("modifiedDate)"));

		db.execDML(_T("CREATE TRIGGER MainDeletes_delete_data_trigger BEFORE DELETE ON MainDeletes FOR EACH ROW\n")
			_T("BEGIN\n")
			_T("DELETE FROM CopyBuffers WHERE lClipID = old.clipID;\n")
			_T("DELETE FROM Data WHERE lParentID = old.clipID;\n")
			_T("END\n"));

		db.execDML(_T("CREATE INDEX Data_ParentId_Format ON Data(lParentID COLLATE BINARY ASC, strClipBoardFormat COLLATE NOCASE ASC);"));

		db.execDML(_T("CREATE INDEX IF NOT EXISTS Main_TopLevelParentID ON Main(lParentId ASC, stickyClipOrder DESC, bIsGroup ASC, clipOrder DESC);"));
		db.execDML(_T("CREATE INDEX IF NOT EXISTS Main_TopLevel ON Main(stickyClipOrder DESC, bIsGroup ASC, clipOrder DESC);"));
		db.execDML(_T("CREATE INDEX IF NOT EXISTS Main_InGroup2 ON Main(lParentId ASC, stickyClipGroupOrder DESC, bIsGroup ASC, clipGroupOrder DESC);"));

		db.execDML(_T("CREATE INDEX IF NOT EXISTS Main_ShortCut2 on Main(lShortCut DESC, globalShortCut DESC)"));
		db.execDML(_T("CREATE INDEX IF NOT EXISTS Main_MoveToGroup on Main(MoveToGroupShortCut DESC, GlobalMoveToGroupShortCut DESC)"));
		db.execDML(_T("CREATE INDEX IF NOT EXISTS Main_CRC on Main(CRC ASC)"));

		EnsureStickyOrderMigration(db);
		EnsureLookupIndexes(db);

		db.close();
	}
	CATCH_SQLITE_EXCEPTION_AND_RETURN(FALSE)

		return TRUE;
}

BOOL CompactDatabase()
{
	//	if(!theApp.CloseDB())
	//		return FALSE;
	//
	//	CString csDBName = GetDBName();
	//	CString csTempDBName = csDBName;
	//	csTempDBName.Replace(".mdb", "TempDBName.mdb");
	//	
	//	//Compact the database			
	//	try
	//	{
	//		CDaoWorkspace::CompactDatabase(csDBName, csTempDBName);//, dbLangGeneral, 0, "andrew");//DATABASE_PASSWORD);
	//	}
	//	catch(CDaoException* e)
	//	{
	//		AfxMessageBox(e->m_pErrorInfo->m_strDescription);
	//		DeleteFile(csTempDBName);
	//		e->Delete();
	//		return FALSE;
	//	}
	//	catch(CMemoryException* e) 
	//	{
	//		AfxMessageBox("Memory Exception");
	//		DeleteFile(csTempDBName);
	//		e->Delete();
	//		return FALSE;
	//	}
	//	
	//	//Since compacting the database creates a new db delete the old one and replace it
	//	//with the compacted db
	//	if(DeleteFile(csDBName))
	//	{
	//		try
	//		{
	//			CFile::Rename(csTempDBName, csDBName);
	//		}
	//		catch(CFileException *e)
	//		{
	//			e->ReportError();
	//			e->Delete();
	//			return FALSE;
	//		}
	//	}
	//	else
	//		AfxMessageBox("Error Compacting Database");

	return TRUE;
}

BOOL RepairDatabase()
{
	//	if(!theApp.CloseDB())
	//		return FALSE;

	//	try
	//	{
	//		CDaoWorkspace::RepairDatabase(GetDBName());
	//	}
	//	catch(CDaoException *e)
	//	{
	//		AfxMessageBox(e->m_pErrorInfo->m_strDescription);
	//		e->Delete();
	//		return FALSE;
	//	}

	return TRUE;
}

BOOL RemoveOldEntries(bool checkIdleTime, const std::function<bool()>& shouldYield, bool* moreWork)
{
	if (moreWork != nullptr)
		*moreWork = false;
	// Background maintenance gets short batches and yields to incoming copies.
	// Manual cleanup retains its existing complete Main-table cleanup behavior.
	const DWORD started = GetTickCount();
	auto interrupted = [&]() { return shouldYield && shouldYield(); };
	auto yieldBeforeBatch = [&]()
	{
		if (checkIdleTime && IdleSeconds() <= CGetSetOptions::GetIdleSecondsBeforeDelete())
			return true;
		if (interrupted() || (checkIdleTime && GetTickCount() - started >= 100))
		{
			if (moreWork != nullptr)
				*moreWork = true;
			return true;
		}
		return false;
	};
	if (yieldBeforeBatch())
		return TRUE;

	try
	{
		CppSQLite3DB db;
		db.open(CGetSetOptions::GetDBPath());
		if (checkIdleTime)
			db.setBusyTimeout(100);

		auto deleteMainBatches = [&](const CString& sql)
		{
			while (!yieldBeforeBatch())
			{
				CClipIDs ids;
				{
					CppSQLite3Query q = db.execQuery(sql);
					while (!q.eof())
					{
						ids.Add(q.getIntField(_T("lID")));
						q.nextRow();
					}
				}
				if (ids.GetCount() == 0)
					return true;
				if (interrupted())
				{
					if (moreWork != nullptr)
						*moreWork = true;
					return false;
				}
				// Finish one bounded batch even if its SELECT used the time budget,
				// so a large protected history cannot prevent all forward progress.
				if (!ids.DeleteIDs(false, db))
					return false;
			}
			return false;
		};

		if (CGetSetOptions::GetCheckForMaxEntries() && CGetSetOptions::GetMaxEntries() >= 0)
		{
			// Apply OFFSET before the protection filter: retained groups/shortcuts
			// still count toward the configured history size, as before.
			CString sql = StrF(_T("SELECT lID FROM (SELECT lID, lShortCut, lParentID, lDontAutoDelete, stickyClipOrder, stickyClipGroupOrder ")
				_T("FROM Main WHERE bIsGroup = 0 ORDER BY clipOrder DESC LIMIT -1 OFFSET %d) ")
				_T("WHERE COALESCE(lShortCut, 0) = 0 AND COALESCE(lDontAutoDelete, 0) = 0 AND COALESCE(lParentID, 0) <= 0 ")
				_T("AND stickyClipOrder = -(2147483647) AND stickyClipGroupOrder = -(2147483647) LIMIT 25;"),
				CGetSetOptions::GetMaxEntries());
			if (!deleteMainBatches(sql))
				return TRUE;
		}

		if (CGetSetOptions::GetCheckForExpiredEntries() && CGetSetOptions::GetExpiredEntries())
		{
			CTime cutoff = CTime::GetCurrentTime() - CTimeSpan(CGetSetOptions::GetExpiredEntries(), 0, 0, 0);
			CString sql = StrF(_T("SELECT lID FROM Main WHERE lastPasteDate < %d AND ")
				_T("bIsGroup = 0 AND lShortCut = 0 AND lParentID <= 0 AND lDontAutoDelete = 0 ")
				_T("AND stickyClipOrder = -(2147483647) AND stickyClipGroupOrder = -(2147483647) LIMIT 25;"),
				(int)cutoff.GetTime());
			if (!deleteMainBatches(sql))
				return TRUE;
		}

		if (yieldBeforeBatch())
			return TRUE;
		int deleteLimit = CGetSetOptions::GetMainDeletesDeleteCount();
		if (checkIdleTime && (deleteLimit < 0 || deleteLimit > 100))
			deleteLimit = 100;
		std::vector<int> pendingDeletes;
		{
			CppSQLite3Query q = db.execQueryEx(_T("SELECT clipID FROM MainDeletes LIMIT %d;"), deleteLimit);
			while (!q.eof())
			{
				pendingDeletes.push_back(q.getIntField(_T("clipID")));
				q.nextRow();
			}
		}
		for (int clipId : pendingDeletes)
		{
			if (yieldBeforeBatch())
				return TRUE;
			db.execDMLEx(_T("DELETE FROM MainDeletes WHERE clipID=%d"), clipId);
		}
		if (checkIdleTime && moreWork != nullptr && !pendingDeletes.empty())
			*moreWork = db.execScalar(_T("SELECT EXISTS(SELECT 1 FROM MainDeletes);")) != 0;
	}
	catch (CppSQLite3Exception& e)
	{
		// A busy writer should take priority; retry maintenance after saving.
		if (checkIdleTime && (e.errorCode() == SQLITE_BUSY || e.errorCode() == SQLITE_LOCKED))
		{
			if (moreWork != nullptr)
				*moreWork = true;
			return TRUE;
		}
		Log(StrF(_T("RemoveOldEntries failed: %s"), e.errorMessage()));
		return FALSE;
	}
	return TRUE;
}

BOOL DeleteNonUsedClips(bool fromAppWindow)
{
	Log(_T("Start of delete all non used clips"));
	CClipIDs IDs;

	CppSQLite3Query q = theApp.m_db.execQueryEx(_T("SELECT lID FROM Main WHERE bIsGroup = 0 AND lShortCut = 0 AND lParentID <= 0 AND lDontAutoDelete = 0 AND stickyClipOrder = -(2147483647) AND stickyClipGroupOrder = -(2147483647)"));

	while (q.eof() == false)
	{
		IDs.Add(q.getIntField(_T("lID")));

		Log(StrF(_T("From Clips DeleteNonUsedClips - Deleting Id: %d"), q.getIntField(_T("lID"))));

		q.nextRow();
	}

	int clipsDeleted = (int)IDs.GetCount();
	int deletedTableCount = 0;

	if (IDs.GetCount() > 0)
	{
		IDs.DeleteIDs(fromAppWindow, theApp.m_db);

		deletedTableCount = theApp.m_db.execDMLEx(_T("DELETE FROM MainDeletes"));
	}

	Log(StrF(_T("End of delete all non used clips, clips deleted: %d, delete table delted: %d"), clipsDeleted, deletedTableCount));

	return TRUE;
}

BOOL EnsureDirectory(CString csPath)
{
	TCHAR drive[_MAX_DRIVE];
	TCHAR dir[_MAX_DIR];
	TCHAR fname[_MAX_FNAME];
	TCHAR ext[_MAX_EXT];

	SPLITPATH(csPath, drive, dir, fname, ext);

	CString csDir(drive);
	csDir += dir;

	if (FileExists(csDir) == FALSE)
	{
		if (CreateDirectory(csDir, NULL))
			return TRUE;
	}
	else
		return TRUE;

	return FALSE;
}

// BOOL RunZippApp(CString csCommandLine)
// {
// 	CString csLocalPath = GETENV(_T("U3_HOST_EXEC_PATH"));
// 	FIX_CSTRING_PATH(csLocalPath);
// 
// 	CString csZippApp = GETENV(_T("U3_DEVICE_EXEC_PATH"));
// 	FIX_CSTRING_PATH(csZippApp);
// 	csZippApp += "7za.exe";
// 
// 	csZippApp += " ";
// 	csZippApp += csCommandLine;
// 
// 	Log(csZippApp);
// 
// 	STARTUPINFO			StartupInfo;
// 	PROCESS_INFORMATION	ProcessInformation;
// 
// 	ZeroMemory(&StartupInfo, sizeof(StartupInfo));
// 	StartupInfo.cb = sizeof(StartupInfo);
// 	ZeroMemory(&ProcessInformation, sizeof(ProcessInformation));
// 
// 	StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
// 	StartupInfo.wShowWindow = SW_HIDE;
// 
// 	BOOL bRet = CreateProcess(NULL, csZippApp.GetBuffer(csZippApp.GetLength()), NULL, NULL, FALSE,
// 			CREATE_DEFAULT_ERROR_MODE | NORMAL_PRIORITY_CLASS, NULL, csLocalPath,
// 			&StartupInfo, &ProcessInformation);
// 
// 	if(bRet)
// 	{
// 		WaitForSingleObject(ProcessInformation.hProcess, INFINITE);
// 
// 		DWORD dwExitCode;
// 		GetExitCodeProcess(ProcessInformation.hProcess, &dwExitCode);
// 
// 		CString cs;
// 		cs.Format(_T("Exit code from unzip = %d"), dwExitCode);
// 		Log(cs);
// 
// 		if(dwExitCode != 0)
// 		{
// 			bRet = FALSE;
// 		}
// 	}
// 	else
// 	{
// 		bRet = FALSE;
// 		Log(_T("Create Process Failed"));
// 	}
// 
// 	csZippApp.ReleaseBuffer();
// 
// 	return bRet;
// }

// BOOL CopyDownDatabase()
// {
// 	BOOL bRet = FALSE;
// 
// 	CString csZippedPath = GETENV(_T("U3_APP_DATA_PATH"));
// 	FIX_CSTRING_PATH(csZippedPath);
// 	
// 	CString csUnZippedPath = csZippedPath;
// 	csUnZippedPath += "Ditto.db";
// 
// 	csZippedPath += "Ditto.7z";
// 	
// 	CString csLocalPath = GETENV(_T("U3_HOST_EXEC_PATH"));
// 	FIX_CSTRING_PATH(csLocalPath);
// 
// 	if(FileExists(csZippedPath))
// 	{
// 		CString csCommandLine;
// 
// 		//e = extract
// 		//surround command line arguments with quotes
// 		//-aoa = overight files with extracted files
// 
// 		csCommandLine += "e ";
// 		csCommandLine += "\"";
// 		csCommandLine += csZippedPath;
// 		csCommandLine += "\"";
// 		csCommandLine += " -o";
// 		csCommandLine += "\"";
// 		csCommandLine += csLocalPath;
// 		csCommandLine += "\"";
// 		csCommandLine += " -aoa";
// 
// 		bRet = RunZippApp(csCommandLine);
// 
// 		csLocalPath += "Ditto.db";
// 	}
// 	else if(FileExists(csUnZippedPath))
// 	{
// 		csLocalPath += "Ditto.db";
// 		bRet = CopyFile(csUnZippedPath, csLocalPath, FALSE);
// 	}
// 
// 	if(FileExists(csLocalPath) == FALSE)
// 	{
// 		Log(_T("Failed to copy files from device zip file"));
// 	}
// 
// 	CGetSetOptions::nLastDbWriteTime = GetLastWriteTime(csLocalPath);
// 
// 	return bRet;
// }

//BOOL CopyUpDatabase()
//{
//	CStringA csZippedPath = "C:\\";//getenv("U3_APP_DATA_PATH");
//	FIX_CSTRING_PATH(csZippedPath);
//	csZippedPath += "Ditto.zip";

//	CStringA csLocalPath = GetDBName();//getenv("U3_HOST_EXEC_PATH");
//	//FIX_CSTRING_PATH(csLocalPath);
//	//csLocalPath += "Ditto.db";
//
//	CZipper Zip;
//
//	if(Zip.OpenZip(csZippedPath))
//	{
//		Zip.AddFileToZip(csLocalPath);
//	}
//
//	return TRUE;
//}
