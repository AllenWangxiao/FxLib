﻿#include "mytcpsock.h"
#include "sockmgr.h"
#include <stdio.h>
#include "connectionmgr.h"
#include "connection.h"
#include "iothread.h"
#include "net.h"
#include "netstream.h"

#ifdef WIN32
struct tcp_keepalive
{
	u_long  onoff;
	u_long  keepalivetime;
	u_long  keepaliveinterval;
};
#define SIO_KEEPALIVE_VALS _WSAIOW(IOC_VENDOR,4)
#else
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <execinfo.h>
#include <pthread.h>
int     MAX_SYS_SEND_BUF = (128 * 1024);
int     VAL_SO_SNDLOWAT = (64 * 1024);
#endif // WIN32

#define RECV_BUFF_SIZE 8*64*1024
#define SEND_BUFF_SIZE 64*64*1024

FxTCPListenSock::FxTCPListenSock()
{
	// 千万不要在这里有给指针赋值的操作 因为 会复制构造//
	Reset();

	SetState(SSTATE_INVALID);
	SetSock(INVALID_SOCKET);
	//m_dwLastError = 0;
	//m_bSendLinger = false;     // 发送延迟，直到成功，或者30次后，这时默认设置//
	m_poSessionFactory = NULL;
}


FxTCPListenSock::~FxTCPListenSock()
{
	if (m_poSessionFactory)
	{
		delete m_poSessionFactory;
		m_poSessionFactory = NULL;
	}
}

bool FxTCPListenSock::Init()
{
	return true;
}

void FxTCPListenSock::OnRead()
{
}

void FxTCPListenSock::OnWrite()
{
}

bool FxTCPListenSock::Listen(UINT32 dwIP, UINT16 wPort)
{
	SetSock(socket(AF_INET, SOCK_STREAM, 0));
	if (INVALID_SOCKET == GetSock())
	{

#ifdef WIN32
		int dwErr = WSAGetLastError();
		LogFun(LT_Screen | LT_File, LogLv_Error, "create socket error, %u:%u, errno %d", dwIP, wPort, dwErr);
#else
		LogFun(LT_Screen | LT_File, LogLv_Error, "create socket error, %u:%u, errno %d", dwIP, wPort, errno);
#endif // WIN32

		return false;
	}

	INT32 nReuse = 1;
	setsockopt(GetSock(), SOL_SOCKET, SO_REUSEADDR, (char*)&nReuse, sizeof(nReuse));

	sockaddr_in stAddr = { 0 };
	stAddr.sin_family = AF_INET;
	if (0 == dwIP)
	{
		stAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	}
	else
	{
		stAddr.sin_addr.s_addr = dwIP;
	}
	stAddr.sin_port = htons(wPort);

	if (bind(GetSock(), (sockaddr*)&stAddr, sizeof(stAddr)) < 0)
	{
#ifdef WIN32
		int dwErr = WSAGetLastError();
		LogFun(LT_Screen | LT_File, LogLv_Error, "bind at %u:%d failed, errno %d", dwIP, wPort, dwErr);
#else
		LogFun(LT_Screen | LT_File, LogLv_Error, "bind at %u:%d failed, errno %d", dwIP, wPort, errno);
#endif // WIN32
		return false;
	}
	if (listen(GetSock(), 128) < 0)
	{
#ifdef WIN32
		int dwErr = WSAGetLastError();
		LogFun(LT_Screen | LT_File, LogLv_Error, "listen at %u:%d failed, errno %d", dwIP, wPort, dwErr);
#else
		LogFun(LT_Screen | LT_File, LogLv_Error, "listen at %u:%d failed, errno %d", dwIP, wPort, errno);
#endif // WIN32
		return false;
	}

	m_poIoThreadHandler = FxNetModule::Instance()->FetchIoThread(GetSockId());
	if (NULL == m_poIoThreadHandler)
	{
		Close();
		return false;
	}

	SetState(SSTATE_LISTEN);

	// 添加到事件 //
	if (false == AddEvent())
	{
		return false;
	}
#ifdef WIN32

	if (false == InitAcceptEx())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "CCpListener::Start, InitAcceptEx failed");

		return false;
	}

	for (INT32 i = 0; i < sizeof(m_oSPerIoDatas) / sizeof(m_oSPerIoDatas[0]); i++)
	{
		if (false == PostAccept(m_oSPerIoDatas[i]))
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "CCpListener::Start, PostAccept failed");
			return false;
		}
	}
#else
#endif // WIN32
	LogFun(LT_Screen | LT_File, LogLv_Info, "listen at %u:%d success", dwIP, wPort);
	return true;
}

bool FxTCPListenSock::StopListen()
{
	if (SSTATE_LISTEN != GetState())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "state : %d != SSTATE_LISTEN", (UINT32)GetState());
		return false;
	}

	if (INVALID_SOCKET == GetSock())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "socket : %d == INVALID_SOCKET", GetSock());
		return false;
	}

#ifdef WIN32
	shutdown(GetSock(), SD_RECEIVE);
#else
	shutdown(GetSock(), SHUT_RD);
#endif // WIN32

	SetState(SSTATE_STOP_LISTEN);

	// 不close 如需要 另外close
	return true;
}

bool FxTCPListenSock::Close()
{
	m_oLock.Lock();
	if (GetState() == SSTATE_CLOSE)
	{
		m_oLock.UnLock();
		return true;
	}
	SetState(SSTATE_CLOSE);
#ifdef WIN32
	closesocket(GetSock());
#else
	m_poIoThreadHandler->DelEvent(GetSock());
	close(GetSock());
#endif // WIN32

	SetSock(INVALID_SOCKET);
	
	PushNetEvent(NETEVT_TERMINATE, 0);
	m_oLock.UnLock();

	return true;
}

void FxTCPListenSock::Reset()
{
	m_poSessionFactory = NULL;
	SetState(SSTATE_INVALID);
	SetSock(INVALID_SOCKET);
}

bool FxTCPListenSock::PushNetEvent(ENetEvtType eType, UINT32 dwValue)
{
	if (SSTATE_INVALID == GetState())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "state : %d == SSTATE_INVALID", (UINT32)GetState());

		return false;
	}
	SNetEvent oEvent;
	// 先扔网络事件进去，然后在报告上层有事件，先后顺序不能错，这样上层就不会错取事件//
	oEvent.eType = eType;
	oEvent.dwValue = dwValue;

	while (!FxNetModule::Instance()->PushNetEvent(this, oEvent))
	{
		FxSleep(1);
	}

	return true;
}

bool FxTCPListenSock::AddEvent()
{
#ifdef WIN32
	if (!m_poIoThreadHandler->AddEvent(GetSock(), this))
	{
		PushNetEvent(NETEVT_ERROR, WSAGetLastError());
		Close();
		return false;
	}
#else
	if (!m_poIoThreadHandler->AddEvent(GetSock(), EPOLLIN, this))
	{
		PushNetEvent(NETEVT_ERROR, errno);
		Close();
		return false;
	}
#endif // WIN32

	PushNetEvent(NETEVT_ASSOCIATE, 0);
	return true;

}

void FxTCPListenSock::ProcEvent(SNetEvent oEvent)
{
	{
		switch (oEvent.eType)
		{
		case NETEVT_ASSOCIATE:
		{
			__ProcAssociate();
		}
			break;

		case NETEVT_ERROR:
		{
			__ProcError(oEvent.dwValue);
		}
			break;

		case NETEVT_TERMINATE:
		{
			__ProcTerminate();
		}
			break;

		default:
		{
				   Assert(0);
		}
			break;
		}
	}
}

void FxTCPListenSock::__ProcAssociate()
{

}

void FxTCPListenSock::__ProcError(UINT32 dwErrorNo)
{

}

void FxTCPListenSock::__ProcTerminate()
{
}

#ifdef WIN32
bool FxTCPListenSock::PostAccept(SPerIoData& oSPerIoData)
{
	SOCKET hNewSock = WSASocket(
		AF_INET,
		SOCK_STREAM,
		0,
		NULL,
		0,
		WSA_FLAG_OVERLAPPED);

	if (INVALID_SOCKET == hNewSock)
	{
		int dwErr = WSAGetLastError();
		LogFun(LT_Screen | LT_File, LogLv_Error, "WSASocket failed, errno %d", dwErr);
		return false;
	}

	unsigned long ul = 1;
	if (SOCKET_ERROR == ioctlsocket(hNewSock, FIONBIO, (unsigned long*)&ul))
	{
		int dwErr = WSAGetLastError();
		LogFun(LT_Screen | LT_File, LogLv_Error, "Set socket FIONBIO error : %d", dwErr);

		closesocket(hNewSock);
		return false;
	}

	int nSendBuffSize = 256 * 1024;
	int nRecvBuffSize = 8 * nSendBuffSize;
	if ((0 != setsockopt(hNewSock, SOL_SOCKET, SO_RCVBUF, (char*)&nRecvBuffSize, sizeof(int))) ||
		(0 != setsockopt(hNewSock, SOL_SOCKET, SO_SNDBUF, (char*)&nSendBuffSize, sizeof(int))))
	{
		int nError = WSAGetLastError();
		LogFun(LT_Screen | LT_File, LogLv_Error, "Set socket setsockopt error : %d", nError);
		closesocket(hNewSock);
		return false;
	}

	memset(&(oSPerIoData.stOverlapped), 0, sizeof(oSPerIoData.stOverlapped));
	oSPerIoData.hSock = hNewSock;
	oSPerIoData.nOp = IOCP_ACCEPT;

	DWORD dwBytes;
	BOOL bRet = m_lpfnAcceptEx(
		GetSock(),
		hNewSock,
		oSPerIoData.Buf,
		0,
		sizeof(SOCKADDR_IN)+16,
		sizeof(SOCKADDR_IN)+16,
		&dwBytes,
		&oSPerIoData.stOverlapped);

	if (false == bRet)
	{
		int nError = WSAGetLastError();
		if (WSA_IO_PENDING != nError)
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "Init AcceptEx failed, errno %d", nError);

			closesocket(hNewSock);
			return false;
		}
	}

	return true;
}

