#include "SearchIndexSql.h"

namespace SearchIndexSql
{
	std::vector<std::string> SchemaStatementsUtf8()
	{
		return
		{
			"CREATE TABLE IF NOT EXISTS MainSearchMeta(version INTEGER NOT NULL);",
			"INSERT INTO MainSearchMeta(version) SELECT 0 WHERE NOT EXISTS(SELECT 1 FROM MainSearchMeta);",
			// Descriptions and quick-paste names stay in the small Main table.
			"CREATE TABLE IF NOT EXISTS MainFullTextCache("
				"clipID INTEGER PRIMARY KEY, fulltext TEXT NOT NULL DEFAULT '');",
			"CREATE VIRTUAL TABLE IF NOT EXISTS MainFullTextIndex USING fts5("
				"fulltext, content='MainFullTextCache', content_rowid='clipID', "
				"tokenize='trigram', detail='none');",
			"CREATE TRIGGER IF NOT EXISTS MainFullTextCache_ai AFTER INSERT ON MainFullTextCache BEGIN "
				"INSERT INTO MainFullTextIndex(rowid, fulltext) VALUES (new.clipID, new.fulltext); END;",
			"CREATE TRIGGER IF NOT EXISTS MainFullTextCache_ad AFTER DELETE ON MainFullTextCache BEGIN "
				"INSERT INTO MainFullTextIndex(MainFullTextIndex, rowid, fulltext) "
				"VALUES('delete', old.clipID, old.fulltext); END;",
			"CREATE TRIGGER IF NOT EXISTS MainFullTextCache_au AFTER UPDATE OF fulltext ON MainFullTextCache "
				"WHEN old.fulltext IS NOT new.fulltext BEGIN "
				"INSERT INTO MainFullTextIndex(MainFullTextIndex, rowid, fulltext) "
				"VALUES('delete', old.clipID, old.fulltext); "
				"INSERT INTO MainFullTextIndex(rowid, fulltext) VALUES (new.clipID, new.fulltext); END;",
			"CREATE TRIGGER IF NOT EXISTS MainFullTextMain_ai AFTER INSERT ON Main BEGIN "
				"INSERT OR IGNORE INTO MainFullTextCache(clipID) VALUES (new.lID); END;",
			"CREATE TRIGGER IF NOT EXISTS MainFullTextMain_ad AFTER DELETE ON Main BEGIN "
				"DELETE FROM MainFullTextCache WHERE clipID = old.lID; END;",
			"CREATE TRIGGER IF NOT EXISTS MainFullTextDataUnicode_ad AFTER DELETE ON Data "
				"WHEN old.strClipBoardFormat = 'CF_UNICODETEXT' BEGIN "
				"UPDATE MainFullTextCache SET fulltext = '' WHERE clipID = old.lParentID; END;"
		};
	}

	std::vector<std::string> LegacyCleanupStatementsUtf8()
	{
		return
		{
			"DROP TRIGGER IF EXISTS MainSearchCache_ai;",
			"DROP TRIGGER IF EXISTS MainSearchCache_ad;",
			"DROP TRIGGER IF EXISTS MainSearchCache_au;",
			"DROP TRIGGER IF EXISTS MainSearchMain_ai;",
			"DROP TRIGGER IF EXISTS MainSearchMain_au;",
			"DROP TRIGGER IF EXISTS MainSearchMain_ad;",
			"DROP TRIGGER IF EXISTS MainSearchDataUnicode_ad;",
			"DROP TABLE IF EXISTS MainSearchIndex;",
			"DROP TABLE IF EXISTS MainSearchCache;"
		};
	}

	std::string MigrateLegacyCacheUtf8()
	{
		return "INSERT INTO MainFullTextCache(clipID, fulltext) "
			"SELECT Main.lID, COALESCE(Legacy.fulltext, '') FROM Main "
			"LEFT JOIN MainSearchCache Legacy ON Legacy.clipID = Main.lID;";
	}
}
