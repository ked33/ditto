#pragma once

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>

#pragma comment(lib, "uxtheme.lib")

inline LRESULT CustomDrawNativeToolTipBorder(const NMTTCUSTOMDRAW& draw, COLORREF border)
{
	// Tooltips also send custom-draw notifications while measuring text.
	if (draw.uDrawFlags & DT_CALCRECT)
		return CDRF_DODEFAULT;
	if (draw.nmcd.dwDrawStage == CDDS_PREPAINT)
		return CDRF_NOTIFYPOSTPAINT;
	if (draw.nmcd.dwDrawStage == CDDS_POSTPAINT)
	{
		RECT rect;
		if (GetClientRect(draw.nmcd.hdr.hwndFrom, &rect))
		{
			// Reuse the native paint DC and stock brush; do not schedule another paint.
			COLORREF previous = SetDCBrushColor(draw.nmcd.hdc, border);
			FrameRect(draw.nmcd.hdc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
			SetDCBrushColor(draw.nmcd.hdc, previous);
		}
	}
	return CDRF_DODEFAULT;
}

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