bool FxTCPListenSock::InitAcceptEx()
{
	DWORD dwbytes = 0;

	GUID m_GuidAcceptEx = WSAID_ACCEPTEX;

	int nRt = ::WSAIoctl(
		GetSock(),
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&m_GuidAcceptEx,
		sizeof(m_GuidAcceptEx),
		&m_lpfnAcceptEx,
		sizeof(LPFN_ACCEPTEX),
		&dwbytes,
		NULL,
		NULL);

	if (SOCKET_ERROR == nRt)
	{
		int dwErr = WSAGetLastError();
		LogFun(LT_Screen | LT_File, LogLv_Error, "WSAIoctl WSAID_ACCEPTEX failed, errno %d", dwErr);

		return false;
	}

	GUID m_GuidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;

	dwbytes = 0;

	nRt = ::WSAIoctl(
		GetSock(),
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&m_GuidGetAcceptExSockaddrs,
		sizeof(m_GuidGetAcceptExSockaddrs),
		&m_lpfnGetAcceptExSockaddrs,
		sizeof(LPFN_GETACCEPTEXSOCKADDRS),
		&dwbytes,
		NULL,
		NULL);

	if (SOCKET_ERROR == nRt)
	{
		int dwErr = WSAGetLastError();
		LogFun(LT_Screen | LT_File, LogLv_Error, "WSAIoctl WSAID_GETACCEPTEXSOCKADDRS failed, errno %d", dwErr);

		return false;
	}

	return true;
}

void FxTCPListenSock::OnParserIoEvent(bool bRet, void* pIoData, UINT32 dwByteTransferred)
{
	SPerIoData* pSPerIoData = (SPerIoData*)pIoData;
	switch (GetState())
	{
		case SSTATE_LISTEN:
		{
			m_oLock.Lock();
			if (false == bRet)
			{
				int dwErr = WSAGetLastError();
				LogFun(LT_Screen | LT_File, LogLv_Error, "OnParserIoEvent failed, errno %d", dwErr);

				closesocket(pSPerIoData->hSock);
				PostAccept(*pSPerIoData);
			}
			else
			{
				OnAccept(pSPerIoData);
			}
			m_oLock.UnLock();
		}
		break;
		case SSTATE_CLOSE:
		case SSTATE_STOP_LISTEN:
		{
			m_oLock.Lock();
			if (bRet)
			{
				LogFun(LT_Screen | LT_File, LogLv_Error, "%s", "listen socket has stoped but ret is true");
			}
			else
			{
				pSPerIoData->hSock = INVALID_SOCKET;
			}
			m_oLock.UnLock();
		}
		break;
		default:
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "state : %d != SSTATE_LISTEN", (UINT32)GetState());

			Close();        // 未知错误，不应该发生//
		}
		break;
	}
}

void FxTCPListenSock::OnAccept(SPerIoData* pstPerIoData)
{
	SOCKET hSock = pstPerIoData->hSock;

	if (SSTATE_LISTEN != GetState())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "state : %d != SSTATE_LISTEN", GetState());

		closesocket(hSock);
		return;
	}

	{
		FxTCPConnectSock* poSock = FxMySockMgr::Instance()->CreateTcpSock();
		if (NULL == poSock)
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "CCPSock::OnAccept, create CCPSock failed");

			closesocket(hSock);
			PostAccept(*pstPerIoData);
			return;
		}

		FxIoThread* poIoThreadHandler = FxNetModule::Instance()->FetchIoThread(poSock->GetSockId());
		if (NULL == poIoThreadHandler)
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "CCPSock::OnAccept, get iothread failed");

			closesocket(hSock);
			PostAccept(*pstPerIoData);
			return;
		}

		FxConnection* poConnection = FxConnectionMgr::Instance()->Create();
		if (NULL == poConnection)
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "CCPSock::OnAccept, create Connection failed");

			closesocket(hSock);
			PostAccept(*pstPerIoData);
			FxMySockMgr::Instance()->ReleaseTcpSock(poSock);
			return;
		}

		::setsockopt(hSock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&(GetSock()), sizeof(SOCKET));

		poSock->SetSock(hSock);
		poSock->SetConnection(poConnection);

		poConnection->SetSockType(SOCKTYPE_TCP);
		poConnection->SetSock(poSock);
		poConnection->SetID(poSock->GetSockId());

		sockaddr_in* pstRemoteAddr = NULL;
		sockaddr_in* pstLocalAddr = NULL;
		INT32 nRemoteAddrLen = sizeof(sockaddr_in);
		INT32 nLocalAddrLen = sizeof(sockaddr_in);
		INT32 nAddrLen = sizeof(sockaddr_in)+16;

		m_lpfnGetAcceptExSockaddrs(
			pstPerIoData->Buf,
			0,
			nAddrLen,
			nAddrLen,
			(SOCKADDR**)&pstLocalAddr,
			&nLocalAddrLen,
			(SOCKADDR**)&pstRemoteAddr,
			&nRemoteAddrLen);

		poConnection->SetLocalIP(pstLocalAddr->sin_addr.s_addr);
		poConnection->SetLocalPort(ntohs(pstLocalAddr->sin_port));

		poConnection->SetRemoteIP(pstRemoteAddr->sin_addr.s_addr);
		poConnection->SetRemotePort(ntohs(pstRemoteAddr->sin_port));

		FxSession* poSession = m_poSessionFactory->CreateSession();
		if (NULL == poSession)
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "CCPSock::OnAccept, CreateSession failed");

			closesocket(hSock);
			PostAccept(*pstPerIoData);
			FxMySockMgr::Instance()->ReleaseTcpSock(poSock);
			FxConnectionMgr::Instance()->Release(poConnection);
			return;
		}

		poSession->Init(poConnection);
		poConnection->SetSession(poSession);
		poSock->SetIoThread(poIoThreadHandler);

		poSock->SetState(SSTATE_ESTABLISH);

		// keep alive
		struct tcp_keepalive keepAliveIn;
		struct tcp_keepalive keepAliveOut;

		unsigned long ulBytesReturn = 0;

		keepAliveIn.keepaliveinterval = 10000;//
		keepAliveIn.keepalivetime = 1000 * 30;//
		keepAliveIn.onoff = 1;

		int ret = WSAIoctl
			(
			poSock->GetSock(),
			SIO_KEEPALIVE_VALS,
			&keepAliveIn,
			sizeof(keepAliveIn),
			&keepAliveOut,
			sizeof(keepAliveOut),
			&ulBytesReturn,
			NULL,
			NULL
			);
		if (ret == SOCKET_ERROR)
		{
			int dwErr = WSAGetLastError();
			LogFun(LT_Screen | LT_File, LogLv_Error, "Set keep alive error: %d", dwErr);

			PostAccept(*pstPerIoData);
			poSock->PushNetEvent(NETEVT_ERROR, dwErr);
			poSock->Close();
			return;
		}

		//
		// 应该先投递连接事件再关联套接口，否则可能出现第一个Recv事件先于连接事件入队列
		//

		if (false == poSock->AddEvent())
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "poSock->AddEvent failed");

			poSock->Close();
		}
		else
		{
			poSock->PushNetEvent(NETEVT_ESTABLISH, 0);

			if (false == poSock->PostRecv())
			{
				int dwErr = WSAGetLastError();
				poSock->PushNetEvent(NETEVT_ERROR, dwErr);
				LogFun(LT_Screen | LT_File, LogLv_Error, "poSock->PostRecv failed, errno : %d", dwErr);

				poSock->Close();
			}
		}

		PostAccept(*pstPerIoData);
	}
}

#else
void FxTCPListenSock::OnParserIoEvent(int dwEvents)
{
	if (dwEvents & EPOLLERR)
	{
		PushNetEvent(NETEVT_ERROR, errno);
		LogFun(LT_Screen | LT_File, LogLv_Error, "get error event errno : %d", errno);

		Close();
		return;
	}

	if (dwEvents & EPOLLIN)
	{
		OnAccept();
	}
}

