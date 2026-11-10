#pragma once

#include "Clip.h"

class CClipFormatQListCtrl : public CClipFormat
{
public:
	CClipFormatQListCtrl(void);
	~CClipFormatQListCtrl(void);

	int m_clipRow;
	bool m_convertedToSmallImage;
	ULONGLONG m_counter;
    UINT_PTR m_generation = 0;

	HGLOBAL GetDibFittingToHeight(CDC *pDc, int height, int maxWidth = 0);
};

