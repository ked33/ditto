#include "stdafx.h"
#include "AutoSendToClientThread.h"
#include "Misc.h"
#include "Options.h"
#include "CP_Main.h"
#include "client.h"

CAutoSendToClientThread::CAutoSendToClientThread(void)
{
	m_waitTimeout = 30000;
	m_threadName = "CAutoSendToClientThread";
	for(int eventEnum = 0; eventEnum < ECAUTOSENDTOCLIENTTHREADEVENTS_COUNT; eventEnum++)
	{
		AddEvent(eventEnum);
	}
}

CAutoSendToClientThread::~CAutoSendToClientThread(void)
{
	Stop();
}

void CAutoSendToClientThread::FireSendToClient(CClipList *pClipList)
{
	// Publish work before checking the worker lifetime. The timeout uses the
	// same lock, so it cannot decide to exit between Start and queue insertion.
	{
		ATL::CCritSecLock csLock(m_cs.m_sect);
		POSITION pos = pClipList->GetHeadPosition();
		while (pos)
			m_saveClips.AddTail(pClipList->GetNext(pos));
		pClipList->RemoveAll();
	}
	Start();
	if (IsRunning())
		FireEvent(SEND_TO_CLIENTS);
	else
		Log(_T("Error creating thread to send to clients"));
}

void CAutoSendToClientThread::OnTimeOut(void *param)
{
	ATL::CCritSecLock csLock(m_cs.m_sect);
	if (m_saveClips.IsEmpty())
		Stop(-1);
}

void CAutoSendToClientThread::OnEvent(int eventId, void *param)
{
	switch((eCAutoSendToClientThreadEvents)eventId)
	{
	case SEND_TO_CLIENTS:
		OnSendToClient();
		break;
	}
}

void CAutoSendToClientThread::OnSendToClient()
{
	CClipList *pLocalClips = new CClipList();

	//Save the clips locally
	{
		ATL::CCritSecLock csLock(m_cs.m_sect);

		POSITION pos;
		CClip* pClip;

		pos = m_saveClips.GetHeadPosition();
		while(pos)
		{
			pClip = m_saveClips.GetNext(pos);
			pLocalClips->AddTail(pClip);
		}

		//pLocalClips now own, the clips
		m_saveClips.RemoveAll();
	}

	SendToClient(pLocalClips);

	delete pLocalClips;
	pLocalClips = NULL;
}

bool CAutoSendToClientThread::SendToClient(CClipList *pClipList)
{
	LogSendRecieveInfo("@@@@@@@@@@@@@@@ - START OF SendClientThread - @@@@@@@@@@@@@@@");

	if(pClipList == NULL)
	{
		LogSendRecieveInfo("ERROR if(pClipList == NULL)");
		return FALSE;
	}

	INT_PTR lCount = pClipList->GetCount();

	LogSendRecieveInfo(StrF(_T("Start of Send ClientThread Count - %d"), lCount));

	for(int nClient = 0; nClient < MAX_SEND_CLIENTS && !IsCancelled(); nClient++)
	{
		if(CGetSetOptions::m_SendClients[nClient].bSendAll && 
			CGetSetOptions::m_SendClients[nClient].csIP.GetLength() > 0)
		{
			CClient client;
			client.SetCancellationFlag(&m_exitThread);
			if(client.OpenConnection(CGetSetOptions::m_SendClients[nClient].csIP) == FALSE)
			{
				if (IsCancelled())
					return false;
				LogSendRecieveInfo(StrF(_T("ERROR opening connection to %s"), CGetSetOptions::m_SendClients[nClient].csIP));

				if(CGetSetOptions::m_SendClients[nClient].bShownFirstError == FALSE)
				{
					CString cs;
					cs.Format(_T("Error opening connection to %s"), CGetSetOptions::m_SendClients[nClient].csIP);
					::SendMessage(theApp.m_MainhWnd, WM_SEND_RECIEVE_ERROR, (WPARAM)cs.GetBuffer(cs.GetLength()), 0);
					cs.ReleaseBuffer();

					CGetSetOptions::m_SendClients[nClient].bShownFirstError = TRUE;
				}

				continue;
			}

			//We were connected successfully show an error next time we can't connect
			CGetSetOptions::m_SendClients[nClient].bShownFirstError = FALSE;

			CClip* pClip;
			POSITION pos;
			pos = pClipList->GetHeadPosition();
			while(pos && !IsCancelled())
			{
				pClip = pClipList->GetNext(pos);
				if(pClip == NULL)
				{
					ASSERT(FALSE);
					LogSendRecieveInfo("Error in GetNext");
					break;
				}

				LogSendRecieveInfo(StrF(_T("Sending clip to %s"), CGetSetOptions::m_SendClients[nClient].csIP));

				if(client.SendItem(pClip, false) == FALSE)
				{
					if (IsCancelled())
						return false;
					CString cs;
					cs.Format(_T("Error sending clip to %s"), CGetSetOptions::m_SendClients[nClient].csIP);
					::SendMessage(theApp.m_MainhWnd, WM_SEND_RECIEVE_ERROR, (WPARAM)cs.GetBuffer(cs.GetLength()), 0);
					cs.ReleaseBuffer();
					break;
				}
			}

			client.CloseConnection();
		}
	}

	LogSendRecieveInfo("@@@@@@@@@@@@@@@ - END OF SendClientThread - @@@@@@@@@@@@@@@");

	return TRUE;
}