void FxTCPListenSock::OnAccept()
{
	sockaddr_in stLocalAddr;
	sockaddr_in stRemoteAddr;
	UINT32 dwAddrLen = sizeof(stRemoteAddr);
	UINT32 hAcceptSock = accept(GetSock(), (sockaddr*)&stRemoteAddr, &dwAddrLen);
	if (INVALID_SOCKET == hAcceptSock)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "%s", "INVALID_SOCKET == hAcceptSock");

		return;
	}

	FxTCPConnectSock* poSock = FxMySockMgr::Instance()->CreateTcpSock();
	if (NULL == poSock)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "%s", "create FxConnectSock failed");

		close(hAcceptSock);
		return;
	}

	FxIoThread* poEpollHandler = FxNetModule::Instance()->FetchIoThread(poSock->GetSockId());
	if (NULL == poEpollHandler)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "%s", "NULL == poEpollHandler");

		close(hAcceptSock);
		return;
	}

	FxConnection* poConnection = FxConnectionMgr::Instance()->Create();
	if (NULL == poConnection)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "%s", "NULL == poConnection");

		close(hAcceptSock);
		FxMySockMgr::Instance()->ReleaseTcpSock(poSock);
		return;
	}

	FxSession* poSession = m_poSessionFactory->CreateSession();
	if (NULL == poSession)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "%s", "NULL == poSession");

		close(hAcceptSock);
		FxMySockMgr::Instance()->ReleaseTcpSock(poSock);
		FxConnectionMgr::Instance()->Release(poConnection);
		return;
	}

	setsockopt(GetSock(), SOL_SOCKET, SO_SNDLOWAT, &VAL_SO_SNDLOWAT, sizeof(VAL_SO_SNDLOWAT));
	setsockopt(GetSock(), SOL_SOCKET, SO_SNDBUF, &MAX_SYS_SEND_BUF, sizeof(MAX_SYS_SEND_BUF));

	dwAddrLen = sizeof(stLocalAddr);
	getsockname(hAcceptSock, (sockaddr*)&stLocalAddr, &dwAddrLen);

	poSession->Init(poConnection);
	poConnection->SetSockType(SOCKTYPE_TCP);
	poConnection->SetID(poSock->GetSockId());
	poConnection->SetSock(poSock);
	poConnection->SetSession(poSession);

	poConnection->SetLocalIP(stLocalAddr.sin_addr.s_addr);
	poConnection->SetLocalPort(ntohs(stLocalAddr.sin_port));
	poConnection->SetRemoteIP(stRemoteAddr.sin_addr.s_addr);
	poConnection->SetRemotePort(ntohs(stRemoteAddr.sin_port));

	poSock->SetSock(hAcceptSock);
	poSock->SetConnection(poConnection);
	poSock->SetIoThread(poEpollHandler);

	poSock->SetState(SSTATE_ESTABLISH);

	// keep alive
	int keepAlive = 1;
	setsockopt(GetSock(), SOL_SOCKET, SO_KEEPALIVE, (void*)&keepAlive, sizeof(keepAlive));

	int keepIdle = 30;
	int keepInterval = 5;
	int keepCount = 6;
	setsockopt(GetSock(), SOL_TCP, TCP_KEEPIDLE, (void *)&keepIdle, sizeof(keepIdle));
	setsockopt(GetSock(), SOL_TCP, TCP_KEEPINTVL, (void *)&keepInterval, sizeof(keepInterval));
	setsockopt(GetSock(), SOL_TCP, TCP_KEEPCNT, (void *)&keepCount, sizeof(keepCount));

	if(!poSock->AddEvent())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "%s", "poSock->AddEvent() failed");

		close(hAcceptSock);
		FxMySockMgr::Instance()->ReleaseTcpSock(poSock);

		poSession->Release();
		return;
	}
	poSock->PushNetEvent(NETEVT_ESTABLISH, 0);

	return;
}

#endif // WIN32

/************************************************************************/
/*                                                                      */
/************************************************************************/

FxTCPConnectSock::FxTCPConnectSock()
{
	//m_pListenSocket = NULL;
	m_poSendBuf = NULL;
	m_poRecvBuf = NULL;
	m_poConnection = NULL;
	SetState(SSTATE_INVALID);
	SetSock(INVALID_SOCKET);
	m_nNeedData = 0;
	m_nPacketLen = 0;
#ifdef WIN32
	m_dwLastError = 0;
	m_stRecvIoData.nOp = IOCP_RECV;
	m_stSendIoData.nOp = IOCP_SEND;
	m_nPostRecv = 0;
	m_nPostSend = 0;
	m_dwLastError = 0;
#else
	m_bSending = false;
#endif // WIN32

	m_bSendLinger = false;     // 发送延迟，直到成功，或者30次后，这时默认设置//

	Reset();
}

FxTCPConnectSock::~FxTCPConnectSock()
{
	if (m_poConnection)
	{
		// 既然是要销毁 那么应该通知 将相应指针置零//
		m_poConnection->OnSocketDestroy();
		m_poConnection = NULL;
	}

	if (m_poRecvBuf)
	{
		FxLoopBuffMgr::Instance()->Release(m_poRecvBuf);
		m_poRecvBuf = NULL;
	}
	if (m_poSendBuf)
	{
		FxLoopBuffMgr::Instance()->Release(m_poSendBuf);
		m_poSendBuf = NULL;
	}

}

bool FxTCPConnectSock::Init()
{
	if (NULL == m_poSendBuf)
	{
		m_poSendBuf = FxLoopBuffMgr::Instance()->Fetch();
		if (NULL == m_poSendBuf)
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "NULL == m_poSendBuf, socket : %d, socket id : %d", GetSock(), GetSockId());
			return false;
		}
	}

	if (NULL == m_poRecvBuf)
	{
		m_poRecvBuf = FxLoopBuffMgr::Instance()->Fetch();
		if (NULL == m_poRecvBuf)
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "NULL == m_poRecvBuf, socket : %d, socket id : %d", GetSock(), GetSockId());

			return false;
		}
	}

	if (!m_poRecvBuf->Init(RECV_BUFF_SIZE))
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "m_poRecvBuf->Init failed, socket : %d, socket id : %d", GetSock(), GetSockId());

		return false;
	}

	if (!m_poSendBuf->Init(SEND_BUFF_SIZE))
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "m_poSendBuf->Init failed, socket : %d, socket id : %d", GetSock(), GetSockId());

		return false;
	}
	return true;
}

void FxTCPConnectSock::OnRead()
{
}

void FxTCPConnectSock::OnWrite()
{
}

bool FxTCPConnectSock::Close()
{
	// 首先 把数据先发过去//
	m_oLock.Lock();

	if(GetState() == SSTATE_RELEASE || GetState() == SSTATE_CLOSE)
	{
		m_oLock.UnLock();
		return true;
	}

	if (IsConnected())
	{
		SetState(SSTATE_CLOSE);
	}
	if (GetSock() == INVALID_SOCKET)
	{
		m_oLock.UnLock();
		return true;
	}

#ifdef WIN32
	shutdown(GetSock(), SD_RECEIVE);
#else
	shutdown(GetSock(), SHUT_RD);
	m_poIoThreadHandler->DelEvent(GetSock());
	// 有bug 就不发了 修改后再说//
	//SendImmediately();
#endif	//WIN32

#ifdef WIN32
	if (0 != m_dwLastError)
	{
		PushNetEvent(NETEVT_ERROR, m_dwLastError);
		m_dwLastError = 0;
	}
#endif // WIN32

#ifdef WIN32
	CancelIo((HANDLE)GetSock());
	closesocket(GetSock());
#else
	close(GetSock());
#endif // WIN32

	SetSock(INVALID_SOCKET);

	PushNetEvent(NETEVT_TERMINATE, 0);
	m_oLock.UnLock();

	return true;
}

void FxTCPConnectSock::Reset()
{
	if (NULL != GetConnection())
	{
		// 既然是要销毁 那么应该通知 将相应指针置零//
		GetConnection()->OnSocketDestroy();
		SetConnection(NULL);
	}

	//m_pListenSocket = NULL;
	SetState(SSTATE_INVALID);
	SetSock(INVALID_SOCKET);
	m_nNeedData = 0;
	m_nPacketLen = 0;

	if (m_poRecvBuf)
	{
		FxLoopBuffMgr::Instance()->Release(m_poRecvBuf);
		m_poRecvBuf = NULL;
	}
	if (m_poSendBuf)
	{
		FxLoopBuffMgr::Instance()->Release(m_poSendBuf);
		m_poSendBuf = NULL;
	}

#ifdef WIN32
	m_dwLastError = 0;
	m_nPostRecv = 0;
	m_nPostSend = 0;
	m_dwLastError = 0;
#else
	m_bSending = false;
#endif // WIN32
	m_bSendLinger = false;     //发送延迟，直到成功，或者30次后，这时默认设置//
}

