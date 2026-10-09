#include "ThemedPopupMenu.h"
#include <commctrl.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace
{
	COLORREF Blend(COLORREF background, COLORREF foreground, int percent)
	{
		return RGB(
			(GetRValue(background) * (100 - percent) + GetRValue(foreground) * percent) / 100,
			(GetGValue(background) * (100 - percent) + GetGValue(foreground) * percent) / 100,
			(GetBValue(background) * (100 - percent) + GetBValue(foreground) * percent) / 100);
	}

	std::wstring MenuText(HMENU menu, UINT position)
	{
		MENUITEMINFOW info = { sizeof(info) };
		info.fMask = MIIM_STRING;
		if (!GetMenuItemInfoW(menu, position, TRUE, &info))
			return {};
		std::vector<wchar_t> buffer(info.cch + 1);
		info.dwTypeData = buffer.data();
		info.cch = static_cast<UINT>(buffer.size());
		if (!GetMenuItemInfoW(menu, position, TRUE, &info))
			return {};
		return std::wstring(buffer.data(), info.cch);
	}

	bool ContainsMenu(HMENU root, HMENU target)
	{
		if (root == target)
			return true;
		for (int i = 0; i < GetMenuItemCount(root); ++i)
		{
			HMENU child = GetSubMenu(root, i);
			if (child != NULL && ContainsMenu(child, target))
				return true;
		}
		return false;
	}
}

struct CThemedPopupMenu::Impl
{
	struct Item
	{
		HMENU menu;
		ULONG_PTR originalData;
		UINT type;
		std::wstring text;
		bool submenu;
		bool isDefault;
	};
	struct Menu
	{
		HMENU handle;
		HBRUSH originalBackground;
		UINT width = 0;
	};

	HWND owner;
	HMENU root;
	PopupMenuColors colors;
	UINT dpi = 96;
	HFONT font = NULL;
	HFONT boldFont = NULL;
	HBRUSH background = NULL;
	bool painting = false;
	UINT refreshMessage = RegisterWindowMessageW(L"Ditto.ThemedPopupMenu.Refresh");
	std::vector<HWND> popupWindows;
	std::vector<std::unique_ptr<Item>> items;
	std::vector<Menu> menus;

	int Scale(int value) const { return MulDiv(value, dpi, 96); }

