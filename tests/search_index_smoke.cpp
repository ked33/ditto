#define _CRT_SECURE_NO_WARNINGS

#include <cassert>
#include <iostream>
#include <vector>

#include "sqlite/sqlite3mc_amalgamation.h"
#include "SearchIndexSql.h"

namespace
{
	void Exec(sqlite3 *db, const std::string &sql)
	{
		char *error = nullptr;
		const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
		if (rc != SQLITE_OK)
		{
			std::cerr << "sqlite exec failed: " << sql << "\n";
			if (error != nullptr)
			{
				std::cerr << error << "\n";
				sqlite3_free(error);
			}
		}
		assert(rc == SQLITE_OK);
	}

	int Scalar(sqlite3 *db, const std::string &sql)
	{
		sqlite3_stmt *stmt = nullptr;
		const int prepareRc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
		assert(prepareRc == SQLITE_OK);

		const int stepRc = sqlite3_step(stmt);
		assert(stepRc == SQLITE_ROW);

		const int value = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
		return value;
	}
}

int main()
{
	sqlite3 *db = nullptr;
	assert(sqlite3_open(":memory:", &db) == SQLITE_OK);

	Exec(db, "CREATE TABLE Main(lID INTEGER PRIMARY KEY, mText TEXT, QuickPasteText TEXT);");
	Exec(db, "CREATE TABLE Data(lID INTEGER PRIMARY KEY, lParentID INTEGER, strClipBoardFormat TEXT, ooData BLOB);");

	const std::vector<std::string> schema = SearchIndexSql::SchemaStatementsUtf8();
	assert(schema.empty() == false);

	for (const auto &sql : schema)
	{
		Exec(db, sql);
	}

	Exec(db, "INSERT INTO MainSearchCache(clipID, description, quickpaste, fulltext) VALUES (1, 'Alpha clip', 'alpha-quick', 'body text with zebra');");
	Exec(db, "INSERT INTO MainSearchCache(clipID, description, quickpaste, fulltext) VALUES (2, 'Beta clip', '', 'body text with yak');");

	assert(Scalar(db, "SELECT COUNT(*) FROM MainSearchIndex WHERE description LIKE '%Alpha%';") == 1);
	assert(Scalar(db, "SELECT COUNT(*) FROM MainSearchIndex WHERE quickpaste LIKE '%alpha-quick%';") == 1);
	assert(Scalar(db, "SELECT COUNT(*) FROM MainSearchIndex WHERE fulltext LIKE '%zebra%';") == 1);

	Exec(db, "UPDATE MainSearchCache SET description = 'Gamma clip' WHERE clipID = 1;");
	assert(Scalar(db, "SELECT COUNT(*) FROM MainSearchIndex WHERE description LIKE '%Alpha%';") == 0);
	assert(Scalar(db, "SELECT COUNT(*) FROM MainSearchIndex WHERE description LIKE '%Gamma%';") == 1);

	Exec(db, "DELETE FROM MainSearchCache WHERE clipID = 2;");
	assert(Scalar(db, "SELECT COUNT(*) FROM MainSearchIndex;") == 1);

	sqlite3_close(db);
	return 0;
}
