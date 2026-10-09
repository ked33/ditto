// Standalone Win32 regression test; the native tooltip is displayed offscreen.
#include "../src/ThemedPopupMenu.h"
#include "../src/NativeToolTipTheme.h"
#include <commctrl.h>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
	int initCount = 0;
	int commandCount = 0;
	HMENU root = NULL;
	wchar_t tooltipSample[] = L"Clipboard text\r\nAdded: 2026-10-09\r\nLast Used: 2026-10-09";

	struct ToolTipProbe
	{
		COLORREF background = 0;
		COLORREF text = 0;
		COLORREF border = 0;
		HWND window = NULL;
		bool drawBorder = true;
		int textRequests = 0;
		int themeChanges = 0;
		int backgroundWrites = 0;
		int textWrites = 0;
		int borderPaints = 0;
	};

	LRESULT CALLBACK ToolTipProbeProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
		UINT_PTR, DWORD_PTR reference)
	{
		auto& probe = *reinterpret_cast<ToolTipProbe*>(reference);
		if (message == WM_THEMECHANGED) ++probe.themeChanges;
		if (message == TTM_SETTIPBKCOLOR) ++probe.backgroundWrites;
		if (message == TTM_SETTIPTEXTCOLOR) ++probe.textWrites;
		return DefSubclassProc(window, message, wParam, lParam);
	}

	MENUITEMINFOW Info(HMENU menu, UINT position)
	{
		MENUITEMINFOW info = { sizeof(info) };
		info.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STATE | MIIM_ID | MIIM_SUBMENU;
		assert(GetMenuItemInfoW(menu, position, TRUE, &info));
		return info;
	}

	std::wstring Text(HMENU menu, UINT position)
	{
		wchar_t text[256] = {};
		MENUITEMINFOW info = { sizeof(info) };
		info.fMask = MIIM_STRING;
		info.dwTypeData = text;
		info.cch = 256;
		assert(GetMenuItemInfoW(menu, position, TRUE, &info));
		return text;
	}

	LRESULT CALLBACK OwnerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lParam)->code == TTN_GETDISPINFOW)
		{
			auto* info = reinterpret_cast<NMTTDISPINFOW*>(lParam);
			auto& probe = *reinterpret_cast<ToolTipProbe*>(info->lParam);
			++probe.textRequests;
			// Match the native hover tooltip's existing text callback in QListCtrl.
			ApplyNativeToolTipTheme(info->hdr.hwndFrom, probe.background, probe.text);
			info->lpszText = tooltipSample;
			return 0;
		}
		if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lParam)->code == NM_CUSTOMDRAW)
		{
			auto* probe = reinterpret_cast<ToolTipProbe*>(GetWindowLongPtrW(window, GWLP_USERDATA));
			auto* draw = reinterpret_cast<NMTTCUSTOMDRAW*>(lParam);
			if (probe && draw->nmcd.hdr.hwndFrom == probe->window && probe->drawBorder)
			{
				COLORREF brushColor = GetDCBrushColor(draw->nmcd.hdc);
				LRESULT result = CustomDrawNativeToolTipBorder(*draw, probe->border);
				assert(GetDCBrushColor(draw->nmcd.hdc) == brushColor);
				if (draw->nmcd.dwDrawStage == CDDS_POSTPAINT && !(draw->uDrawFlags & DT_CALCRECT))
					++probe->borderPaints;
				return result;
			}
		}
		if (message == WM_INITMENUPOPUP)
		{
			++initCount;
			HMENU menu = reinterpret_cast<HMENU>(wParam);
			// Command UI updates must see normal strings, even on a second opening.
			for (int i = 0; i < GetMenuItemCount(menu); ++i)
				assert(!(Info(menu, i).fType & MFT_OWNERDRAW));
			if (menu == root)
			{
				MENUITEMINFOW info = { sizeof(info) };
				info.fMask = MIIM_STRING;
				std::wstring text = L"&Other options\tCtrl+Shift+Alt+O";
				info.dwTypeData = &text[0];
				assert(SetMenuItemInfoW(menu, 1, TRUE, &info));
			}
			return 0;
		}
		if (message == WM_COMMAND)
		{
			assert(LOWORD(wParam) == 100);
			++commandCount;
			return 0;
		}
		return DefWindowProcW(window, message, wParam, lParam);
	}

	void CheckToolTipBorderPixels(ToolTipProbe& probe)
	{
		DWORD baseline = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
		RECT rect;
		assert(GetClientRect(probe.window, &rect));
		const int width = rect.right, height = rect.bottom;
		HDC dc = CreateCompatibleDC(NULL);
		BITMAPINFO info = {};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		void* pixels = nullptr;
		HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
		assert(bitmap);
		HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
		probe.drawBorder = false;
		const int nativeTextRequestsBefore = probe.textRequests;
		SendMessageW(probe.window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
		const int nativeTextRequestsPerPaint = probe.textRequests - nativeTextRequestsBefore;
		GdiFlush();
		auto* data = static_cast<DWORD*>(pixels);
		std::vector<DWORD> original(data, data + width * height);
		probe.drawBorder = true;
		const auto before = probe;
		for (int repeat = 0; repeat < 100; ++repeat)
			SendMessageW(probe.window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
		GdiFlush();
		assert(probe.borderPaints == before.borderPaints + 100);
		assert(probe.textRequests == before.textRequests + 100 * nativeTextRequestsPerPaint);
		assert(probe.themeChanges == before.themeChanges);
		assert(probe.backgroundWrites == before.backgroundWrites);
		assert(probe.textWrites == before.textWrites);
		const DWORD borderPixel = RGB(GetBValue(probe.border), GetGValue(probe.border), GetRValue(probe.border));
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				const int index = y * width + x;
				const DWORD expected = x == 0 || y == 0 || x == width - 1 || y == height - 1
					? borderPixel : original[index];
				assert((data[index] & 0xFFFFFF) == (expected & 0xFFFFFF));
			}
		// Printing does not validate the native window's pending update region.
		// A normal paint must consume it without scheduling another paint.
		UpdateWindow(probe.window);
		assert(!GetUpdateRect(probe.window, NULL, FALSE));
		SelectObject(dc, oldBitmap);
		DeleteObject(bitmap);
		DeleteDC(dc);
		assert(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == baseline);
	}

	void CheckNativeToolTip(HWND owner)
	{
		HWND tip = CreateWindowExW(0, TOOLTIPS_CLASSW, L"", WS_POPUP | TTS_ALWAYSTIP,
			0, 0, 300, 100, owner, NULL, GetModuleHandleW(NULL), NULL);
		assert(tip);
		ToolTipProbe probe;
		probe.window = tip;
		SetWindowLongPtrW(owner, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&probe));
		assert(SetWindowSubclass(tip, ToolTipProbeProc, 1, reinterpret_cast<DWORD_PTR>(&probe)));
		SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, 500);
		SendMessageW(tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 7000);
		TOOLINFOW tool = { sizeof(tool) };
		tool.hwnd = owner;
		tool.uId = 1;
		tool.rect = {0, 0, 300, 100};
		tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
		tool.lpszText = LPSTR_TEXTCALLBACKW;
		tool.lParam = reinterpret_cast<LPARAM>(&probe);
		assert(SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)));
		auto readText = [&]()
		{
			wchar_t text[100] = {};
			tool.lpszText = text;
			SendMessageW(tip, TTM_GETTEXTW, 100, reinterpret_cast<LPARAM>(&tool));
			assert(std::wstring(text) == tooltipSample);
		};

		for (const auto& palette : std::vector<PopupMenuColors>{
			{RGB(40, 42, 54), RGB(248, 248, 242), 0, 0, RGB(98, 114, 164)},
			{RGB(255, 255, 225), RGB(25, 25, 25), 0, 0, RGB(204, 204, 204)},
			{RGB(40, 42, 54), RGB(248, 248, 242), 0, 0, RGB(98, 114, 164)}})
		{
			probe.background = palette.background;
			probe.text = palette.text;
			probe.border = palette.border;
			readText();
			assert(GetWindowTheme(tip) == NULL);
			assert(SendMessageW(tip, TTM_GETTIPBKCOLOR, 0, 0) == palette.background);
			assert(SendMessageW(tip, TTM_GETTIPTEXTCOLOR, 0, 0) == palette.text);
			SendMessageW(tip, TTM_TRACKPOSITION, 0, MAKELPARAM(-30000, -30000));
			SendMessageW(tip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
			RECT position;
			assert(GetWindowRect(tip, &position));
			assert(position.right < 0 && position.bottom < 0);
			UpdateWindow(tip);
			assert(!GetUpdateRect(tip, NULL, FALSE));
			const auto before = probe;
			for (int repeat = 0; repeat < 1000; ++repeat)
				readText();
			assert(probe.textRequests == before.textRequests + 1000);
			assert(probe.themeChanges == before.themeChanges);
			assert(probe.backgroundWrites == before.backgroundWrites);
			assert(probe.textWrites == before.textWrites);
			assert(probe.borderPaints == before.borderPaints);
			assert(!GetUpdateRect(tip, NULL, FALSE));
			assert(SendMessageW(tip, TTM_GETMAXTIPWIDTH, 0, 0) == 500);
			assert(SendMessageW(tip, TTM_GETDELAYTIME, TTDT_AUTOPOP, 0) == 7000);
			CheckToolTipBorderPixels(probe);
			SendMessageW(tip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tool));
		}
		assert(RemoveWindowSubclass(tip, ToolTipProbeProc, 1));
		DestroyWindow(tip);
		SetWindowLongPtrW(owner, GWLP_USERDATA, 0);
		ApplyNativeToolTipTheme(NULL, 0, 0);
		NMTTCUSTOMDRAW measurement = {};
		measurement.nmcd.dwDrawStage = CDDS_PREPAINT;
		measurement.uDrawFlags = DT_CALCRECT;
		assert(CustomDrawNativeToolTipBorder(measurement, 0) == CDRF_DODEFAULT);
		puts("PASS: native tooltip border pixels, unchanged interior, 300 paints without extra text requests or GDI leaks");
		puts("PASS: native hover tooltip colors, 3000 text callbacks without theme/color writes or invalidation, text and timing");
	}

	void CheckMnemonic(HWND owner, HMENU menu, wchar_t key, UINT position, UINT action)
	{
		LRESULT result = SendMessageW(owner, WM_MENUCHAR, MAKEWPARAM(key, MF_POPUP), reinterpret_cast<LPARAM>(menu));
		assert(LOWORD(result) == position);
		assert(HIWORD(result) == action);
	}
}