// win下出现问题时 要投递关闭信息 因为需要一个post关闭的过程//
bool FxTCPConnectSock::Send(const char* pData, int dwLen)
{
	if (false == IsConnected())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "socket not connected, socket : %d, socket id : %d", GetSock(), GetSockId());

		return false;
	}

	if (m_poSendBuf->IsEmpty())
	{
		m_poSendBuf->Clear();
	}

	IFxDataHeader* pDataHeader = GetDataHeader();
	if (pDataHeader == NULL)
	{
#ifdef WIN32
		m_dwLastError = NET_SEND_OVERFLOW;
		LogFun(LT_Screen | LT_File, LogLv_Error, "send error pDataHeader == NULL, socket : %d, socket id : %d", GetSock(), GetSockId());

		PostClose();
#else
		PushNetEvent(NETEVT_ERROR, NET_SEND_OVERFLOW);
		LogFun(LT_Screen | LT_File, LogLv_Error, "send error pDataHeader == NULL, socket : %d, socket id : %d", GetSock(), GetSockId());

		Close();
#endif // WIN32
		return false;
	}

	if ((unsigned int)dwLen + pDataHeader->GetHeaderLength() > (unsigned int)m_poSendBuf->GetTotalLen())
	{
#ifdef WIN32
		m_dwLastError = NET_SEND_OVERFLOW;
		LogFun(LT_Screen | LT_File, LogLv_Error, "send error NET_SEND_OVERFLOW, socket : %d, socket id : %d", GetSock(), GetSockId());

		PostClose();
#else
		PushNetEvent(NETEVT_ERROR, NET_SEND_OVERFLOW);
		LogFun(LT_Screen | LT_File, LogLv_Error, "send error NET_SEND_OVERFLOW, socket : %d, socket id : %d", GetSock(), GetSockId());

		Close();
#endif // WIN32
		return false;
	}

	// 这个是在主线程调用 所以 声明为静态就可以了 防止重复生成 占用空间
	static char pTemData[RECV_BUFF_SIZE] = {0};
	CNetStream oNetStream(ENetStreamType_Write, pTemData, dwLen + pDataHeader->GetHeaderLength());
	oNetStream.WriteData((char*)(pDataHeader->BuildSendPkgHeader(dwLen)), pDataHeader->GetHeaderLength());
	oNetStream.WriteData(pData, dwLen);

	int nSendCount = 0;
	while (!m_poSendBuf->PushBuff(pTemData, dwLen + pDataHeader->GetHeaderLength()))
	{
		if (!m_bSendLinger || 30 < ++nSendCount)  // 连续30次还没发出去，就认为失败，失败结果逻辑层处理//
		{
			LogFun(LT_Screen | LT_File, LogLv_Critical, "send buffer overflow!!!!!!!!, socket : %d, socket id : %d", GetSock(), GetSockId());
			return false;
		}
		FxSleep(10);
	}

	if (false == PostSendFree())
	{
#ifdef WIN32
		m_dwLastError = WSAGetLastError();
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == PostSendFree(), socket : %d, socket id : %d", GetSock(), GetSockId());

		PostClose();
#else
		PushNetEvent(NETEVT_ERROR, NET_SEND_OVERFLOW);
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == PostSendFree(), socket : %d, socket id : %d", GetSock(), GetSockId());

		Close();
#endif // WIN32
		return false;
	}

	return true;
}

bool FxTCPConnectSock::PushNetEvent(ENetEvtType eType, UINT32 dwValue)
{
	SNetEvent oEvent;
	// 先扔网络事件进去，然后在报告上层有事件，先后顺序不能错，这样上层就不会错取事件//
	oEvent.eType = eType;
	oEvent.dwValue = dwValue;

	while (!FxNetModule::Instance()->PushNetEvent(this, oEvent))
	{
		FxSleep(1);
	}
	return true;
}

bool FxTCPConnectSock::PostSend()
{
#ifdef WIN32
	if (false == IsConnected())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == IsConnect(), socket : %d, socket id : %d", GetSock(), GetSockId());

		return false;
	}

	LONG nPostSend = InterlockedCompareExchange(&m_nPostSend, 1, 0);
	if (0 != nPostSend)
	{
		return true;
	}

	ZeroMemory(&m_stSendIoData.stOverlapped, sizeof(m_stSendIoData.stOverlapped));
	int nLen = m_poSendBuf->GetOutCursorPtr(m_stSendIoData.stWsaBuf.buf);
	if (0 >= nLen)
	{
		//LogFun(LT_Screen | LT_File, LogLv_Critical, "m_poSendBuf->GetOutCursorPtr() = %d, socket : %d, socket id : %d", nLen, GetSock(), GetSockId());
		// 不算失败，只是没有投递而已，下次可以继续//
		InterlockedCompareExchange(&m_nPostSend, 0, 1);
		return true;
	}

	nLen = 65536 < nLen ? 65536 : nLen;     // ???64K

	m_stSendIoData.stWsaBuf.len = nLen;
	DWORD dwNumberOfBytesSent = 0;

	int nRet = WSASend(GetSock(), &m_stSendIoData.stWsaBuf, 1, &dwNumberOfBytesSent, 0, &m_stSendIoData.stOverlapped, NULL);
	if (0 != nRet)
	{
		if (WSAGetLastError() != WSA_IO_PENDING)
		{
			InterlockedCompareExchange(&m_nPostSend, 0, 1);

			UINT32 dwErr = WSAGetLastError();
			LogFun(LT_Screen | LT_File, LogLv_Error, "WSASend errno : %d, socket : %d, socket id : %d", WSAGetLastError(), GetSock(), GetSockId());

			return false;
		}
	}

	return true;
#else
	if (false == IsConnected())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == IsConnected(), socket : %d, socket id : %d", GetSock(), GetSockId());

		return false;
	}

	if (NULL == m_poIoThreadHandler)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "NULL == m_poIoThreadHandler, socket : %d, socket id : %d", GetSock(), GetSockId());

		Close();
		return false;
	}

	if (m_bSending)
	{
		return true;
	}

	char* pSendBuf = NULL;
	int nLen = m_poSendBuf->GetOutCursorPtr(pSendBuf);
	if (0 >= nLen || NULL == pSendBuf)
	{
//		LogFun(LT_Screen | LT_File, LogLv_Error, "m_poSendBuf->GetOutCursorPtr() = %d, socket : %d, socket id : %d", nLen, GetSock(), GetSockId());
		// 不算失败，只是没有投递而已，下次可以继续//
		// modify: 在这里需要设置事件为EPOLLIN，否则OUT事件一直被设置，导致CPU很高//
		if (false == m_poIoThreadHandler->ChangeEvent(GetSock(), EPOLLIN, this))
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "false == m_poIoThreadHandler->ChangeEvent, socket : %d, socket id : %d", GetSock(), GetSockId());

			PushNetEvent(NETEVT_ERROR, errno);
			Close();
			return false;
		}
		return true;
	}

	m_bSending = true;
	nLen = VAL_SO_SNDLOWAT < nLen ? VAL_SO_SNDLOWAT : nLen;     // ���64K

	int nRet = send(GetSock(), pSendBuf, nLen, 0);
	if (0 > nRet)
	{
		if (EAGAIN == errno)
		{
			if (false == m_poIoThreadHandler->ChangeEvent(GetSock(), EPOLLOUT | EPOLLIN, this))
			{
				LogFun(LT_Screen | LT_File, LogLv_Error, "false == m_poIoThreadHandler->ChangeEvent, socket : %d, socket id : %d", GetSock(), GetSockId());

				m_bSending = false;
				return false;
			}
			return true;
		}

		LogFun(LT_Screen | LT_File, LogLv_Error, "send nRet < 0, socket : %d, socket id : %d", GetSock(), GetSockId());

		m_bSending = false;
		return false;
	}
	else if (0 == nRet)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "send 0 == nRet, socket : %d, socket id : %d", GetSock(), GetSockId());

		m_bSending = false;
		return false;
	}

	// 把成功发送了的从发送缓冲区丢弃//
	m_poSendBuf->DiscardBuff(nRet);

	if (false == m_poIoThreadHandler->ChangeEvent(GetSock(), EPOLLOUT | EPOLLIN, this))
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == m_poIoThreadHandler->ChangeEvent, socket : %d, socket id : %d", GetSock(), GetSockId());

		m_bSending = false;
		PushNetEvent(NETEVT_ERROR, errno);
		Close();
		return false;
	}

	return true;
#endif // WIN32

}

bool FxTCPConnectSock::PostSendFree()
{
#ifdef WIN32
	return PostSend();
#else
	if (false == IsConnected())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == IsConnected(), socket : %d, socket id : %d", GetSock(), GetSockId());

		return false;
	}

	if (NULL == m_poIoThreadHandler)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "NULL == m_poIoThreadHandler, socket : %d, socket id : %d", GetSock(), GetSockId());

		Close();
		return false;
	}

	return m_poIoThreadHandler->ChangeEvent(GetSock(), EPOLLOUT | EPOLLIN, this);

	return true;
