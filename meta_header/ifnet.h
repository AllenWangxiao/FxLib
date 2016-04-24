#ifndef __IFNET_H__
#define __IFNET_H__


#ifdef WIN32
#include <WinSock2.h>

#ifdef	FXN_DLLCLASS
#define	DLLCLASS_DECL			__declspec(dllimport)	
#else
#define	DLLCLASS_DECL			__declspec(dllexport)	
#endif

#ifdef FXN_STATICLIB
#undef	DLLCLASS_DECL
#define DLLCLASS_DECL
#endif

#else
#include <arpa/inet.h>
#define SOCKET UINT32
#define DLLCLASS_DECL
#define INVALID_SOCKET UINT32(-1)
#endif

#include <assert.h>
#include "fxmeta.h"
#include "ifsocket.h"

class FxConnection;

class IFxDataHeader;

#define LINUX_NETTHREAD_COUNT	2	// linux????????????,Windows??????cpu?????2??
#define MAX_CONNECTION_COUNT	64
#define MAX_NETEVENT_PERSOCK	1024

#define RECV_BUFF_SIZE 64*1024
#define SEND_BUFF_SIZE 64*1024

enum ENetErrCode{
	NET_RECVBUFF_ERROR	= -7,
	NET_CONNECT_FAIL	= -6,
	NET_SYSTEM_ERROR	= -5, 
	NET_RECV_ERROR		= -4, 
	NET_SEND_ERROR		= -3, 
	NET_SEND_OVERFLOW	= -2,
	NET_PACKET_ERROR	= -1,
	NET_SUCCESS			= 0
};

enum ESessionOpt
{
	ESESSION_SENDLINGER = 1,	// ????????????????????30?????????
};

enum ENetOpt
{
	ENET_MAX_CONNECTION = 1,	// ?????????
	ENET_MAX_TOTALEVENT,		// ???Socket????????????
};

class DLLCLASS_DECL FxSession
{
public:
	FxSession();
	virtual ~FxSession();

	virtual void		OnConnect(void) = 0;

	virtual void		OnClose(void) = 0;

	virtual void		OnError(UINT32 dwErrorNo) = 0;

	virtual void		OnRecv(const char* pBuf, UINT32 dwLen) = 0;

	virtual void		Release(void) = 0;

	virtual char*		GetRecvBuf() = 0;

	virtual UINT32		GetRecvSize() = 0;

	virtual const char* GetRemoteIPStr();

	virtual UINT32		GetRemotePort();

	virtual bool		Send(const char* pBuf,UINT32 dwLen);

	virtual void		Close(void);

	virtual SOCKET		Reconnect(void);

	virtual bool		IsConnected(void);
	virtual bool		IsConnecting(void);

	virtual void		Init(FxConnection* poConnection);

	virtual bool		SetSessionOpt(ESessionOpt eOpt, bool bSetting);

	virtual void		SetDataHeader(IFxDataHeader* pDataHeader);

	virtual IFxDataHeader*	GetDataHeader(){ return m_pDataHeader; }

	virtual bool		OnDestroy();

	virtual FxConnection*	GetConnection(void);
private:
	FxConnection*		m_poConnection;

	IFxDataHeader* m_pDataHeader;
};

class IFxSessionFactory
{
public:
	virtual ~IFxSessionFactory() {}
	virtual FxSession*	CreateSession() = 0;
};

struct SPerIoData;
class IFxSocket
{
public:
	virtual ~IFxSocket(){}

	virtual bool Init() = 0;
	virtual void OnRead() = 0;
	virtual void OnWrite() = 0;
	virtual bool Close() = 0;
	virtual void ProcEvent() = 0;
	
	void SetSock(SOCKET hSock){ m_hSock = hSock; }
	SOCKET& GetSock(){ return m_hSock; }
	void SetSockId(unsigned int dwSockId){ m_dwSockId = dwSockId; }
	unsigned int GetSockId(){ return m_dwSockId; }

#ifdef WIN32
	virtual void OnParserIoEvent(bool bRet, SPerIoData* pIoData, UINT32 dwByteTransferred) = 0;    // ????????????
#else
	virtual void OnParserIoEvent(int dwEvents) = 0;    // ????????????
#endif // WIN32


protected:
private:
	SOCKET m_hSock;

	unsigned int m_dwSockId;
};

class IFxDataHeader
{
public:
	virtual ~IFxDataHeader(){}

