#include "SocketSession.h"
#include "lua_engine.h"
#include "fxtimer.h"
#include "fxdb.h"
#include "fxmeta.h"

#include <signal.h>

bool g_bRun = true;

void EndFun(int n)
{
	if (n == SIGINT || n == SIGTERM)
	{
		g_bRun = false;
	}
	else
	{
		printf("unknown signal : %d !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n", n);
	}
}

int main()
{
	//----------------------order can't change begin-----------------------//
	signal(SIGINT, EndFun);
	signal(SIGTERM, EndFun);
	if (!LogThread::CreateInstance())
	{
		return 0;
	}

	if (!CLuaEngine::CreateInstance())
	{
		return 0;
	}
	if (!CLuaEngine::Instance()->Reload())
	{
		return 0;
	}

	if (!GetTimeHandler()->Init())
	{
		return 0;
	}
	GetTimeHandler()->Run();
	if (!LogThread::Instance()->Init())
	{
		return 0;
	}

	if (!CSessionFactory::CreateInstance())
	{
		return 0;
	}
	CSessionFactory::Instance()->Init();
	IFxNet* pNet = FxNetGetModule();
	if (!pNet)
	{
		return 0;
	}
	//----------------------order can't change end-----------------------//

	//SDBConnInfo oInfo;
	//memset(&oInfo, 0, sizeof(oInfo));
	//oInfo.m_dwDBId = 0;
	//strcpy(oInfo.m_stAccount.m_szCharactSet, "utf8");
	//strcpy(oInfo.m_stAccount.m_szDBName, "jianghu");
	//strcpy(oInfo.m_stAccount.m_szHostName, "192.168.5.6");
	//strcpy(oInfo.m_stAccount.m_szLoginName, "root");
	//strcpy(oInfo.m_stAccount.m_szLoginPwd, "test");
	//oInfo.m_stAccount.m_wConnPort = 3306;
	//if (FxDBGetModule()->Open(oInfo))
	//{
	//	LogFun(LT_Screen, LogLv_Info, "%s", "db connected~~~~");
	//}

	IFxListenSocket* pListenSocket = pNet->Listen(CSessionFactory::Instance(), 0, 0, 12000);

	while (g_bRun)
	{
		GetTimeHandler()->Run();
		pNet->Run(0xffffffff);
		//LogFun(LT_Screen, LogLv_Info, "%s", PrintTrace());
		FxSleep(1);
	}
	pListenSocket->StopListen();
	pListenSocket->Close();
	for (std::set<FxSession*>::iterator it = CSessionFactory::Instance()->m_setSessions.begin();
	it != CSessionFactory::Instance()->m_setSessions.end(); ++it)
	{
		(*it)->Close();
	}

	while (CSessionFactory::Instance()->m_setSessions.size())
	{
		FxSleep(10);
	}
	pNet->Run(0xffffffff);
	FxSleep(10);
	pNet->Release();
	LogThread::Instance()->Stop();
}