#endif // WIN32
}
bool FxTCPConnectSock::SendImmediately()
{
	if (GetState() != SSTATE_CLOSE)
	{
		return false;
	}

#ifdef WIN32
	if (m_dwLastError)
#else
	if(errno != 0)
#endif	//WIN32
	{
		// 已经出错了 不能发送//
		return true;
	}

	if (NULL == m_poIoThreadHandler)
	{
//		Close();
		return false;
	}

	while (true)
	{
#ifdef WIN32
		LONG nPostSend = InterlockedCompareExchange(&m_nPostSend, 1, 0);
		if (0 != nPostSend)
		{
			FxSleep(10);
		}
		else
		{
			break;
		}
#else
		if (m_bSending)
		{
			FxSleep(10);
		}
		else
		{
			break;
		}
#endif // WIN32
	}

	while (true)
	{
		if (false == IsConnected())
		{
			return false;
		}

		char* pSendBuf = NULL;
		int nLen = m_poSendBuf->GetOutCursorPtr(pSendBuf);
		if (0 >= nLen || NULL == pSendBuf)
		{
			return true;
		}

#ifdef WIN32
		InterlockedCompareExchange(&m_nPostSend, 0, 1);
#else
		m_bSending = true;
#endif // WIN32

		nLen = 64 * 1024 < nLen ? 64 * 1024 : nLen;     // 最大64K

		int nRet = send(GetSock(), pSendBuf, nLen, 0);
		if (0 > nRet)
		{
			UINT32 dwError = 0;
#ifdef WIN32
			dwError = WSAGetLastError();
			InterlockedCompareExchange(&m_nPostSend, m_nPostSend - 1, m_nPostSend);
#else
			m_bSending = false;
			dwError = errno;
#endif // WIN32
			//continue;
			if (dwError)
			{
				return false;
			}
			else
			{
				continue;
			}
			//return false;	//前面已经等待了一段时间 这个时候要是发不过去 可能是客户端那边已经断了 发不过去了//
		}
		else if (0 == nRet)
		{
			return true;
		}

		// 把成功发送了的从发送缓冲区丢弃//
		m_poSendBuf->DiscardBuff(nRet);
	}
	return true;
}

void FxTCPConnectSock::__ProcEstablish()
{
	if (GetConnection())
	{
		if (GetSockId() != GetConnection()->GetID())
		{
			return;
		}

		GetConnection()->OnConnect();
	}
}

void FxTCPConnectSock::__ProcAssociate()
{
	if (GetConnection())
	{
		if (GetSockId() != GetConnection()->GetID())
		{
			return;
		}

		GetConnection()->OnAssociate();
	}
}

void FxTCPConnectSock::__ProcConnectError(UINT32 dwErrorNo)
{
	if (GetConnection())
	{
		if (GetSockId() != GetConnection()->GetID())
		{
			return;
		}

		GetConnection()->OnConnError(dwErrorNo);
	}
}

void FxTCPConnectSock::__ProcError(UINT32 dwErrorNo)
{
	if (GetConnection())
	{
		if (GetSockId() != GetConnection()->GetID())
		{
			return;
		}

		GetConnection()->OnError(dwErrorNo);
	}
}

void FxTCPConnectSock::__ProcTerminate()
{
	PushNetEvent(NETEVT_RELEASE, 0);
}

void FxTCPConnectSock::__ProcRecv(UINT32 dwLen)
{
	if (GetConnection())
	{
		if (UINT32(-1) == dwLen)
		{
#ifdef WIN32
			PostRecvFree();
#else
			PushNetEvent(NETEVT_ERROR, NET_RECV_ERROR);
			Close();
#endif
			return;
		}

		if (GetSockId() != GetConnection()->GetID())
		{
			return;
		}

		if (GetConnection()->GetRecvSize() < dwLen)
		{
			PushNetEvent(NETEVT_ERROR, NET_RECV_ERROR);
#ifdef WIN32
			PostClose();
#else
			Close();
#endif // WIN32
			return;
		}

		if (!m_poRecvBuf->PopBuff(GetConnection()->GetRecvBuf(), dwLen))
		{
			return;
		}
		GetConnection()->OnRecv(dwLen);
	}
}

void FxTCPConnectSock::__ProcRelease()
{
	if (GetConnection())
	{
		if (GetSockId() != GetConnection()->GetID())
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "socket : %d, socket id : %d, connection id : %d, connection addr : %p",
				GetSock(), GetSockId(), GetConnection()->GetID(), GetConnection());
			return;
		}

		GetConnection()->OnSocketDestroy();
		GetConnection()->OnClose();

		SetConnection(NULL);
	}
	FxMySockMgr::Instance()->ReleaseTcpSock(this);
}

IFxDataHeader* FxTCPConnectSock::GetDataHeader()
{
	if (GetConnection())
	{
		return GetConnection()->GetDataHeader();
	}
	return NULL;
}

bool FxTCPConnectSock::AddEvent()
{
#ifdef WIN32
	if (!m_poIoThreadHandler->AddEvent(GetSock(), this))
	{
		PushNetEvent(NETEVT_ERROR, WSAGetLastError());
		LogFun(LT_Screen | LT_File, LogLv_Error, "error : %d, socket : %d, socket id : %d", WSAGetLastError(), GetSock(), GetSockId());

		Close();
		return false;
	}
#else
	if (!m_poIoThreadHandler->AddEvent(GetSock(), EPOLLOUT|EPOLLIN, this))
	{
		PushNetEvent(NETEVT_ERROR, errno);
		LogFun(LT_Screen | LT_File, LogLv_Error, "error : %d, socket : %d, socket id : %d", errno, GetSock(), GetSockId());

		Close();
		return false;
	}
#endif // WIN32

	PushNetEvent(NETEVT_ASSOCIATE, 0);
	return true;

}

SOCKET FxTCPConnectSock::Connect()
{
	if (IsConnected())
	{
		return INVALID_SOCKET;
	}

	if (NULL == GetConnection())
	{
		return INVALID_SOCKET;
	}

	if (GetConnection()->IsConnected())
	{
		return INVALID_SOCKET;
	}

	if (INVALID_SOCKET != GetSock())
	{
		return INVALID_SOCKET;
	}

#ifdef WIN32
	SetSock(WSASocket(
		AF_INET,
		SOCK_STREAM,
		0,
		NULL,
		0,
		WSA_FLAG_OVERLAPPED));
#else
	SetSock(socket(AF_INET, SOCK_STREAM, 0));
#endif // WIN32

	if (INVALID_SOCKET == GetSock())
	{
		PushNetEvent(NETEVT_ERROR, errno);
		Close();
		return INVALID_SOCKET;
	}

#ifdef WIN32
	// keep alive
	struct tcp_keepalive keepAliveIn;
	struct tcp_keepalive keepAliveOut;

	unsigned long ulBytesReturn = 0;

	keepAliveIn.keepaliveinterval = 10000;//
	keepAliveIn.keepalivetime = 1000 * 30;//
	keepAliveIn.onoff = 1;

	int ret = WSAIoctl
		(
		GetSock(),
		SIO_KEEPALIVE_VALS,
		&keepAliveIn,
		sizeof(keepAliveIn),
		&keepAliveOut,
		sizeof(keepAliveOut),
		&ulBytesReturn,
		NULL,
		NULL
		);
	if (ret == SOCKET_ERROR)
	{
		PushNetEvent(NETEVT_ERROR, WSAGetLastError());
		Close();
		return INVALID_SOCKET;
	}

	sockaddr_in local_addr;
	ZeroMemory(&local_addr, sizeof (sockaddr_in));
	local_addr.sin_family = AF_INET;

	int nSendBuffSize = 256 * 1024;
	int nRecvBuffSize = 8 * nSendBuffSize;
	if ((0 != setsockopt(GetSock(), SOL_SOCKET, SO_RCVBUF, (char*)&nRecvBuffSize, sizeof(int))) ||
		(0 != setsockopt(GetSock(), SOL_SOCKET, SO_SNDBUF, (char*)&nSendBuffSize, sizeof(int))))
	{
		int nError = WSAGetLastError();
		PushNetEvent(NETEVT_CONN_ERR, nError);
		closesocket(GetSock());
		return INVALID_SOCKET;
	}

	//int irt = ::bind(GetSock(), (sockaddr *)(&local_addr), sizeof (sockaddr_in));
#else
	setsockopt(GetSock(), SOL_SOCKET, SO_SNDLOWAT, &VAL_SO_SNDLOWAT, sizeof(VAL_SO_SNDLOWAT));
	setsockopt(GetSock(), SOL_SOCKET, SO_SNDBUF, &MAX_SYS_SEND_BUF, sizeof(MAX_SYS_SEND_BUF));

	int keepAlive = 1;
	setsockopt(GetSock(), SOL_SOCKET, SO_KEEPALIVE, (void*)&keepAlive, sizeof(keepAlive));

	int keepIdle = 30;
	int keepInterval = 5;
	int keepCount = 6;
	setsockopt(GetSock(), SOL_TCP, TCP_KEEPIDLE, (void *)&keepIdle, sizeof(keepIdle));
	setsockopt(GetSock(), SOL_TCP, TCP_KEEPINTVL, (void *)&keepInterval, sizeof(keepInterval));
	setsockopt(GetSock(), SOL_TCP, TCP_KEEPCNT, (void *)&keepCount, sizeof(keepCount));
#endif // WIN32
	// [end]

	SetIoThread(FxNetModule::Instance()->FetchIoThread(GetSockId()));
	if (NULL == m_poIoThreadHandler)
	{
//		m_pLock.UnLock();
		PushNetEvent(NETEVT_ERROR, 0);
		Close();
		return INVALID_SOCKET;
	}

	sockaddr_in stAddr = { 0 };
	stAddr.sin_family = AF_INET;
	stAddr.sin_addr.s_addr = GetConnection()->GetRemoteIP();
	stAddr.sin_port = htons(GetConnection()->GetRemotePort());

	SetState(SSTATE_CONNECT);

#ifdef WIN32
	// todo connectEX暂时不能用 所以暂时不加到完成端口中//
#else
	if (!AddEvent())
	{
//		m_pLock.UnLock();
		PushNetEvent(NETEVT_ERROR, errno);
		Close();
		return INVALID_SOCKET;
	}
#endif // WIN32

	//请求连接时 Windows跟linux是有区别的//
#ifdef WIN32
	//LPFN_CONNECTEX m_lpfnConnectEx = NULL ;
	//DWORD dwBytes = 0;
	//GUID GuidConnectEx = WSAID_CONNECTEX;

	//if (SOCKET_ERROR == WSAIoctl(GetSock(), SIO_GET_EXTENSION_FUNCTION_POINTER,
	//	&GuidConnectEx, sizeof (GuidConnectEx),
	//	&m_lpfnConnectEx, sizeof (m_lpfnConnectEx), &dwBytes, 0, 0))
	//{
	//	PushNetEvent(NETEVT_CONN_ERR, (UINT32)WSAGetLastError());
	//	closesocket(GetSock());
	//	return INVALID_SOCKET;
	//}

	//// 这个时候 还没有连上 所以 将这个改成IOCP_CONNECT 等连完以后再改回来
	//m_stRecvIoData.nOp = IOCP_CONNECT;

	//int bResult = m_lpfnConnectEx(GetSock(),
	//	(sockaddr *)&stAddr,  // [in] 对方地址
	//	sizeof(stAddr),               // [in] 对方地址长度
	//	NULL,       // [in] 连接后要发送的内容，这里不用
	//	0,   // [in] 发送内容的字节数 ，这里不用
	//	NULL,       // [out] 发送了多少个字节，这里不用
	//	&m_stRecvIoData.stOverlapped); // [in]
	//if (!bResult)      // 返回值处//

	if(-1 == connect(GetSock(), (sockaddr*)&stAddr, sizeof(stAddr)))
	{
		// todo ֱ������ס������
		PushNetEvent(NETEVT_CONN_ERR, (UINT32)WSAGetLastError());
		closesocket(GetSock());
		return INVALID_SOCKET;
	}
	else
	{
		m_stRecvIoData.nOp = IOCP_RECV;
		unsigned long ul = 1;
		if (SOCKET_ERROR == ioctlsocket(GetSock(), FIONBIO, (unsigned long*)&ul))
		{
			PushNetEvent(NETEVT_CONN_ERR, (UINT32)WSAGetLastError());
			closesocket(GetSock());
			LogFun(LT_Screen | LT_File, LogLv_Error, "Set socket FIONBIO error : %d, socket : %d, socket id %d", WSAGetLastError(), GetSock(), GetSockId());
			return INVALID_SOCKET;
		}
		if (!AddEvent())
		{
			PushNetEvent(NETEVT_CONN_ERR, (UINT32)WSAGetLastError());
			closesocket(GetSock());
			return INVALID_SOCKET;
		}
		OnConnect();
	}
#else
	if (-1 == connect(GetSock(), (sockaddr*)&stAddr, sizeof(stAddr)))
	{
		if (errno != EINPROGRESS && errno!= EINTR && errno != EAGAIN)
		{
//			m_pLock.UnLock();
			PushNetEvent(NETEVT_CONN_ERR, errno);
			LogFun(LT_Screen | LT_File, LogLv_Error, "connect error id : %d, socket : %d, socket id : %d", errno, GetSock(), GetSockId());
			Close();
			return INVALID_SOCKET;
		}
	}
#endif // WIN32

	return GetSock();
}