	Impl(HWND window, HMENU menu, POINT position, const PopupMenuColors& palette)
		: owner(window), root(menu), colors(palette)
	{
		// The tray owner may be on a different monitor from the popup.
		HMODULE shcore = LoadLibraryW(L"shcore.dll");
		if (shcore != NULL)
		{
			using MonitorDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
			auto getDpi = reinterpret_cast<MonitorDpi>(GetProcAddress(shcore, "GetDpiForMonitor"));
			UINT x = 96, y = 96;
			if (getDpi && SUCCEEDED(getDpi(MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST), 0, &x, &y)))
				dpi = x;
			FreeLibrary(shcore);
		}
		NONCLIENTMETRICSW metrics = { sizeof(metrics) };
		using SystemMetricsForDpi = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
		auto getMetrics = reinterpret_cast<SystemMetricsForDpi>(
			GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"));
		BOOL loaded = getMetrics && getMetrics(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
		if (!loaded)
			loaded = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
		if (loaded)
		{
			font = CreateFontIndirectW(&metrics.lfMenuFont);
			metrics.lfMenuFont.lfWeight = FW_BOLD;
			boldFont = CreateFontIndirectW(&metrics.lfMenuFont);
		}
		background = CreateSolidBrush(colors.background);
		if (!font || !boldFont || !background ||
			!SetWindowSubclass(owner, WindowProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
			owner = NULL;
	}

	~Impl()
	{
		for (HWND popup : popupWindows)
			RemoveWindowSubclass(popup, PopupProc, reinterpret_cast<UINT_PTR>(this));
		if (owner != NULL)
			RemoveWindowSubclass(owner, WindowProc, reinterpret_cast<UINT_PTR>(this));
		for (const auto& menu : menus)
		{
			RestoreItems(menu.handle);
			MENUINFO info = { sizeof(info) };
			info.fMask = MIM_BACKGROUND;
			if (GetMenuInfo(menu.handle, &info) && info.hbrBack == background)
			{
				info.hbrBack = menu.originalBackground;
				SetMenuInfo(menu.handle, &info);
			}
		}
		if (background) DeleteObject(background);
		if (boldFont) DeleteObject(boldFont);
		if (font) DeleteObject(font);
	}

	Item* FindItem(ULONG_PTR data)
	{
		for (const auto& item : items)
			if (reinterpret_cast<ULONG_PTR>(item.get()) == data)
				return item.get();
		return nullptr;
	}

	void RestoreItems(HMENU menu)
	{
		for (int i = 0; i < GetMenuItemCount(menu); ++i)
		{
			MENUITEMINFOW info = { sizeof(info) };
			info.fMask = MIIM_FTYPE | MIIM_DATA;
			if (GetMenuItemInfoW(menu, i, TRUE, &info))
			{
				Item* item = FindItem(info.dwItemData);
				if (item && item->menu == menu)
				{
					info.fType &= ~MFT_OWNERDRAW;
					info.dwItemData = item->originalData;
					SetMenuItemInfoW(menu, i, TRUE, &info);
				}
			}
		}
	}

	void Apply(HMENU menu)
	{
		auto found = std::find_if(menus.begin(), menus.end(), [menu](const Menu& entry) { return entry.handle == menu; });
		if (found == menus.end())
		{
			MENUINFO info = { sizeof(info) };
			info.fMask = MIM_BACKGROUND;
			if (!GetMenuInfo(menu, &info)) return;
			menus.push_back({ menu, info.hbrBack, 0 });
			info.hbrBack = background;
			SetMenuInfo(menu, &info);
		}

		HDC dc = GetDC(owner);
		if (!dc) return;
		int maxLabel = 0, maxShortcut = 0;
		for (int i = 0; i < GetMenuItemCount(menu); ++i)
		{
			MENUITEMINFOW info = { sizeof(info) };
			info.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_SUBMENU | MIIM_STATE;
			if (!GetMenuItemInfoW(menu, i, TRUE, &info) || (info.fType & MFT_OWNERDRAW)) continue;
			auto item = std::make_unique<Item>();
			item->menu = menu;
			item->originalData = info.dwItemData;
			item->type = info.fType;
			item->text = MenuText(menu, i);
			item->submenu = info.hSubMenu != NULL;
			item->isDefault = (info.fState & MFS_DEFAULT) != 0;
			if (!(info.fType & MFT_SEPARATOR))
			{
				HGDIOBJ oldFont = SelectObject(dc, item->isDefault ? boldFont : font);
				auto tab = item->text.find(L'\t');
				std::wstring label = item->text.substr(0, tab);
				RECT rect = {};
				DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &rect, DT_CALCRECT | DT_SINGLELINE);
				maxLabel = (std::max)(maxLabel, static_cast<int>(rect.right));
				if (tab != std::wstring::npos)
				{
					std::wstring shortcut = item->text.substr(tab + 1);
					rect = {};
					DrawTextW(dc, shortcut.c_str(), static_cast<int>(shortcut.size()), &rect, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
					maxShortcut = (std::max)(maxShortcut, static_cast<int>(rect.right));
				}
				SelectObject(dc, oldFont);
			}
			info.fMask = MIIM_FTYPE | MIIM_DATA;
			info.fType |= MFT_OWNERDRAW;
			info.dwItemData = reinterpret_cast<ULONG_PTR>(item.get());
			if (SetMenuItemInfoW(menu, i, TRUE, &info)) items.push_back(std::move(item));
		}
		ReleaseDC(owner, dc);
		for (auto& entry : menus)
			if (entry.handle == menu)
				entry.width = Scale(26 + 12 + 18) + maxLabel + (maxShortcut ? Scale(24) + maxShortcut : 0);
	}

	void Measure(MEASUREITEMSTRUCT& measure, const Item& item)
	{
		for (const auto& menu : menus)
			if (menu.handle == item.menu) measure.itemWidth = menu.width;
		if (item.type & MFT_SEPARATOR)
			measure.itemHeight = Scale(9);
		else
		{
			HDC dc = GetDC(owner);
			TEXTMETRICW metrics = {};
			if (dc)
			{
				HGDIOBJ oldFont = SelectObject(dc, item.isDefault ? boldFont : font);
				GetTextMetricsW(dc, &metrics);
				SelectObject(dc, oldFont);
				ReleaseDC(owner, dc);
			}
			measure.itemHeight = (std::max)(Scale(24), static_cast<int>(metrics.tmHeight) + Scale(8));
		}
	}

	void Draw(const DRAWITEMSTRUCT& draw, const Item& item)
	{
		int saved = SaveDC(draw.hDC);
		bool selected = (draw.itemState & ODS_SELECTED) != 0;
		bool disabled = (draw.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
		COLORREF bg = selected ? colors.selectedBackground : colors.background;
		COLORREF fg = selected ? colors.selectedText : colors.text;
		if (disabled) fg = Blend(bg, fg, 50);
		SetDCBrushColor(draw.hDC, bg);
		FillRect(draw.hDC, &draw.rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		if (item.type & MFT_SEPARATOR)
		{
			SetDCPenColor(draw.hDC, Blend(colors.background, colors.text, 25));
			SelectObject(draw.hDC, GetStockObject(DC_PEN));
			int y = (draw.rcItem.top + draw.rcItem.bottom) / 2;
			MoveToEx(draw.hDC, draw.rcItem.left + Scale(26), y, NULL);
			LineTo(draw.hDC, draw.rcItem.right - Scale(10), y);
		}
		else
		{
			if (selected)
			{
				SetDCBrushColor(draw.hDC, colors.border);
				FrameRect(draw.hDC, &draw.rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
			}
			SelectObject(draw.hDC, (draw.itemState & ODS_DEFAULT) || item.isDefault ? boldFont : font);
			SetBkMode(draw.hDC, TRANSPARENT);
			SetTextColor(draw.hDC, fg);
			RECT textRect = draw.rcItem;
			textRect.left += Scale(26);
			textRect.right -= Scale(18 + 12);
			auto tab = item.text.find(L'\t');
			std::wstring label = item.text.substr(0, tab);
			UINT flags = DT_SINGLELINE | DT_VCENTER;
			if (draw.itemState & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
			DrawTextW(draw.hDC, label.c_str(), static_cast<int>(label.size()), &textRect, flags);
			if (tab != std::wstring::npos)
			{
				std::wstring shortcut = item.text.substr(tab + 1);
				DrawTextW(draw.hDC, shortcut.c_str(), static_cast<int>(shortcut.size()), &textRect, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
			}
			SetDCBrushColor(draw.hDC, fg);
			SetDCPenColor(draw.hDC, fg);
			SelectObject(draw.hDC, GetStockObject(DC_BRUSH));
			SelectObject(draw.hDC, GetStockObject(DC_PEN));
			int centerY = (draw.rcItem.top + draw.rcItem.bottom) / 2;
			if (draw.itemState & ODS_CHECKED)
			{
				int x = draw.rcItem.left + Scale(13);
				if (item.type & MFT_RADIOCHECK)
					Ellipse(draw.hDC, x - Scale(3), centerY - Scale(3), x + Scale(3), centerY + Scale(3));
				else
				{
					HPEN pen = CreatePen(PS_SOLID, Scale(2), fg);
					HGDIOBJ oldPen = SelectObject(draw.hDC, pen);
					MoveToEx(draw.hDC, x - Scale(4), centerY, NULL);
					LineTo(draw.hDC, x - Scale(1), centerY + Scale(3));
					LineTo(draw.hDC, x + Scale(5), centerY - Scale(4));
					SelectObject(draw.hDC, oldPen);
					DeleteObject(pen);
				}
			}
			if (item.submenu)
			{
				int x = draw.rcItem.right - Scale(10);
				POINT arrow[] = { {x - Scale(2), centerY - Scale(4)}, {x + Scale(2), centerY}, {x - Scale(2), centerY + Scale(4)} };
				Polygon(draw.hDC, arrow, 3);
			}
		}
		RestoreDC(draw.hDC, saved);
	}

	LRESULT MenuChar(HMENU menu, wchar_t key)
	{
		std::vector<int> matches;
		int selected = -1;
		for (int i = 0; i < GetMenuItemCount(menu); ++i)
		{
			MENUITEMINFOW info = { sizeof(info) };
			info.fMask = MIIM_DATA | MIIM_STATE;
			if (!GetMenuItemInfoW(menu, i, TRUE, &info)) continue;
			if (info.fState & MFS_HILITE) selected = i;
			Item* item = FindItem(info.dwItemData);
			if (!item || (info.fState & MFS_DISABLED)) continue;
			for (size_t j = 0; j + 1 < item->text.size() && item->text[j] != L'\t'; ++j)
			{
				if (item->text[j] != L'&') continue;
				if (item->text[j + 1] == L'&') { ++j; continue; }
				if (std::towupper(item->text[j + 1]) == std::towupper(key)) matches.push_back(i);
				break;
			}
		}
		if (matches.empty()) return MAKELRESULT(0, MNC_IGNORE);
		if (matches.size() == 1) return MAKELRESULT(matches.front(), MNC_EXECUTE);
		for (int match : matches)
			if (match > selected) return MAKELRESULT(match, MNC_SELECT);
		return MAKELRESULT(matches.front(), MNC_SELECT);
	}

	void FinishNativePainting(HWND popup)
	{
		if (painting || GetClassLongPtrW(popup, GCW_ATOM) != 32768) return;
		RECT windowRect;
		if (!GetWindowRect(popup, &windowRect)) return;
		// Match a visible menu by its documented item rectangles. Avoid private
		// menu-window messages and leave unrelated popups on the thread alone.
		HMENU matched = NULL;
		for (const auto& menu : menus)
		{
			// Long menus can scroll, so the first item need not be visible.
			for (int i = 0; i < GetMenuItemCount(menu.handle); ++i)
			{
				RECT itemRect;
				if (GetMenuItemRect(NULL, menu.handle, i, &itemRect) &&
					itemRect.left >= windowRect.left && itemRect.left <= windowRect.left + Scale(8) &&
					itemRect.right <= windowRect.right && itemRect.right >= windowRect.right - Scale(8) &&
					itemRect.top >= windowRect.top && itemRect.bottom <= windowRect.bottom)
				{
					matched = menu.handle;
					break;
				}
			}
			if (matched) break;
		}
		if (!matched) return;
		painting = true;
		HDC dc = GetWindowDC(popup);
		if (dc)
		{
			int saved = SaveDC(dc);
			SelectObject(dc, GetStockObject(DC_BRUSH));
			SelectObject(dc, GetStockObject(DC_PEN));
			for (int i = 0; i < GetMenuItemCount(matched); ++i)
			{
				MENUITEMINFOW info = { sizeof(info) };
				info.fMask = MIIM_STATE | MIIM_DATA;
				RECT rect;
				if (!GetMenuItemInfoW(matched, i, TRUE, &info) || !GetMenuItemRect(NULL, matched, i, &rect)) continue;
				if (rect.top < windowRect.top || rect.bottom > windowRect.bottom) continue;
				Item* item = FindItem(info.dwItemData);
				if (!item || !item->submenu) continue;
				OffsetRect(&rect, -windowRect.left, -windowRect.top);
				bool selected = (info.fState & MFS_HILITE) != 0;
				COLORREF bg = selected ? colors.selectedBackground : colors.background;
				COLORREF fg = selected ? colors.selectedText : colors.text;
				if (info.fState & MFS_DISABLED) fg = Blend(bg, fg, 50);
				RECT gutter = rect;
				gutter.left = gutter.right - Scale(18);
				SetDCBrushColor(dc, bg);
				FillRect(dc, &gutter, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
				if (selected)
				{
					SetDCBrushColor(dc, colors.border);
					FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
				}
				SetDCBrushColor(dc, fg);
				SetDCPenColor(dc, fg);
				int x = rect.right - Scale(10), y = (rect.top + rect.bottom) / 2;
				POINT arrow[] = { {x - Scale(2), y - Scale(4)}, {x + Scale(2), y}, {x - Scale(2), y + Scale(4)} };
				Polygon(dc, arrow, 3);
			}
			RECT border = {0, 0, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top};
			SetDCBrushColor(dc, colors.border);
			FrameRect(dc, &border, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
			RestoreDC(dc, saved);
			ReleaseDC(popup, dc);
		}
		painting = false;
	}

	static LRESULT CALLBACK PopupProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
	{
		auto* self = reinterpret_cast<Impl*>(data);
		if (message == WM_NCDESTROY)
		{
			RemoveWindowSubclass(window, PopupProc, id);
			auto& windows = self->popupWindows;
			windows.erase(std::remove(windows.begin(), windows.end(), window), windows.end());
		}
		LRESULT result = DefSubclassProc(window, message, wParam, lParam);
		if (message == WM_PAINT || message == WM_NCPAINT)
			self->FinishNativePainting(window);
		return result;
	}

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
	{
		auto* self = reinterpret_cast<Impl*>(data);
		if (message == WM_INITMENUPOPUP && ContainsMenu(self->root, reinterpret_cast<HMENU>(wParam)))
		{
			HMENU menu = reinterpret_cast<HMENU>(wParam);
			// Let MFC update labels, shortcuts and command states as a normal string menu.
			self->RestoreItems(menu);
			LRESULT result = DefSubclassProc(window, message, wParam, lParam);
			self->Apply(menu);
			return result;
		}
		if (message == WM_MEASUREITEM && lParam)
		{
			auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
			Item* item = measure->CtlType == ODT_MENU ? self->FindItem(measure->itemData) : nullptr;
			if (item) { self->Measure(*measure, *item); return TRUE; }
		}
		if (message == WM_DRAWITEM && lParam)
		{
			auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
			Item* item = draw->CtlType == ODT_MENU ? self->FindItem(draw->itemData) : nullptr;
			if (item) { self->Draw(*draw, *item); return TRUE; }
		}
		if (message == WM_MENUCHAR && ContainsMenu(self->root, reinterpret_cast<HMENU>(lParam)))
			return self->MenuChar(reinterpret_cast<HMENU>(lParam), LOWORD(wParam));
		if (message == WM_ENTERIDLE && wParam == MSGF_MENU && lParam)
		{
			HWND popup = reinterpret_cast<HWND>(lParam);
			if (std::find(self->popupWindows.begin(), self->popupWindows.end(), popup) == self->popupWindows.end() &&
				SetWindowSubclass(popup, PopupProc, reinterpret_cast<UINT_PTR>(self), data))
				self->popupWindows.push_back(popup);
			// Native menus add their own black submenu arrow after WM_DRAWITEM.
			self->FinishNativePainting(popup);
		}
		if (message == WM_MENUSELECT && self->refreshMessage)
			PostMessageW(window, self->refreshMessage, reinterpret_cast<WPARAM>(self), 0);
		if (self->refreshMessage && message == self->refreshMessage && wParam == reinterpret_cast<WPARAM>(self))
		{
			for (HWND popup : self->popupWindows) self->FinishNativePainting(popup);
			return 0;
		}
		if (message == WM_NCDESTROY)
		{
			RemoveWindowSubclass(window, WindowProc, id);
			self->owner = NULL;
		}
		return DefSubclassProc(window, message, wParam, lParam);
	}
};

CThemedPopupMenu::CThemedPopupMenu(HWND owner, HMENU menu, POINT position, const PopupMenuColors& colors)
	: m_impl(std::make_unique<Impl>(owner, menu, position, colors))
{
}

CThemedPopupMenu::~CThemedPopupMenu() = default;
