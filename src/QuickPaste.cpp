// QuickPaste.cpp: implementation of the CQuickPaste class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "CP_Main.h"
#include "QuickPaste.h"

#ifdef _DEBUG
#undef THIS_FILE
static char THIS_FILE[]=__FILE__;
#define new DEBUG_NEW
#endif

#define ID_QPASTE_WND			0x1001

namespace
{
	BOOL ResolveFillListRequest(CQPasteWnd *pasteWnd, BOOL requestedFill)
	{
		BOOL fillList = requestedFill;

		if (fillList == FALSE)
		{
			if (pasteWnd->m_listItems.size() == 0)
			{
				fillList = TRUE;
			}
			else if (theApp.m_databaseOnNetworkShare)
			{
				__int64 lastWrite = GetLastWriteTime(CGetSetOptions::GetDBPath());
				if (lastWrite > pasteWnd->m_lastDbWrite)
				{
					pasteWnd->m_lastDbWrite = lastWrite;
					fillList = TRUE;
				}
			}
		}

		return fillList;
	}

	void BeginDeferredReveal(CQPasteWnd *pasteWnd)
	{
		if (pasteWnd == NULL || ::IsWindow(pasteWnd->GetSafeHwnd()) == FALSE)
		{
			return;
		}

		pasteWnd->m_Alpha.SetOpacity(0);
		pasteWnd->m_Alpha.SetTransparent(TRUE);
	}

