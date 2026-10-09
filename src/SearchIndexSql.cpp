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
			// Positions let a quoted character phrase prove substring membership
			// without reading the potentially multi-megabyte external content.
			"CREATE VIRTUAL TABLE IF NOT EXISTS MainFullTextLiteralIndex USING fts5("
				"fulltext, content='MainFullTextCache', content_rowid='clipID', "
				"tokenize='ditto_char', detail='full');",
			"CREATE TRIGGER IF NOT EXISTS MainFullTextLiteral_ai AFTER INSERT ON MainFullTextCache BEGIN "
				"INSERT INTO MainFullTextLiteralIndex(rowid, fulltext) VALUES (new.clipID, new.fulltext); END;",
			"CREATE TRIGGER IF NOT EXISTS MainFullTextLiteral_ad AFTER DELETE ON MainFullTextCache BEGIN "
				"INSERT INTO MainFullTextLiteralIndex(MainFullTextLiteralIndex, rowid, fulltext) "
				"VALUES('delete', old.clipID, old.fulltext); END;",
			"CREATE TRIGGER IF NOT EXISTS MainFullTextLiteral_au AFTER UPDATE OF fulltext ON MainFullTextCache "
				"WHEN old.fulltext IS NOT new.fulltext BEGIN "
				"INSERT INTO MainFullTextLiteralIndex(MainFullTextLiteralIndex, rowid, fulltext) "
				"VALUES('delete', old.clipID, old.fulltext); "
				"INSERT INTO MainFullTextLiteralIndex(rowid, fulltext) VALUES (new.clipID, new.fulltext); END;",
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

	std::string WebLinkPredicateUtf8()
	{
		// This exact predicate is shared by the partial index and the query.
		// Bump the index suffix if matching semantics change, to rebuild old keys.
		return "strClipBoardFormat IN ('CF_TEXT', 'CF_UNICODETEXT') AND ditto_clipboard_text(ooData, strClipBoardFormat) REGEXP '(?i)\\A\\s*(?:https?://(?:[^\\s/?#<>\"@]+@)?|www\\.)(?:[\\p{L}\\p{N}](?:[\\p{L}\\p{N}-]*[\\p{L}\\p{N}])?(?:\\.[\\p{L}\\p{N}](?:[\\p{L}\\p{N}-]*[\\p{L}\\p{N}])?)*|\\[[0-9a-f:.]+\\])(?::[0-9]{1,5})?(?:[/?#][^\\s<>\"]*)?\\s*\\z'";
	}

	std::string WebLinkIndexUtf8()
	{
		return "CREATE INDEX IF NOT EXISTS Data_WebLink_v1 ON Data(lParentID, strClipBoardFormat) WHERE " + WebLinkPredicateUtf8();
	}

	std::string WebLinkFilterUtf8()
	{
		// Unicode takes precedence even when the ANSI body looks like a link.
		// The predicate also works without the index if setup failed.
		return "EXISTS (SELECT 1 FROM Data LinkData WHERE LinkData.lParentID = Main.lID AND " +
			WebLinkPredicateUtf8() +
			" AND (LinkData.strClipBoardFormat = 'CF_UNICODETEXT' OR NOT EXISTS "
			"(SELECT 1 FROM Data UnicodeData WHERE UnicodeData.lParentID = LinkData.lParentID "
			"AND UnicodeData.strClipBoardFormat = 'CF_UNICODETEXT')))";
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
