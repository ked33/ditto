#pragma once

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>

#pragma comment(lib, "uxtheme.lib")

inline void ApplyNativeToolTipTheme(HWND tooltip, COLORREF background, COLORREF text)
{
	if (!IsWindow(tooltip))
		return;

	// Active visual styles ignore custom colors. Disable them only when present.
	if (GetWindowTheme(tooltip) != NULL)
		SetWindowTheme(tooltip, L"", L"");
	if (static_cast<COLORREF>(SendMessage(tooltip, TTM_GETTIPBKCOLOR, 0, 0)) != background)
		SendMessage(tooltip, TTM_SETTIPBKCOLOR, background, 0);
	if (static_cast<COLORREF>(SendMessage(tooltip, TTM_GETTIPTEXTCOLOR, 0, 0)) != text)
		SendMessage(tooltip, TTM_SETTIPTEXTCOLOR, text, 0);
}