	void EndDeferredReveal(CQPasteWnd *pasteWnd)
	{
		if (pasteWnd == NULL || ::IsWindow(pasteWnd->GetSafeHwnd()) == FALSE)
		{
			return;
		}

		pasteWnd->RedrawWindow(NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

		if (::IsWindow(pasteWnd->m_search.GetSafeHwnd()))
		{
			pasteWnd->m_search.RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
		}

		if (::IsWindow(pasteWnd->m_lstHeader.GetSafeHwnd()))
		{
			pasteWnd->m_lstHeader.RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
		}

		if (CGetSetOptions::GetEnableTransparency())
		{
			pasteWnd->SetCurrentTransparency();
		}
		else
		{
			pasteWnd->m_Alpha.SetOpacity(OPACITY_MAX);
		}
	}
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CQuickPaste::CQuickPaste()
{
	m_forceResizeOnNextShow = false;
	m_pwndPaste = NULL;
}

CQuickPaste::~CQuickPaste()
{
	if(m_pwndPaste)
	{
		delete m_pwndPaste;
		m_pwndPaste = NULL;
	}	
}

void CQuickPaste::Create(CWnd *pParent)
{
	CPoint point;
	CSize csSize;
	
	ASSERT(!m_pwndPaste);
	m_pwndPaste = new CQPasteWnd;
	ASSERT(m_pwndPaste);
	// load previous position and size
	CGetSetOptions::GetQuickPastePoint(point);
	CGetSetOptions::GetQuickPasteSize(csSize);

	CRect crRect = CRect(point, csSize);
	// Create the window
	ASSERT( m_pwndPaste->Create(crRect, pParent) );
	// place it at the previous position and size
	m_pwndPaste->MoveWindow(CRect(point, csSize));

	Log(_T("Creating QPasteWnd"));
}

BOOL CQuickPaste::CloseQPasteWnd()
{
	if(m_pwndPaste)
	{		
		if(m_pwndPaste)
		{
			m_pwndPaste->CloseWindow();
			m_pwndPaste->DestroyWindow();
		}

		Log(_T("CloseQPasteWnd called closing qpastewnd"));
		
		delete m_pwndPaste;
		m_pwndPaste = NULL;

		theApp.m_bShowingQuickPaste = false;
	}
	
	return TRUE;
}

void CQuickPaste::ShowQPasteWnd(CWnd *pParent, bool bAtPrevPos, bool bFromKeyboard, BOOL bReFillList)
{		
	Log(StrF(_T("Start of ShowQPasteWnd, AtPrevPos: %d, FromKeyboard: %d, RefillList: %d"), bAtPrevPos, bFromKeyboard, bReFillList));

	if(bFromKeyboard == false && GetKeyState(VK_SHIFT) & 0x8000 && CONTROL_PRESSED)
	{
		if(m_pwndPaste)
		{
			m_pwndPaste->CloseWindow();
			m_pwndPaste->DestroyWindow();
		}

		Log(_T("CloseQPasteWnd called closing qpastewnd from keyboard"));

		delete m_pwndPaste;
		m_pwndPaste = NULL;

		theApp.m_db.close();
		OpenDatabase(CGetSetOptions::GetDBPath());

		return;
	}

	if(CGetSetOptions::m_bShowPersistent && m_pwndPaste != NULL)
	{
		if (theApp.m_bShowingQuickPaste == false &&
			::IsWindow(m_pwndPaste->GetSafeHwnd()))
		{
			m_pwndPaste->ShowQPasteWindow(ResolveFillListRequest(m_pwndPaste, bReFillList));
		}

		BeginDeferredReveal(m_pwndPaste);
		m_pwndPaste->ShowWindow(SW_SHOW);
		m_pwndPaste->MinMaxWindow(FORCE_MAX);
		m_pwndPaste->SetForegroundWindow();
		EndDeferredReveal(m_pwndPaste);
		return;
	}
	
	int nPosition = CGetSetOptions::GetQuickPastePosition();
	
	CPoint point;
	CRect rcPrev;
	CSize csSize;
	
	if(!m_pwndPaste)
		m_pwndPaste = new CQPasteWnd;
	
	if(!m_pwndPaste)
	{
		ASSERT(FALSE);
		return;
	}

	m_pwndPaste->MinMaxWindow(FORCE_MAX);
	
	//If it is a window get the rect otherwise get the saved point and size
	if (IsWindow(m_pwndPaste->m_hWnd) &&
		m_pwndPaste->IsIconic() == FALSE &&
		m_forceResizeOnNextShow == false)
	{
		m_pwndPaste->GetWindowRect(rcPrev);
		csSize = rcPrev.Size();
	}
	else
	{
		CGetSetOptions::GetQuickPastePoint(point);
		CGetSetOptions::GetQuickPasteSize(csSize);

		if (IsWindow(m_pwndPaste->m_hWnd))
		{
			csSize.cx = m_pwndPaste->m_DittoWindow.m_dpi.Scale(csSize.cx);
			csSize.cy = m_pwndPaste->m_DittoWindow.m_dpi.Scale(csSize.cy);
		}
	}

	CPoint ptCaret = theApp.m_activeWnd.FocusCaret();
	if(ptCaret.x == -1 || ptCaret.y == -1)
	{
		CRect cr;
		::GetWindowRect(theApp.m_activeWnd.ActiveWnd(), cr);
		
		if(theApp.m_activeWnd.DesktopHasFocus() == false &&
			cr.Width() > 0 && 
			cr.Height() > 0)
		{
			ptCaret = cr.CenterPoint();
			ptCaret.x -= csSize.cx/2;
			ptCaret.y -= csSize.cy/2;
		}
		else
		{
			GetCursorPos(&point);

			CRect crPoint(point, CSize(1, 1));

			CRect crMonitor = MonitorRectFromRect(crPoint);

			ptCaret = crMonitor.CenterPoint();
			ptCaret.x -= csSize.cx/2;
			ptCaret.y -= csSize.cy/2;
		}
	}
	
	if(bAtPrevPos)
	{
		CGetSetOptions::GetQuickPastePoint(point);
		CGetSetOptions::GetQuickPasteSize(csSize);
	}
	else if (nPosition == POS_AT_CARET)
	{
		point = ptCaret;
		if (CGetSetOptions::m_centerWindowBelowCursorOrCaret)
		{
			point.x -= csSize.cx / 2;
		}
	}
	else if (nPosition == POS_AT_CURSOR)
	{
		GetCursorPos(&point);
		//keep the mouse from showing the tooltip because if overlaps with the top corner
		point.x += 2;
		point.y += 2;

		if (CGetSetOptions::m_centerWindowBelowCursorOrCaret)
		{
			point.x -= csSize.cx / 2;
		}
	}
	else if(nPosition == POS_AT_PREVIOUS)
		CGetSetOptions::GetQuickPastePoint(point);

	CRect crRect = CRect(point, csSize);

	bool forceMoveWindow = m_forceResizeOnNextShow;

	if(CGetSetOptions::m_bEnsureEntireWindowCanBeSeen)
	{
		if(EnsureWindowVisible(&crRect))
		{
			forceMoveWindow = true;
		}
	}
	
	if((crRect.left >= (crRect.right - 20)) ||
		(crRect.top >= (crRect.bottom - 20)))
	{
		CRect orig = crRect;
		crRect = CRect(ptCaret, CSize(300, 300));
		forceMoveWindow = true;

		Log(StrF(_T("Invalid initial size %d %d %d %d, Centered Window %d %d %d %d"), orig.left, orig.top, orig.right, orig.bottom, crRect.left, crRect.top, crRect.right, crRect.bottom));
	}	
	
	bool adjustRect = false;

	if( !IsWindow(m_pwndPaste->m_hWnd) )
	{
		CWnd *pLocalParent = pParent;

		if(CGetSetOptions::GetShowInTaskBar())
		{
			pLocalParent = NULL;
		}

		VERIFY( m_pwndPaste->Create(crRect, pLocalParent) );

		adjustRect = true;
	}	

	if (theApp.m_bShowingQuickPaste == false)
	{
		m_pwndPaste->ShowQPasteWindow(ResolveFillListRequest(m_pwndPaste, bReFillList));
	}

	//If minimized
	if (m_pwndPaste->IsIconic())
	{
		BeginDeferredReveal(m_pwndPaste);
		m_pwndPaste->ShowWindow(SW_RESTORE);

		if ((nPosition == POS_AT_CARET) ||
			(nPosition == POS_AT_CURSOR) ||
			bAtPrevPos ||
			forceMoveWindow)
		{
			if (adjustRect)
			{
				crRect.right = crRect.left + m_pwndPaste->m_DittoWindow.m_dpi.Scale(crRect.Width());
				crRect.bottom = crRect.top + m_pwndPaste->m_DittoWindow.m_dpi.Scale(crRect.Height());

				if (CGetSetOptions::m_bEnsureEntireWindowCanBeSeen)
				{
					EnsureWindowVisible(&crRect);
				}
			}

			m_pwndPaste->MoveWindow(crRect);
		}
	}
	else
	{
		if ((nPosition == POS_AT_CARET) ||
			(nPosition == POS_AT_CURSOR) ||
			bAtPrevPos ||
			forceMoveWindow)
		{
			if (adjustRect)
			{
				crRect.right = crRect.left + m_pwndPaste->m_DittoWindow.m_dpi.Scale(crRect.Width());
				crRect.bottom = crRect.top + m_pwndPaste->m_DittoWindow.m_dpi.Scale(crRect.Height());

				if (CGetSetOptions::m_bEnsureEntireWindowCanBeSeen)
				{
					EnsureWindowVisible(&crRect);
				}
			}
			m_pwndPaste->MoveWindow(crRect);
		}


		// Show the window
		BeginDeferredReveal(m_pwndPaste);
		m_pwndPaste->ShowWindow(SW_SHOW);
	}	

	m_pwndPaste->SetKeyModiferState(bFromKeyboard);

	// Refresh scrollbar colors to match current theme
	m_pwndPaste->RefreshScrollBarColors();
	
	m_pwndPaste->SetForegroundWindow();

	if (::IsWindow(m_pwndPaste->m_search.GetSafeHwnd()))
	{
		m_pwndPaste->m_search.SetFocus();
	}

	EndDeferredReveal(m_pwndPaste);

	Log(StrF(_T("END of ShowQPasteWnd, AtPrevPos: %d, FromKeyboard: %d, RefillList: %d, Position, %d %d %d %d"), bAtPrevPos, bFromKeyboard, bReFillList, crRect.left, crRect.top, crRect.right, crRect.bottom));

	m_forceResizeOnNextShow = false;
}

void CQuickPaste::MoveSelection(bool down)
{
	if(m_pwndPaste && CGetSetOptions::m_moveSelectionOnOpenHotkey)
	{
		if (IsWindow(m_pwndPaste->m_hWnd))
		{
			m_pwndPaste->MoveSelection(down, true);
		}
	}
}

void CQuickPaste::OnKeyStateUp()
{
	if(m_pwndPaste && CGetSetOptions::m_moveSelectionOnOpenHotkey)
	{
		if (IsWindow(m_pwndPaste->m_hWnd))
		{
			m_pwndPaste->OnKeyStateUp();
		}
	}
}

void CQuickPaste::SetKeyModiferState(bool bActive)
{
	if(m_pwndPaste && CGetSetOptions::m_moveSelectionOnOpenHotkey)
	{
		if (IsWindow(m_pwndPaste->m_hWnd))
		{
			m_pwndPaste->SetKeyModiferState(bActive);
		}
	}
}

void CQuickPaste::HideQPasteWnd()
{
	// Hide the window
	if(m_pwndPaste)
	{
		if (IsWindow(m_pwndPaste->m_hWnd))
			m_pwndPaste->HideQPasteWindow(true);
	}
}

BOOL CQuickPaste::IsWindowVisibleEx()
{
	if(m_pwndPaste)
		return IsWindowVisible(m_pwndPaste->m_hWnd);

	return FALSE;
}

bool CQuickPaste::IsWindowTopLevel()
{
	if(m_pwndPaste)
	{
		return ::GetForegroundWindow() == m_pwndPaste->GetSafeHwnd();
	}

	return false;
}

void CQuickPaste::OnScreenResolutionChange()
{
	if(m_pwndPaste != NULL &&
		::IsWindow(m_pwndPaste->m_hWnd) &&
		m_pwndPaste->IsIconic() == FALSE &&
		IsWindowVisibleEx())
	{
		Log(StrF(_T("Window Position changed, moving window to position as of this screen resolution %dx%d"), GetScreenWidth(), GetScreenHeight()));
		CPoint point;
		CSize csSize;

		CGetSetOptions::GetQuickPastePoint(point);
		CGetSetOptions::GetQuickPasteSize(csSize);

		csSize.cx = m_pwndPaste->m_DittoWindow.m_dpi.Scale(csSize.cx);
		csSize.cy = m_pwndPaste->m_DittoWindow.m_dpi.Scale(csSize.cy);

		m_pwndPaste->MoveWindow(point.x, point.y, csSize.cx, csSize.cy);
	}
	else
	{
		m_forceResizeOnNextShow = true;
	}
}
