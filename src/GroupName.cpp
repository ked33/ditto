// GroupName.cpp : implementation file
//

#include "stdafx.h"
#include "cp_main.h"
#include "GroupName.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CGroupName dialog


CGroupName::CGroupName(CWnd* pParent, LPCTSTR title, LPCTSTR prompt)
	: CDialog(CGroupName::IDD, pParent), m_title(title), m_prompt(prompt)
{
	//{{AFX_DATA_INIT(CGroupName)
	m_csName = _T("");
	//}}AFX_DATA_INIT
}


void CGroupName::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(CGroupName)
	DDX_Text(pDX, IDC_NAME, m_csName);
	//}}AFX_DATA_MAP
}


BEGIN_MESSAGE_MAP(CGroupName, CDialog)
	//{{AFX_MSG_MAP(CGroupName)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CGroupName message handlers

void CGroupName::OnOK() 
{
	UpdateData(TRUE);
	
	CDialog::OnOK();
}

BOOL CGroupName::OnInitDialog() 
{
	CDialog::OnInitDialog();

	SetWindowText(theApp.m_Language.GetString(m_title, m_title));
	SetDlgItemText(IDC_STATIC_NAME, theApp.m_Language.GetString(m_prompt, m_prompt));
	SetDlgItemText(IDOK, theApp.m_Language.GetString(_T("OK"), _T("OK")));
	SetDlgItemText(IDCANCEL, theApp.m_Language.GetString(_T("Cancel"), _T("Cancel")));

	CWnd *pWnd = GetDlgItem(IDC_NAME);
	if(pWnd)
		pWnd->SetFocus();
		
	return FALSE;
}