void FxTCPConnectSock::ProcEvent(SNetEvent oEvent)
{
	{
		switch (oEvent.eType)
		{
			case NETEVT_ESTABLISH:
			{
				__ProcEstablish();
			}
			break;

			case NETEVT_ASSOCIATE:
			{
				__ProcAssociate();
			}
			break;
		case NETEVT_CONN_ERR:
		{
			__ProcConnectError(oEvent.dwValue);
		}
			break;

		case NETEVT_ERROR:
		{
			__ProcError(oEvent.dwValue);
		}
			break;

			case NETEVT_TERMINATE:
			{
				__ProcTerminate();
			}
			break;

		case NETEVT_RECV:
		{
			__ProcRecv(oEvent.dwValue);
		}
			break;

		case NETEVT_RELEASE:
		{
			__ProcRelease();
		}
			break;

			default:
			{
				Assert(0);
			}
			break;
		}
	}
}

void FxTCPConnectSock::OnConnect()
{
#ifdef WIN32
	sockaddr_in stAddr = {0};
	INT32 nAddrLen = sizeof(stAddr);
	getsockname(GetSock(), (sockaddr*)&stAddr, &nAddrLen);

	GetConnection()->SetLocalIP(stAddr.sin_addr.s_addr);
	GetConnection()->SetLocalPort(ntohs(stAddr.sin_port));

	GetConnection()->SetID(GetSockId());

	SetState(SSTATE_ESTABLISH);
	PushNetEvent(NETEVT_ESTABLISH, 0);

	if (false == PostRecv())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == PostRecv(), socket : %d, socket id : %d", GetSock(), GetSockId());

		PushNetEvent(NETEVT_ERROR, WSAGetLastError());
		Close();
	}
#else
	SetState(SSTATE_ESTABLISH);
	PushNetEvent(NETEVT_ESTABLISH, 0);

	INT32 nError = 0;
	socklen_t nLen = sizeof(nError);
	if (getsockopt(GetSock(), SOL_SOCKET, SO_ERROR, &nError, &nLen) < 0)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "getsockopt errno : %d, socket : %d, socket id : %d", errno, GetSock(), GetSockId());

		PushNetEvent(NETEVT_ERROR, errno);
		Close();
		return;
	}

	if (nError != 0)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "getsockopt errno : %d, socket : %d, socket id : %d", nError, GetSock(), GetSockId());

		PushNetEvent(NETEVT_ERROR, nError);
		Close();
		return;
	}

	sockaddr_in stAddr = { 0 };
	UINT32 dwAddrLen = sizeof(stAddr);
	getsockname(GetSock(), (sockaddr*)&stAddr, &dwAddrLen);

	GetConnection()->SetLocalIP(stAddr.sin_addr.s_addr);
	GetConnection()->SetLocalPort(ntohs(stAddr.sin_port));

	GetConnection()->SetID(GetSockId());

	if (!m_poIoThreadHandler->ChangeEvent(GetSock(), EPOLLIN, this))
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "m_poIoThreadHandler->ChangeEvent, socket : %d, socket id : %d", GetSock(), GetSockId());

		PushNetEvent(NETEVT_ERROR, errno);
		Close();
	}
#endif // WIN32

}

bool FxTCPConnectSock::PostClose()
{
#ifdef WIN32
	if (false == IsConnected())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == IsConnected(), socket : %d, socket id : %d", GetSock(), GetSockId());

		return false;
	}

	ZeroMemory(&m_stRecvIoData.stOverlapped, sizeof(m_stRecvIoData.stOverlapped));
	// Post失败的时候再进入这个函数时可能会丢失一次//

	if (!PostQueuedCompletionStatus(m_poIoThreadHandler->GetHandle(), UINT32(0), (ULONG_PTR)this, &m_stRecvIoData.stOverlapped))
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "PostQueuedCompletionStatus errno : %d, socket : %d, socket id : %d", WSAGetLastError(), GetSock(), GetSockId());

		return false;
	}

	return true;
#else
	return false;
#endif // WIN32
}

#ifdef WIN32
void FxTCPConnectSock::OnParserIoEvent(bool bRet, void* pIoData, UINT32 dwByteTransferred)
{
	SPerIoData* pSPerIoData = (SPerIoData*)pIoData;
	if (NULL == pSPerIoData)
	{
		Close();
		return;
	}

	switch (pSPerIoData->nOp)
	{
	case IOCP_RECV:
	{
		OnRecv(bRet, dwByteTransferred);
	}
		break;
	case IOCP_SEND:
	{
		OnSend(bRet, dwByteTransferred);
	}
		break;
	case IOCP_CONNECT:
	{
		if (GetState() != SSTATE_CONNECT)
		{
			Assert(0);
		}
		OnConnect();
	}
		break;
	default:
	{
		Close();
	}
		break;
	}
}

