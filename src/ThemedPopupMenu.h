#pragma once

#include <windows.h>
#include <memory>

struct PopupMenuColors
{
	COLORREF background;
	COLORREF text;
	COLORREF selectedBackground;
	COLORREF selectedText;
	COLORREF border;
};

// Scoped drawing for an existing native menu. Command routing stays with its owner.
class CThemedPopupMenu
{
public:
	CThemedPopupMenu(HWND owner, HMENU menu, POINT position, const PopupMenuColors& colors);
	~CThemedPopupMenu();
	CThemedPopupMenu(const CThemedPopupMenu&) = delete;
	CThemedPopupMenu& operator=(const CThemedPopupMenu&) = delete;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