	virtual unsigned int GetHeaderLength() = 0;		// ?????

	inline int ParsePacket(const char* pBuf, UINT32 dwLen)
	{
		if (dwLen < GetHeaderLength())
		{
			return 0;
		}

		INT32 iPkgLen = __CheckPkgHeader(pBuf);

		return iPkgLen;
	}

	virtual void* GetPkgHeader() = 0;
	virtual void* BuildSendPkgHeader(UINT32 dwDataLen) = 0;
	virtual bool BuildRecvPkgHeader(char* pBuff, UINT32 dwLen, UINT32 dwOffset) = 0;
	virtual int __CheckPkgHeader(const char* pBuf) = 0;

};

class IFxDataHeaderFactory
{
public:
	virtual ~IFxDataHeaderFactory(){}

	virtual IFxDataHeader* CreateDataHeader() = 0;

private:

};

class IFxListenSocket : public IFxSocket
{
public:
	virtual ~IFxListenSocket(){}

	virtual bool Init() = 0;

	virtual bool Init(IFxSessionFactory* pSessionFactory){ m_poSessionFactory = pSessionFactory; return Init(); }

	virtual void OnRead() = 0;
	virtual void OnWrite() = 0;
	virtual bool Listen(UINT32 dwIP, UINT16 wPort) = 0;
	virtual bool StopListen() = 0;
	virtual bool Close() = 0;
	virtual void ProcEvent() = 0;

#ifdef WIN32
	virtual void OnParserIoEvent(bool bRet, SPerIoData* pIoData, UINT32 dwByteTransferred) = 0;		// ????????????
#else
	virtual void OnParserIoEvent(int dwEvents) = 0;		// ????????????
#endif // WIN32

	IFxSessionFactory* GetSessionFactory(){ return m_poSessionFactory; }

protected:
	IFxSessionFactory* m_poSessionFactory;
private:

};

class IFxConnectSocket : public IFxSocket
{
public:
	virtual ~IFxConnectSocket(){}

	virtual bool Init() = 0;
	//bool Init(IFxListenSocket* pListenSocket){ m_pListenSocket = pListenSocket;return Init(); }
	virtual void OnRead() = 0;
	virtual void OnWrite() = 0;
	virtual bool Close() = 0;
	virtual void ProcEvent() = 0;

	//IFxListenSocket* GetListenSocket(){ return m_pListenSocket; }

	virtual bool Send(const char* pData, int dwLen) = 0;

#ifdef WIN32
	virtual void OnParserIoEvent(bool bRet, SPerIoData* pIoData, UINT32 dwByteTransferred) = 0;		// ????????????
#else
	virtual void OnParserIoEvent(int dwEvents) = 0;		// ????????????
#endif // WIN32

protected:
	//IFxListenSocket* m_pListenSocket;	//????????????? ???????
private:

};

class IFxNet
{
public:
	virtual ~IFxNet() {}
	virtual bool		Init() = 0;
	virtual bool		Run(UINT32 dwCount) = 0;
	virtual void		Release() = 0;

	virtual SOCKET      Connect(FxSession* poSession, UINT32 dwIP, UINT16 wPort, bool bReconnect = false) = 0;
	virtual bool		Listen(IFxSessionFactory* pSessionFactory, UINT32 dwListenId, UINT32 dwIP, UINT16 dwPort) = 0;

	//public:
	//	virtual ~IFxNet() {}
	//	virtual bool		SetNetOpt(ENetOpt eOpt, int nValue) = 0;
	//	virtual bool		Init(int nThread = 1) = 0;
	//	virtual bool		Run(UINT32 dwCount) = 0;
	//	virtual void		Release() = 0;
	//
	//	virtual SOCKET		Connect(FxSession* poSession, UINT32 dwIP, UINT16 wPort, UINT32 dwRecvSize, UINT32 dwSendSize, bool bReconnect = false, int nEventPerSocket = MAX_NETEVENT_PERSOCK) = 0;
	//	virtual bool		Listen(IFxSessionFactory* poFactory, UINT32 dwIP, UINT16 wPort, UINT32 dwRecvSize, UINT32 dwSendSize, int nEventPerSocket = MAX_NETEVENT_PERSOCK) = 0;

private:

};

IFxNet* FxNetGetModule();

typedef IFxNet* (*PFN_FxNetGetModule)();


#endif	// __IFNET_H__
