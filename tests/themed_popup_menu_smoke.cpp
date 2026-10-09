// Standalone Win32 regression test; does not require MFC or display a window.
#include "../src/ThemedPopupMenu.h"
#include <commctrl.h>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
	int initCount = 0;
	int commandCount = 0;
	HMENU root = NULL;

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
	root = CreatePopupMenu();
	HMENU child = CreatePopupMenu();
	assert(AppendMenuW(root, MF_STRING, 100, L"&Options\tCtrl+O"));
	assert(AppendMenuW(root, MF_STRING, 101, L"&Other"));
	assert(AppendMenuW(root, MF_SEPARATOR, 0, NULL));
	assert(AppendMenuW(root, MF_STRING | MF_GRAYED, 102, L"&Disabled"));
	assert(AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(child), L"&Search"));
	assert(AppendMenuW(root, MF_STRING, 103, L"Rock && Roll"));
	assert(AppendMenuW(root, MF_STRING, 104, L"&\u8bbe\u7f6e\tCtrl+,"));
	assert(AppendMenuW(child, MF_STRING | MF_CHECKED, 105, L"&Find"));
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
		MENUINFO restored = { sizeof(restored) };
		restored.fMask = MIM_BACKGROUND;
		assert(GetMenuInfo(root, &restored) && restored.hbrBack == originalBrush);
		DWORD current = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
		// GDI caches stock objects on first use; subsequent openings must be stable.
		if (repeat == 0) baseline = current;
		else assert(current <= baseline);
	}
	assert(initCount == 9 && commandCount == 3);
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
