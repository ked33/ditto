#include "stdafx.h"
#include "SearchIndex.h"

#include "SearchIndexSql.h"
#include "Clip.h"
#include "Misc.h"
#include <atomic>

namespace
{
	std::atomic<bool> g_searchIndexReady{false};

	CString ToCString(const std::string &text)
	{
		return CString(text.c_str());
	}

	void Rebuild(CppSQLite3DB &db)
	{
		Log(_T("SearchIndex - rebuilding full-text cache"));
		db.execDML(_T("DELETE FROM MainFullTextCache;"));
		db.execDML(_T("INSERT INTO MainFullTextCache(clipID) SELECT lID FROM Main;"));

		CppSQLite3Statement updateStmt = db.compileStatement(_T("UPDATE MainFullTextCache SET fulltext = ? WHERE clipID = ?;"));
		CppSQLite3Query q = db.execQuery(_T("SELECT lID FROM Main WHERE bIsGroup = 0"));
		int updated = 0;
		while (q.eof() == false)
		{
			const int clipId = q.getIntField(_T("lID"));
			CClip clip;
			CString fullText;
			if (clip.LoadFormats(clipId, true, false))
			{
				fullText = clip.GetUnicodeTextFormat();
			}
			updateStmt.bind(1, fullText);
			updateStmt.bind(2, clipId);
			updateStmt.execDML();
			updateStmt.reset();
			updated++;
			q.nextRow();
		}
		Log(StrF(_T("SearchIndex - rebuilt %d full-text rows"), updated));
	}
}

namespace SearchIndex
{
	bool EnsureCurrent(CppSQLite3DB &db)
	{
		g_searchIndexReady = false;
		bool inTransaction = false;
		try
		{
			// Failed upgrades leave both the previous cache and its version intact.
			db.execDML(_T("BEGIN IMMEDIATE;"));
			inTransaction = true;
			const bool hadIndex = db.tableExists(_T("MainFullTextIndex"));
			const bool hadLiteralIndex = db.tableExists(_T("MainFullTextLiteralIndex"));
			for (const auto &sql : SearchIndexSql::SchemaStatementsUtf8())
			{
				db.execDML(ToCString(sql));
			}

			const int version = db.execScalar(_T("SELECT version FROM MainSearchMeta LIMIT 1"));
			if (version > SearchIndexSql::kCurrentVersion)
			{
				db.execDML(_T("ROLLBACK;"));
				Log(_T("SearchIndex - newer schema, leaving it unchanged"));
				return false;
			}
			// A recreated index must contain existing cache rows before delete
			// triggers can safely run during a repair.
			if (!hadIndex)
				db.execDML(_T("INSERT INTO MainFullTextIndex(MainFullTextIndex) VALUES('rebuild');"));
			if (!hadLiteralIndex)
			{
				Log(_T("SearchIndex - building character positions for literal full-text search"));
				db.execDML(_T("INSERT INTO MainFullTextLiteralIndex(MainFullTextLiteralIndex) VALUES('rebuild');"));
			}

			const int mainCount = db.execScalar(_T("SELECT COUNT(*) FROM Main"));
			const int cacheCount = db.execScalar(_T("SELECT COUNT(*) FROM MainFullTextCache"));
			const bool missingIds = db.execScalar(_T("SELECT EXISTS(SELECT 1 FROM Main ")
				_T("LEFT JOIN MainFullTextCache Cache ON Cache.clipID = Main.lID WHERE Cache.clipID IS NULL)")) != 0;
			// A valid v2 cache already holds the complete text. Only the new
			// positional index needs building; do not reload the original BLOBs.
			if (version < 2 || mainCount != cacheCount || missingIds)
			{
				if (version == 1 && db.tableExists(_T("MainSearchCache")) &&
					db.execScalar(_T("SELECT COUNT(*) FROM MainSearchCache")) == mainCount &&
					db.execScalar(_T("SELECT EXISTS(SELECT 1 FROM Main LEFT JOIN MainSearchCache Legacy ")
						_T("ON Legacy.clipID = Main.lID WHERE Legacy.clipID IS NULL)")) == 0)
				{
					Log(_T("SearchIndex - migrating cached full text to separate index"));
					db.execDML(_T("DELETE FROM MainFullTextCache;"));
					db.execDML(ToCString(SearchIndexSql::MigrateLegacyCacheUtf8()));
				}
				else
				{
					Rebuild(db);
				}
			}

			for (const auto &sql : SearchIndexSql::LegacyCleanupStatementsUtf8())
			{
				db.execDML(ToCString(sql));
			}
			// Classify text formats once, then let SQLite maintain membership on
			// insert/update/delete. Paging and counts no longer read text BLOBs.
			db.execDML(ToCString(SearchIndexSql::WebLinkIndexUtf8()));
			db.execDMLEx(_T("UPDATE MainSearchMeta SET version = %d;"), SearchIndexSql::kCurrentVersion);
			db.execDML(_T("COMMIT;"));
			inTransaction = false;
			g_searchIndexReady = true;
			return true;
		}
		CATCH_SQLITE_EXCEPTION

		if (inTransaction)
		{
			try { db.execDML(_T("ROLLBACK;")); }
			catch (CppSQLite3Exception&) {}
		}
		return false;
	}

	bool IsReady()
	{
		return g_searchIndexReady;
	}

	bool UpdateClipFullText(CppSQLite3DB &db, int clipId, LPCTSTR fullText)
	{
		if (!g_searchIndexReady || clipId <= 0)
		{
			return false;
		}
		try
		{
			db.execDMLEx(_T("INSERT OR IGNORE INTO MainFullTextCache(clipID) ")
				_T("SELECT lID FROM Main WHERE lID = %d;"), clipId);
			CppSQLite3Statement stmt = db.compileStatement(_T("UPDATE MainFullTextCache SET fulltext = ? WHERE clipID = ?;"));
			stmt.bind(1, fullText != nullptr ? fullText : _T(""));
			stmt.bind(2, clipId);
			stmt.execDML();
			return true;
		}
		CATCH_SQLITE_EXCEPTION_AND_RETURN(false)
	}
}
