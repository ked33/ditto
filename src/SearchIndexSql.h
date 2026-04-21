#pragma once

#include <string>
#include <vector>

namespace SearchIndexSql
{
	inline constexpr int kCurrentVersion = 1;

	std::vector<std::string> SchemaStatementsUtf8();
}