void FxTCPConnectSock::OnRecv(bool bRet, int dwBytes)
{
	if (0 == dwBytes)
	{
		InterlockedCompareExchange(&m_nPostRecv, 0, m_nPostRecv);
		Close();
		return;
	}

	if (UINT32(-1) == dwBytes)
	{
		InterlockedCompareExchange(&m_nPostRecv, m_nPostRecv - 1, m_nPostRecv);
		if (0 == m_nPostRecv)
		{
			if (false == PostRecv())
			{
				PushNetEvent(NETEVT_ERROR, WSAGetLastError());
				Close();
			}
		}
		return;
	}

	if (false == bRet)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == bRet errno : %d, socket : %d, socket id : %d", WSAGetLastError(), GetSock(), GetSockId());

		InterlockedCompareExchange(&m_nPostRecv, 0, m_nPostRecv);
		m_dwLastError = WSAGetLastError();
		PostClose();
		return;
	}

	if (!IsConnected())
	{
		InterlockedCompareExchange(&m_nPostRecv, 0, m_nPostRecv);
		return;
	}

	m_oLock.Lock();
	int nUsedLen = 0;
	int nParserLen = 0;
	int nLen = int(dwBytes);
	if (m_poRecvBuf->CostBuff(nLen))
	{
		//LogFun(LT_Screen | LT_File, LogLv_Error, "m_poRecvBuf->CostBuff error");
		//
		//InterlockedCompareExchange(&m_nPostRecv, 0, m_nPostRecv);
		//m_dwLastError = NET_RECVBUFF_ERROR;
		//PostClose();
		//return;
	}

	char *pUseBuf = NULL;
	nLen = m_poRecvBuf->GetUsedCursorPtr(pUseBuf);
	if (nLen <= 0)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "m_poRecvBuf->GetUsedCursorPtr <= 0, socket : %d, socket id : %d", GetSock(), GetSockId());

		InterlockedCompareExchange(&m_nPostRecv, 0, m_nPostRecv);
		m_dwLastError = NET_RECVBUFF_ERROR;
		PostClose();
		m_oLock.UnLock();
		return;
	}

	while (0 < nLen)
	{
		if (0 != m_nNeedData)
		{
			if (nLen >= m_nNeedData)
			{
				nLen -= m_nNeedData;
				nUsedLen += m_nNeedData;
				nParserLen += m_nNeedData;
				m_poRecvBuf->CostUsedBuff(nUsedLen);
				nUsedLen = 0;
				PushNetEvent(NETEVT_RECV, m_nPacketLen);
				m_nPacketLen = 0;
				m_nNeedData = 0;
			}
			else
			{
				m_nNeedData -= nLen;
				nUsedLen += nLen;
				nLen = 0;
			}
		}
		else
		{
			char* pParseBuf = pUseBuf + nParserLen;
			UINT32 dwHeaderLen = GetDataHeader()->GetHeaderLength();
			GetDataHeader()->BuildRecvPkgHeader(pParseBuf, (int)dwHeaderLen> nLen ? nLen : dwHeaderLen, 0);
			m_nPacketLen = GetDataHeader()->ParsePacket(pParseBuf, nLen);
			if (-1 == m_nPacketLen)
			{
				LogFun(LT_Screen | LT_File, LogLv_Error, "header error, socket : %d, socket id : %d", GetSock(), GetSockId());

				InterlockedCompareExchange(&m_nPostRecv, 0, m_nPostRecv);
				m_dwLastError = NET_RECVBUFF_ERROR;
				PostClose();
				m_oLock.UnLock();
				return;
			}
			else if (0 == m_nPacketLen)
			{
				m_poRecvBuf->CostUsedBuff(nUsedLen);
				nUsedLen = 0;
				m_nNeedData = 0;

				if ((int)(GetDataHeader()->GetHeaderLength()) > nLen)
				{
					// 判断是否在循环buff的头部还有数据//
					int nHasData = m_poRecvBuf->GetUseLen();
					if ((int)(GetDataHeader()->GetHeaderLength()) <= nHasData)
					{
						GetDataHeader()->BuildRecvPkgHeader(pParseBuf, nLen, 0);
						if (false == m_poRecvBuf->CostUsedBuff(nLen))
						{
							break;
						}
						int nNewLen = m_poRecvBuf->GetUsedCursorPtr(pUseBuf);
						if ((int)(GetDataHeader()->GetHeaderLength()) - nLen > nNewLen)
						{
							LogFun(LT_Screen | LT_File, LogLv_Error, "header error, socket : %d, socket id : %d", GetSock(), GetSockId());

							InterlockedCompareExchange(&m_nPostRecv, 0, m_nPostRecv);
							m_dwLastError = NET_RECVBUFF_ERROR;
							PostClose();
							m_oLock.UnLock();
							return;
						}
						GetDataHeader()->BuildRecvPkgHeader(pUseBuf, GetDataHeader()->GetHeaderLength() - nLen, nLen);
						pParseBuf = (char*)(GetDataHeader()->GetPkgHeader());
						m_nPacketLen = GetDataHeader()->ParsePacket(pParseBuf, GetDataHeader()->GetHeaderLength());
						if (0 >= m_nPacketLen)
						{
							LogFun(LT_Screen | LT_File, LogLv_Error, "header error, socket : %d, socket id : %d", GetSock(), GetSockId());

							InterlockedCompareExchange(&m_nPostRecv, 0, m_nPostRecv);
							m_dwLastError = NET_RECVBUFF_ERROR;
							PostClose();
							m_oLock.UnLock();
							return;
						}

						m_nNeedData = m_nPacketLen - GetDataHeader()->GetHeaderLength();
						nUsedLen = GetDataHeader()->GetHeaderLength() - nLen;
						nParserLen = nUsedLen;
						nLen = nNewLen - nUsedLen;
						pParseBuf = pUseBuf + nUsedLen;
						if (0 == m_nNeedData)
						{
							m_poRecvBuf->CostUsedBuff(nUsedLen);
							nUsedLen = 0;
							PushNetEvent(NETEVT_RECV, m_nPacketLen);
							m_nPacketLen = 0;
						}
					}
					else
					{
						nLen = 0;
						break;
					}
				}
			}
			else
			{
				if (nLen >= m_nPacketLen)
				{
					nLen -= m_nPacketLen;
					nUsedLen += m_nPacketLen;
					nParserLen += nUsedLen;
					m_poRecvBuf->CostUsedBuff(nUsedLen);
					nUsedLen = 0;
					PushNetEvent(NETEVT_RECV, m_nPacketLen);
					m_nPacketLen = 0;
					m_nNeedData = 0;
				}
				else
				{
					m_nNeedData = m_nPacketLen - nLen;
					nUsedLen += nLen;
					nLen = 0;
				}
			}
		}
	}

	if (0 != nUsedLen)
	{
		m_poRecvBuf->CostUsedBuff(nUsedLen);
	}

	InterlockedCompareExchange(&m_nPostRecv, m_nPostRecv - 1, m_nPostRecv);
	if (0 == m_nPostRecv)
	{
		if (false == PostRecv())
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "false == PostRecv, socket : %d, socket id : %d", GetSock(), GetSockId());

			m_dwLastError = WSAGetLastError();
			PostClose();
		}
	}
	m_oLock.UnLock();
}

void FxTCPConnectSock::OnSend(bool bRet, int dwBytes)
{
	if (false == bRet)
	{
		InterlockedCompareExchange(&m_nPostSend, 0, m_nPostSend);
		m_dwLastError = WSAGetLastError();
		PostClose();
		return;
	}

	if (false == IsConnected())
	{
		InterlockedCompareExchange(&m_nPostSend, 0, m_nPostSend);
		return;
	}

	if (SSTATE_ESTABLISH != GetState())
	{
		InterlockedCompareExchange(&m_nPostSend, 0, m_nPostSend);
		return;
	}

	if (UINT32(-1) == dwBytes)
	{
		InterlockedCompareExchange(&m_nPostSend, m_nPostSend - 1, m_nPostSend);
		if (0 == m_nPostSend)
		{
			if (false == PostSend())
			{
				LogFun(LT_Screen | LT_File, LogLv_Error, "false == PostSend, socket : %d, socket id : %d", GetSock(), GetSockId());

				m_dwLastError = WSAGetLastError();
				PostClose();
			}
		}
		return;
	}

	if (0 == dwBytes)
	{
		InterlockedCompareExchange(&m_nPostSend, 0, m_nPostSend);
		m_dwLastError = NET_SEND_ERROR;
		PostClose();
		return;
	}

	// 把成功发送了的从发送缓冲区丢弃//
	m_poSendBuf->DiscardBuff(dwBytes);

	InterlockedCompareExchange(&m_nPostSend, m_nPostSend - 1, m_nPostSend);
	if (0 == m_nPostSend)
	{
		if (false == PostSend())
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "false == PostSend, socket : %d, socket id : %d", GetSock(), GetSockId());

			m_dwLastError = WSAGetLastError();
			PostClose();
		}
	}
}

bool FxTCPConnectSock::PostRecv()
{
	if (false == IsConnected())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == IsConnected(), socket : %d, socket id : %d", GetSock(), GetSockId());

		return false;
	}

	LONG nPostRecv = InterlockedCompareExchange(&m_nPostRecv, 1, 0);
	if (0 != nPostRecv)
	{
		return true;
	}

	if (m_poRecvBuf->IsEmpty())
	{
		m_poRecvBuf->Clear();
	}

	ZeroMemory(&m_stRecvIoData.stOverlapped, sizeof(m_stRecvIoData.stOverlapped));
	int nLen = m_poRecvBuf->GetInCursorPtr(m_stRecvIoData.stWsaBuf.buf);
	if (0 >= nLen)
	{
		//LogFun(LT_Screen | LT_File, LogLv_Error, "m_poRecvBuf->GetInCursorPtr() = %d, socket : %d, socket id : %d", nLen, GetSock(), GetSockId());

		// 接受缓存不够 等会继续接收 //
		InterlockedCompareExchange(&m_nPostRecv, 0, 1);
		PushNetEvent(NETEVT_RECV, -1);
		return true;
	}

	nLen = 65536 < nLen ? 65536 : nLen;
	m_stRecvIoData.stWsaBuf.len = nLen;

	DWORD dwReadLen = 0;
	DWORD dwFlags = 0;

	if (0 != WSARecv(GetSock(), &m_stRecvIoData.stWsaBuf, 1, &dwReadLen, &dwFlags, &m_stRecvIoData.stOverlapped, NULL))
	{
		if (WSAGetLastError() != WSA_IO_PENDING)
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "WSARecv errno : %d, socket : %d, socket id : %d", WSAGetLastError(), GetSock(), GetSockId());

			InterlockedCompareExchange(&m_nPostRecv, 0, 1);
			return false;
		}
	}

	return true;
}

