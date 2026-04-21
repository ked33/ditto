#include "SearchIndexSql.h"

namespace SearchIndexSql
{
	std::vector<std::string> SchemaStatementsUtf8()
	{
		return
		{
			"CREATE TABLE IF NOT EXISTS MainSearchMeta(version INTEGER NOT NULL);",
			"INSERT INTO MainSearchMeta(version) SELECT 0 WHERE NOT EXISTS(SELECT 1 FROM MainSearchMeta);",
			"CREATE TABLE IF NOT EXISTS MainSearchCache("
				"clipID INTEGER PRIMARY KEY, "
				"description TEXT NOT NULL DEFAULT '', "
				"quickpaste TEXT NOT NULL DEFAULT '', "
				"fulltext TEXT NOT NULL DEFAULT '');",
			"CREATE VIRTUAL TABLE IF NOT EXISTS MainSearchIndex USING fts5("
				"description, "
				"quickpaste, "
				"fulltext, "
				"content='MainSearchCache', "
				"content_rowid='clipID', "
				"tokenize='trigram', "
				"detail='none');",
			"CREATE TRIGGER IF NOT EXISTS MainSearchCache_ai AFTER INSERT ON MainSearchCache BEGIN "
				"INSERT INTO MainSearchIndex(rowid, description, quickpaste, fulltext) "
				"VALUES (new.clipID, new.description, new.quickpaste, new.fulltext); "
			"END;",
			"CREATE TRIGGER IF NOT EXISTS MainSearchCache_ad AFTER DELETE ON MainSearchCache BEGIN "
				"INSERT INTO MainSearchIndex(MainSearchIndex, rowid, description, quickpaste, fulltext) "
				"VALUES('delete', old.clipID, old.description, old.quickpaste, old.fulltext); "
			"END;",
			"CREATE TRIGGER IF NOT EXISTS MainSearchCache_au AFTER UPDATE ON MainSearchCache BEGIN "
				"INSERT INTO MainSearchIndex(MainSearchIndex, rowid, description, quickpaste, fulltext) "
				"VALUES('delete', old.clipID, old.description, old.quickpaste, old.fulltext); "
				"INSERT INTO MainSearchIndex(rowid, description, quickpaste, fulltext) "
				"VALUES (new.clipID, new.description, new.quickpaste, new.fulltext); "
			"END;",
			"CREATE TRIGGER IF NOT EXISTS MainSearchMain_ai AFTER INSERT ON Main BEGIN "
				"INSERT OR IGNORE INTO MainSearchCache(clipID, description, quickpaste, fulltext) "
				"VALUES (new.lID, COALESCE(new.mText, ''), COALESCE(new.QuickPasteText, ''), ''); "
			"END;",
			"CREATE TRIGGER IF NOT EXISTS MainSearchMain_au AFTER UPDATE OF mText, QuickPasteText ON Main BEGIN "
				"INSERT OR IGNORE INTO MainSearchCache(clipID, description, quickpaste, fulltext) "
				"VALUES (new.lID, COALESCE(new.mText, ''), COALESCE(new.QuickPasteText, ''), ''); "
				"UPDATE MainSearchCache "
				"SET description = COALESCE(new.mText, ''), "
					"quickpaste = COALESCE(new.QuickPasteText, '') "
				"WHERE clipID = new.lID; "
			"END;",
			"CREATE TRIGGER IF NOT EXISTS MainSearchMain_ad AFTER DELETE ON Main BEGIN "
				"DELETE FROM MainSearchCache WHERE clipID = old.lID; "
			"END;",
			"CREATE TRIGGER IF NOT EXISTS MainSearchDataUnicode_ad AFTER DELETE ON Data "
				"WHEN old.strClipBoardFormat = 'CF_UNICODETEXT' BEGIN "
				"UPDATE MainSearchCache SET fulltext = '' WHERE clipID = old.lParentID; "
			"END;"
		};
	}
}
