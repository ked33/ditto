#pragma once

#include <string>
#include <vector>

namespace SearchIndexSql
{
	inline constexpr int kCurrentVersion = 2;

	std::vector<std::string> SchemaStatementsUtf8();
	std::vector<std::string> LegacyCleanupStatementsUtf8();
	std::string MigrateLegacyCacheUtf8();
}