int main()
{
	INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_WIN95_CLASSES };
	assert(InitCommonControlsEx(&controls));
	WNDCLASSW wc = {};
	wc.lpfnWndProc = OwnerProc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"DittoThemedMenuSmoke";
	assert(RegisterClassW(&wc));
	HWND owner = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 300, 200, NULL, NULL, wc.hInstance, NULL);
	assert(owner);
	CheckNativeToolTip(owner);
	root = CreatePopupMenu();
	HMENU child = CreatePopupMenu();
	HMENU addin = CreatePopupMenu();
	assert(AppendMenuW(root, MF_STRING, 100, L"&Options\tCtrl+O"));
	assert(AppendMenuW(root, MF_STRING, 101, L"&Other"));
	assert(AppendMenuW(root, MF_SEPARATOR, 0, NULL));
	assert(AppendMenuW(root, MF_STRING | MF_GRAYED, 102, L"&Disabled"));
	assert(AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(child), L"&Search"));
	assert(AppendMenuW(root, MF_STRING, 103, L"Rock && Roll"));
	assert(AppendMenuW(root, MF_STRING, 104, L"&\u8bbe\u7f6e\tCtrl+,"));
	assert(AppendMenuW(child, MF_STRING | MF_CHECKED, 105, L"&Find"));
	// Clip menus contain an extra level: Add-Ins -> Ditto Utils -> command.
	assert(AppendMenuW(child, MF_POPUP, reinterpret_cast<UINT_PTR>(addin), L"Ditto &Utils"));
	assert(AppendMenuW(addin, MF_STRING, 3000, L"&Paste as text\tCtrl+Shift+V"));
	assert(SetMenuDefaultItem(root, 100, FALSE));
	MENUITEMINFOW metadata = { sizeof(metadata) };
	metadata.fMask = MIIM_DATA;
	metadata.dwItemData = 0x1234;
	assert(SetMenuItemInfoW(root, 0, TRUE, &metadata));
	metadata.fMask = MIIM_FTYPE;
	metadata.fType = MFT_RADIOCHECK;
	assert(SetMenuItemInfoW(child, 0, TRUE, &metadata));
	HBRUSH originalBrush = CreateSolidBrush(RGB(1,2,3));
	MENUINFO original = { sizeof(original) };
	original.fMask = MIM_BACKGROUND;
	original.hbrBack = originalBrush;
	assert(SetMenuInfo(root, &original));
	PopupMenuColors colors = { RGB(40,42,54), RGB(248,248,242), RGB(98,114,164), RGB(139,233,253), RGB(98,114,164) };
	DWORD baseline = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
	for (int repeat = 0; repeat < 3; ++repeat)
	{
		colors = repeat == 1
			? PopupMenuColors{ RGB(240,240,240), RGB(0,0,0), RGB(30,116,211), RGB(255,255,255), RGB(122,150,223) }
			: PopupMenuColors{ RGB(40,42,54), RGB(248,248,242), RGB(98,114,164), RGB(139,233,253), RGB(98,114,164) };
		{
			CThemedPopupMenu themed(owner, root, {0,0}, colors);
			SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(root), 0);
			assert(Info(root, 0).fType & MFT_OWNERDRAW);
			assert(Info(root, 0).fState & MFS_DEFAULT);
			assert(Text(root, 1) == L"&Other options\tCtrl+Shift+Alt+O");
			SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(child), 0);
			assert(Info(child, 0).fType & MFT_RADIOCHECK);
			assert(Info(child, 0).fState & MFS_CHECKED);
			SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(addin), 0);
			assert(Info(addin, 0).fType & MFT_OWNERDRAW);
			assert(Info(addin, 0).wID == 3000);
			CheckMnemonic(owner, addin, L'p', 0, MNC_EXECUTE);
			CheckMnemonic(owner, root, L'o', 0, MNC_SELECT);
			assert(HiliteMenuItem(owner, root, 0, MF_BYPOSITION | MF_HILITE));
			CheckMnemonic(owner, root, L'O', 1, MNC_SELECT);
			CheckMnemonic(owner, root, L'd', 0, MNC_IGNORE);
			CheckMnemonic(owner, root, L'r', 0, MNC_IGNORE);
			CheckMnemonic(owner, root, L's', 4, MNC_EXECUTE);
			CheckMnemonic(owner, root, L'\u8bbe', 6, MNC_EXECUTE);
			CheckMnemonic(owner, child, L'f', 0, MNC_EXECUTE);
			assert(HiliteMenuItem(owner, root, 0, MF_BYPOSITION | MF_UNHILITE));
			UINT width = 0;
			HDC dc = CreateCompatibleDC(NULL);
			BITMAPINFO bitmapInfo = {};
			bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bitmapInfo.bmiHeader.biWidth = 1000;
			bitmapInfo.bmiHeader.biHeight = -100;
			bitmapInfo.bmiHeader.biPlanes = 1;
			bitmapInfo.bmiHeader.biBitCount = 32;
			void* pixels = nullptr;
			HBITMAP bitmap = CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS, &pixels, NULL, 0);
			assert(bitmap);
			HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
			for (int i = 0; i < GetMenuItemCount(root); ++i)
			{
				auto info = Info(root, i);
				MEASUREITEMSTRUCT measure = {};
				measure.CtlType = ODT_MENU;
				measure.itemData = info.dwItemData;
				assert(SendMessageW(owner, WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&measure)));
				assert(measure.itemWidth > 100 && measure.itemHeight > 0);
				if (i == 0) width = measure.itemWidth;
				assert(measure.itemWidth == width);
				DRAWITEMSTRUCT draw = {};
				draw.CtlType = ODT_MENU;
				draw.itemData = info.dwItemData;
				draw.hwndItem = reinterpret_cast<HWND>(root);
				draw.hDC = dc;
				draw.rcItem = {0, 0, static_cast<LONG>(measure.itemWidth), static_cast<LONG>(measure.itemHeight)};
				assert(SendMessageW(owner, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw)));
				assert(GetPixel(dc, 2, 2) == colors.background);
				if (!(info.fType & MFT_SEPARATOR))
				{
					draw.itemState = ODS_SELECTED | ODS_CHECKED;
					assert(SendMessageW(owner, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw)));
					assert(GetPixel(dc, 2, 2) == colors.selectedBackground);
				}
			}
			SelectObject(dc, oldBitmap);
			DeleteObject(bitmap);
			DeleteDC(dc);
			SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(root), 0);
			SendMessageW(owner, WM_COMMAND, 100, 0);
		}
		assert(!(Info(root, 0).fType & MFT_OWNERDRAW));
		assert(Info(root, 0).dwItemData == 0x1234);
		assert(Info(child, 0).fType == MFT_RADIOCHECK);
		assert(!(Info(addin, 0).fType & MFT_OWNERDRAW));
		assert(Text(addin, 0) == L"&Paste as text\tCtrl+Shift+V");
		MENUINFO restored = { sizeof(restored) };
		restored.fMask = MIM_BACKGROUND;
		assert(GetMenuInfo(root, &restored) && restored.hbrBack == originalBrush);
		DWORD current = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
		// GDI caches stock objects on first use; subsequent openings must be stable.
		if (repeat == 0) baseline = current;
		else assert(current <= baseline);
	}
	assert(initCount == 12 && commandCount == 3);
	// Exit/menu commands can destroy the menu and its owner before tracking returns.
	{
		CThemedPopupMenu themed(owner, root, {0,0}, colors);
		SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(root), 0);
		DestroyMenu(root);
		root = NULL;
		DestroyWindow(owner);
	}
	DeleteObject(originalBrush);
	puts("PASS: native menus, live text updates, submenus, checks, Unicode mnemonics, colors, command routing, restoration and GDI lifetime");
}