bool FxTCPConnectSock::PostRecvFree()
{
	if (false == IsConnected())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == IsConnected(), socket : %d, socket id : %d", GetSock(), GetSockId());
		return false;
	}
	LONG nPostRecv = InterlockedCompareExchange(&m_nPostRecv, 1, 0);
	if (0 != nPostRecv)
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "m_nPostRecv == 1, socket : %d, socket id : %d", GetSock(), GetSockId());
		return false;
	}
	ZeroMemory(&m_stRecvIoData.stOverlapped, sizeof(m_stRecvIoData.stOverlapped));
	//post 失败的时候 再进入这个函数 可能会丢失一次
	if (!PostQueuedCompletionStatus(m_poIoThreadHandler->GetHandle(), UINT32(-1), (ULONG_PTR)this, &m_stRecvIoData.stOverlapped))
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "PostQueuedCompletionStatus error : %d, socket : %d, socket id : %d", WSAGetLastError(), GetSock(), GetSockId());
		InterlockedCompareExchange(&m_nPostRecv, 0, 1);
		return false;
	}

	return true;
}

#else
void FxTCPConnectSock::OnParserIoEvent(int dwEvents)
{
	if(GetState() == SSTATE_CONNECT)
	{
		if(dwEvents & EPOLLOUT)
		{
			OnConnect();
		}
		else
		{
			PushNetEvent(NETEVT_ERROR, errno);
			Close();
		}
		return;
	}
	if (!IsConnected())
	{
		PushNetEvent(NETEVT_ERROR, errno);
		Close();
		return;
	}

	if (dwEvents & EPOLLOUT)
	{
		OnSend();
	}

	if (dwEvents & EPOLLIN)
	{
		OnRecv();
	}

	if (dwEvents & EPOLLERR)
	{
		if(errno == EINPROGRESS || errno == EINTR || errno == EAGAIN)
		{
			return;
		}
		PushNetEvent(NETEVT_ERROR, errno);
		Close();
	}
}

void FxTCPConnectSock::OnRecv()
{
	if (false == IsConnected())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == IsConnected(), socket : %d, socket id : %d", GetSock(), GetSockId());

		return;
	}

	if (m_poRecvBuf->IsEmpty())
	{
		m_poRecvBuf->Clear();
	}

	char* pRecvBuff = NULL;
	int nLen = m_poRecvBuf->GetInCursorPtr(pRecvBuff);
	if (0 >= nLen)
	{
		//LogFun(LT_Screen | LT_File, LogLv_Error, "m_poRecvBuf->GetInCursorPtr() = %d, socket : %d, socket id : %d", nLen, GetSock(), GetSockId());
		return;
	}

	nLen = VAL_SO_SNDLOWAT < nLen ? VAL_SO_SNDLOWAT : nLen;

	nLen = recv(GetSock(), pRecvBuff, nLen, 0);

	if(0 > nLen)
	{
		if ((errno != EAGAIN) && (errno != EINPROGRESS) && (errno != EINTR))
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "recv errno : %d, socket : %d, socket id : %d", errno, GetSock(), GetSockId());

			PushNetEvent(NETEVT_ERROR, errno);
			Close();
			return;
		}
		return;
	}
	else if (0 == nLen)
	{
		Close();
		return;
	}
	else
	{
		int nUsedLen = 0;
		int nParserLen = 0;
		m_poRecvBuf->CostBuff(nLen);

		char *pUseBuf = NULL;
		nLen = m_poRecvBuf->GetUsedCursorPtr(pUseBuf);
		if (nLen <= 0)
		{
			LogFun(LT_Screen | LT_File, LogLv_Error, "m_poRecvBuf->GetUsedCursorPtr() <= 0, socket : %d, socket id : %d", GetSock(), GetSockId());

			PushNetEvent(NETEVT_ERROR, NET_RECVBUFF_ERROR);
			Close();
			return;
		}


		while (0 < nLen)
		{
			if (0 != m_nNeedData)
			{
				if (nLen >= m_nNeedData)
				{
					nLen -= m_nNeedData;
					nUsedLen += m_nNeedData;
					nParserLen += m_nNeedData;
					m_poRecvBuf->CostUsedBuff(nUsedLen);
					nUsedLen = 0;
					PushNetEvent(NETEVT_RECV, m_nPacketLen);
					m_nPacketLen = 0;
					m_nNeedData = 0;
				}
				else
				{
					m_nNeedData -= nLen;
					nUsedLen += nLen;
					nLen = 0;
				}
			}
			else
			{
				char* pParseBuf = pUseBuf + nParserLen;
				UINT32 dwHeaderLen = GetDataHeader()->GetHeaderLength();
				GetDataHeader()->BuildRecvPkgHeader(pParseBuf, (int)dwHeaderLen> nLen ? nLen : dwHeaderLen, 0);
				m_nPacketLen = GetDataHeader()->ParsePacket(pParseBuf, nLen);
				if (-1 == m_nPacketLen)
				{
					LogFun(LT_Screen | LT_File, LogLv_Error, "header error, socket : %d, socket id : %d", GetSock(), GetSockId());

					PushNetEvent(NETEVT_ERROR, NET_RECV_ERROR);
					Close();
					return;
				}
				else if (0 == m_nPacketLen)
				{
					m_poRecvBuf->CostUsedBuff(nUsedLen);
					nUsedLen = 0;
					m_nNeedData = 0;

					if ((int)(GetDataHeader()->GetHeaderLength()) > nLen)
					{
							// 判断是否在循环buff的头部还有数据//
						int nHasData = m_poRecvBuf->GetUseLen();
						if ((int)(GetDataHeader()->GetHeaderLength()) <= nHasData)
						{
							GetDataHeader()->BuildRecvPkgHeader(pParseBuf, nLen, 0);
							if (false == m_poRecvBuf->CostUsedBuff(nLen))
							{
								break;
							}
							int nNewLen = m_poRecvBuf->GetUsedCursorPtr(pUseBuf);
							if ((int)(GetDataHeader()->GetHeaderLength()) - nLen > nNewLen)
							{
								LogFun(LT_Screen | LT_File, LogLv_Error, "header error, socket : %d, socket id : %d", GetSock(), GetSockId());

								PushNetEvent(NETEVT_ERROR, NET_RECVBUFF_ERROR);
								Close();
								return;
							}
							GetDataHeader()->BuildRecvPkgHeader(pUseBuf, GetDataHeader()->GetHeaderLength() - nLen, nLen);
							pParseBuf = (char*)(GetDataHeader()->GetPkgHeader());
							m_nPacketLen = GetDataHeader()->ParsePacket(pParseBuf, GetDataHeader()->GetHeaderLength());
							if (0 >= m_nPacketLen)
							{
								LogFun(LT_Screen | LT_File, LogLv_Error, "header error, socket : %d, socket id : %d", GetSock(), GetSockId());

								PushNetEvent(NETEVT_ERROR, NET_RECVBUFF_ERROR);
								Close();
								return;
							}

							m_nNeedData = m_nPacketLen - GetDataHeader()->GetHeaderLength();
							nUsedLen = GetDataHeader()->GetHeaderLength() - nLen;
							nParserLen = nUsedLen;
							nLen = nNewLen - nUsedLen;
							pParseBuf = pUseBuf + nUsedLen;
							if (0 == m_nNeedData)
							{
								m_poRecvBuf->CostUsedBuff(nUsedLen);
								nUsedLen = 0;
								PushNetEvent(NETEVT_RECV, m_nPacketLen);
								m_nPacketLen = 0;
							}
						}
						else
						{
							nLen = 0;
							break;
						}
					}
				}
				else
				{
					if (nLen >= m_nPacketLen)
					{
						nLen -= m_nPacketLen;
						nUsedLen += m_nPacketLen;
						nParserLen += nUsedLen;
						m_poRecvBuf->CostUsedBuff(nUsedLen);
						nUsedLen = 0;
						PushNetEvent(NETEVT_RECV, m_nPacketLen);
						m_nPacketLen = 0;
						m_nNeedData = 0;
					}
					else
					{
						m_nNeedData = m_nPacketLen - nLen;
						nUsedLen += nLen;
						nLen = 0;
					}
				}
			}
		}

		if (0 != nUsedLen)
		{
			m_poRecvBuf->CostUsedBuff(nUsedLen);
		}
	}
}

void FxTCPConnectSock::OnSend()
{
	if (!IsConnected())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == IsConnected(), socket : %d, socket id : %d", GetSock(), GetSockId());

		return;
	}

	m_bSending = false;
	if (!PostSend())
	{
		LogFun(LT_Screen | LT_File, LogLv_Error, "false == PostSend(), socket : %d, socket id : %d", GetSock(), GetSockId());

		PushNetEvent(NETEVT_ERROR, errno);
		Close();
		return;
	}
}


#endif // WIN32


