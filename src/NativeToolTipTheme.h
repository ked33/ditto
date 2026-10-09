#pragma once

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>

#pragma comment(lib, "uxtheme.lib")

inline void ApplyNativeToolTipTheme(HWND tooltip, COLORREF background, COLORREF text)
{
	if (!IsWindow(tooltip))
		return;

	// Visual styles ignore TTM_SETTIPBKCOLOR/TTM_SETTIPTEXTCOLOR. Disable the
	// style on this native tooltip before supplying the application palette.
	SetWindowTheme(tooltip, L"", L"");
	SendMessage(tooltip, TTM_SETTIPBKCOLOR, background, 0);
	SendMessage(tooltip, TTM_SETTIPTEXTCOLOR, text, 0);
	InvalidateRect(tooltip, NULL, TRUE);
}
