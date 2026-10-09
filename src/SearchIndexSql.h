#pragma once

#include <string>
#include <vector>

namespace SearchIndexSql
{
	inline constexpr int kCurrentVersion = 3;

	std::vector<std::string> SchemaStatementsUtf8();
	std::vector<std::string> LegacyCleanupStatementsUtf8();
	std::string MigrateLegacyCacheUtf8();
	std::string WebLinkPredicateUtf8();
	std::string WebLinkIndexUtf8();
	std::string WebLinkFilterUtf8();
}
