#include "stdafx.h"
#include "SearchIndex.h"

#include "SearchIndexSql.h"
#include "Clip.h"
#include "Misc.h"

namespace
{
	bool g_searchIndexReady = false;

	CString ToCString(const std::string &text)
	{
		return CString(text.c_str());
	}

	bool EnsureSchema(CppSQLite3DB &db)
	{
		for (const auto &sql : SearchIndexSql::SchemaStatementsUtf8())
		{
			db.execDML(ToCString(sql));
		}

		return true;
	}

	bool NeedsRebuild(CppSQLite3DB &db)
	{
		const int version = db.execScalar(_T("SELECT version FROM MainSearchMeta LIMIT 1"));
		const int mainCount = db.execScalar(_T("SELECT COUNT(lID) FROM Main"));
		const int cacheCount = db.execScalar(_T("SELECT COUNT(clipID) FROM MainSearchCache"));

		return version != SearchIndexSql::kCurrentVersion || mainCount != cacheCount;
	}

	bool Rebuild(CppSQLite3DB &db)
	{
		Log(_T("SearchIndex - rebuilding search cache"));

		bool inTransaction = false;

		try
		{
			db.execDML(_T("begin transaction;"));
			inTransaction = true;

			db.execDML(_T("DELETE FROM MainSearchCache;"));
			db.execDML(_T("INSERT INTO MainSearchCache(clipID, description, quickpaste, fulltext) ")
				_T("SELECT lID, COALESCE(mText, ''), COALESCE(QuickPasteText, ''), '' FROM Main;"));

			CppSQLite3Statement updateStmt = db.compileStatement(_T("UPDATE MainSearchCache SET fulltext = ? WHERE clipID = ?;"));
			CppSQLite3Query q = db.execQuery(_T("SELECT lID FROM Main WHERE bIsGroup = 0"));

			int updated = 0;
			while (q.eof() == false)
			{
				const int clipId = q.getIntField(_T("lID"));

				CClip clip;
				CString fullText = _T("");
				if (clip.LoadFormats(clipId, true, false))
				{
					fullText = clip.GetUnicodeTextFormat();
				}

				updateStmt.bind(1, fullText);
				updateStmt.bind(2, clipId);
				updateStmt.execDML();
				updateStmt.reset();

				updated++;
				if ((updated % 500) == 0)
				{
					Log(StrF(_T("SearchIndex - rebuilt %d rows"), updated));
				}

				q.nextRow();
			}

			db.execDMLEx(_T("UPDATE MainSearchMeta SET version = %d;"), SearchIndexSql::kCurrentVersion);
			db.execDML(_T("commit transaction;"));
			inTransaction = false;

			Log(StrF(_T("SearchIndex - rebuild complete, rows: %d"), updated));
			return true;
		}
		CATCH_SQLITE_EXCEPTION

		if (inTransaction)
		{
			try
			{
				db.execDML(_T("rollback transaction;"));
			}
			catch (...)
			{
			}
		}

		return false;
	}
}

namespace SearchIndex
{
	bool EnsureCurrent(CppSQLite3DB &db)
	{
		g_searchIndexReady = false;

		try
		{
			EnsureSchema(db);

			if (NeedsRebuild(db))
			{
				if (Rebuild(db) == false)
				{
					return false;
				}
			}

			g_searchIndexReady = true;
			return true;
		}
		CATCH_SQLITE_EXCEPTION_AND_RETURN(false)
	}

	bool IsReady()
	{
		return g_searchIndexReady;
	}

	bool UpdateClipFullText(CppSQLite3DB &db, int clipId, LPCTSTR fullText)
	{
		if (g_searchIndexReady == false || clipId <= 0)
		{
			return false;
		}

		try
		{
			db.execDMLEx(_T("INSERT OR IGNORE INTO MainSearchCache(clipID, description, quickpaste, fulltext) ")
				_T("SELECT lID, COALESCE(mText, ''), COALESCE(QuickPasteText, ''), '' FROM Main WHERE lID = %d;"), clipId);

			CppSQLite3Statement updateStmt = db.compileStatement(_T("UPDATE MainSearchCache SET fulltext = ? WHERE clipID = ?;"));
			updateStmt.bind(1, fullText != nullptr ? fullText : _T(""));
			updateStmt.bind(2, clipId);
			updateStmt.execDML();

			return true;
		}
		CATCH_SQLITE_EXCEPTION_AND_RETURN(false)
	}
}
