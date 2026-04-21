#pragma once

#include "sqlite\CppSQLite3.h"

namespace SearchIndex
{
	bool EnsureCurrent(CppSQLite3DB& db);
	bool IsReady();
	bool UpdateClipFullText(CppSQLite3DB& db, int clipId, LPCTSTR fullText);
}
