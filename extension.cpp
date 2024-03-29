/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod Sample Extension
 * Copyright (C) 2004-2008 AlliedModders LLC.  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 *
 * Version: $Id$
 */

#include "extension.h"
#include "extensionHelper.h"
#include "CDetour/detours.h"
#include "steam/steam_gameserver.h"
#include "sm_namehashset.h"
#include <iclient.h>
#include <netadr.h>
#include <sstream>


/**
 * @file extension.cpp
 * @brief Implement extension code here.
 */

Connect g_Connect;		/**< Global singleton for extension's main interface */

SMEXT_LINK(&g_Connect);

ConVar *g_ConnectVersion = CreateConVar("connect_version", SMEXT_CONF_VERSION, FCVAR_REPLICATED|FCVAR_NOTIFY, SMEXT_CONF_DESCRIPTION " Version");
ConVar *g_SvNoSteam = CreateConVar("sv_nosteam", "1", FCVAR_NOTIFY, "Disable steam validation and force steam authentication.");
ConVar *g_SvForceSteam = CreateConVar("sv_forcesteam", "0", FCVAR_NOTIFY, "Force steam authentication.");
ConVar *g_SvLogging = CreateConVar("sv_connect_logging", "0", FCVAR_NOTIFY, "Log connection checks");
ConVar *g_SvAuthSessionResponseLegal = CreateConVar("sv_auth_session_response_legal", "0,1,2,3,4,5,7,9", FCVAR_NOTIFY, "List of EAuthSessionResponse that are considered as Steam legal (Defined in steam_api_interop.cs).");


IGameConfig *g_pGameConf = NULL;
IForward *g_pConnectForward = NULL;
IForward *g_pOnValidateAuthTicketResponse = NULL;
IGameEventManager2 *g_pGameEvents = NULL;

class CBaseClient;
class CBaseServer;

typedef enum EConnect
{
	k_OnClientPreConnectEx_Reject = 0,
	k_OnClientPreConnectEx_Accept = 1,
	k_OnClientPreConnectEx_Async = -1
} EConnect;

typedef enum EAuthProtocol
{
	k_EAuthProtocolWONCertificate = 1,
	k_EAuthProtocolHashedCDKey = 2,
	k_EAuthProtocolSteam = 3,
} EAuthProtocol;

const char *CSteamID::Render() const
{
	static char szSteamID[64];
	V_snprintf(szSteamID, sizeof(szSteamID), "STEAM_0:%u:%u", (m_steamid.m_comp.m_unAccountID % 2) ? 1 : 0, (int32)m_steamid.m_comp.m_unAccountID/2);
	return szSteamID;
}

class CSteam3Server
{
public:
	void *m_pSteamClient;
	ISteamGameServer *m_pSteamGameServer;
	void *m_pSteamGameServerUtils;
	void *m_pSteamGameServerNetworking;
	void *m_pSteamGameServerStats;
	void *m_pSteamHTTP;
	void *m_pSteamInventory;
	void *m_pSteamUGC;
	void *m_pSteamApps;
} *g_pSteam3Server;

CBaseServer *g_pBaseServer = NULL;

typedef CSteam3Server *(*Steam3ServerFunc)();

#ifndef WIN32
typedef void (*RejectConnectionFunc)(CBaseServer *, const netadr_t &address, int iClientChallenge, const char *pchReason);
#else
typedef void (__fastcall *RejectConnectionFunc)(CBaseServer *, void *, const netadr_t &address, int iClientChallenge, const char *pchReason);
#endif

#ifndef WIN32
typedef void (*SetSteamIDFunc)(CBaseClient *, const CSteamID &steamID);
#else
typedef void (__fastcall *SetSteamIDFunc)(CBaseClient *, void *, const CSteamID &steamID);
#endif

Steam3ServerFunc g_pSteam3ServerFunc = NULL;
RejectConnectionFunc g_pRejectConnectionFunc = NULL;
SetSteamIDFunc g_pSetSteamIDFunc = NULL;

CSteam3Server *Steam3Server()
{
	if(!g_pSteam3ServerFunc)
		return NULL;

	return g_pSteam3ServerFunc();
}

void RejectConnection(const netadr_t &address, int iClientChallenge, const char *pchReason)
{
	if(!g_pRejectConnectionFunc || !g_pBaseServer)
		return;

#ifndef WIN32
	g_pRejectConnectionFunc(g_pBaseServer, address, iClientChallenge, pchReason);
#else
	g_pRejectConnectionFunc(g_pBaseServer, NULL, address, iClientChallenge, pchReason);
#endif
}

void SetSteamID(CBaseClient *pClient, const CSteamID &steamID)
{
	if(!pClient || !g_pSetSteamIDFunc)
		return;

#ifndef WIN32
	g_pSetSteamIDFunc(pClient, steamID);
#else
	g_pSetSteamIDFunc(pClient, NULL, steamID);
#endif
}

EBeginAuthSessionResult BeginAuthSession(const void *pAuthTicket, int cbAuthTicket, CSteamID steamID)
{
	if(!g_pSteam3Server || !g_pSteam3Server->m_pSteamGameServer)
		return k_EBeginAuthSessionResultOK;

	return g_pSteam3Server->m_pSteamGameServer->BeginAuthSession(pAuthTicket, cbAuthTicket, steamID);
}

void EndAuthSession(CSteamID steamID)
{
	if(!g_pSteam3Server || !g_pSteam3Server->m_pSteamGameServer)
		return;

	g_pSteam3Server->m_pSteamGameServer->EndAuthSession(steamID);
}

bool BLoggedOn()
{
	if(!g_pSteam3Server || !g_pSteam3Server->m_pSteamGameServer)
		return false;

	return g_pSteam3Server->m_pSteamGameServer->BLoggedOn();
}

CDetour *g_Detour_CBaseServer__ConnectClient = NULL;
CDetour *g_Detour_CBaseServer__RejectConnection = NULL;
CDetour *g_Detour_CBaseServer__CheckChallengeType = NULL;
CDetour *g_Detour_CSteam3Server__OnValidateAuthTicketResponse = NULL;

class ConnectClientStorage
{
public:
	void* pThis;

	netadr_t address;
	int nProtocol;
	int iChallenge;
	int iClientChallenge;
	int nAuthProtocol;
	char pchName[256];
	char pchPassword[256];
	char pCookie[256];
	int cbCookie;
	IClient *pClient;

	uint64 ullSteamID;
	ValidateAuthTicketResponse_t ValidateAuthTicketResponse;
	bool GotValidateAuthTicketResponse;
	bool SteamLegal;
	bool SteamAuthFailed;
	EAuthSessionResponse eAuthSessionResponse;

	ConnectClientStorage()
	{
		this->GotValidateAuthTicketResponse = false;
		this->SteamLegal = false;
		this->SteamAuthFailed = false;
		this->eAuthSessionResponse = k_EAuthSessionResponseAuthTicketInvalid;
	}
	ConnectClientStorage(netadr_t address, int nProtocol, int iChallenge, int iClientChallenge, int nAuthProtocol, const char *pchName, const char *pchPassword, const char *pCookie, int cbCookie)
	{
		this->address = address;
		this->nProtocol = nProtocol;
		this->iChallenge = iChallenge;
		this->iClientChallenge = iClientChallenge;
		this->nAuthProtocol = nAuthProtocol;
		strncpy(this->pchName, pchName, sizeof(this->pchName));
		strncpy(this->pchPassword, pchPassword, sizeof(this->pchPassword));
		strncpy(this->pCookie, pCookie, sizeof(this->pCookie));
		this->cbCookie = cbCookie;
		this->pClient = NULL;
		this->GotValidateAuthTicketResponse = false;
		this->SteamLegal = false;
		this->SteamAuthFailed = false;
		this->eAuthSessionResponse = k_EAuthSessionResponseAuthTicketInvalid;
	}
};
StringHashMap<ConnectClientStorage> g_ConnectClientStorage;

bool g_bEndAuthSessionOnRejectConnection = false;
CSteamID g_lastClientSteamID;
bool g_bSuppressCheckChallengeType = false;

bool IsAuthSessionResponseSteamLegal(EAuthSessionResponse eAuthSessionResponse)
{
	std::stringstream ss(g_SvAuthSessionResponseLegal->GetString());
	int legalAuthSessionResponse[10];
	char ch;
	int n;
	int size = 0;

	while(ss >> n)
	{
		if(ss >> ch)
			legalAuthSessionResponse[size] = n;
		else
			legalAuthSessionResponse[size] = n;
		size++;
	}

	for (int y = 0; y < size; y++)
	{
	    if (eAuthSessionResponse == legalAuthSessionResponse[y])
	        return true;
	}
	return false;
}

DETOUR_DECL_MEMBER1(CSteam3Server__OnValidateAuthTicketResponse, int, ValidateAuthTicketResponse_t *, pResponse)
{
	char aSteamID[32];
	strncpy(aSteamID, pResponse->m_SteamID.Render(), sizeof(aSteamID) - 1);

	ConnectClientStorage Storage;
	bool StorageValid = g_ConnectClientStorage.retrieve(aSteamID, &Storage);

	EAuthSessionResponse eAuthSessionResponse = pResponse->m_eAuthSessionResponse;
	bool SteamLegal = IsAuthSessionResponseSteamLegal(pResponse->m_eAuthSessionResponse);
	bool force = g_SvNoSteam->GetInt() || g_SvForceSteam->GetInt() || !BLoggedOn();

	if(!SteamLegal && force)
		pResponse->m_eAuthSessionResponse = k_EAuthSessionResponseOK;

	if (g_SvLogging->GetInt())
		g_pSM->LogMessage(myself, "%s SteamLegal: %d (%d)", aSteamID, SteamLegal, pResponse->m_eAuthSessionResponse);

	if(StorageValid && !Storage.GotValidateAuthTicketResponse)
	{
		Storage.GotValidateAuthTicketResponse = true;
		Storage.ValidateAuthTicketResponse = *pResponse;
		Storage.SteamLegal = SteamLegal;
		Storage.eAuthSessionResponse = eAuthSessionResponse;
		g_ConnectClientStorage.replace(aSteamID, Storage);
	}

	g_pOnValidateAuthTicketResponse->PushCell(Storage.eAuthSessionResponse);
	g_pOnValidateAuthTicketResponse->PushCell(Storage.GotValidateAuthTicketResponse);
	g_pOnValidateAuthTicketResponse->PushCell(Storage.SteamLegal);
	g_pOnValidateAuthTicketResponse->PushStringEx(aSteamID, sizeof(aSteamID), SM_PARAM_STRING_UTF8 | SM_PARAM_STRING_COPY, SM_PARAM_COPYBACK);
	g_pOnValidateAuthTicketResponse->Execute();

	return DETOUR_MEMBER_CALL(CSteam3Server__OnValidateAuthTicketResponse)(pResponse);
}

DETOUR_DECL_MEMBER9(CBaseServer__ConnectClient, IClient *, netadr_t &, address, int, nProtocol, int, iChallenge, int, iClientChallenge, int, nAuthProtocol, const char *, pchName, const char *, pchPassword, const char *, pCookie, int, cbCookie)
{
	if(nAuthProtocol != k_EAuthProtocolSteam)
	{
		// This is likely a SourceTV client, we don't want to interfere here.
		return DETOUR_MEMBER_CALL(CBaseServer__ConnectClient)(address, nProtocol, iChallenge, iClientChallenge, nAuthProtocol, pchName, pchPassword, pCookie, cbCookie);
	}

	g_pBaseServer = (CBaseServer *)this;

	if(pCookie == NULL || (size_t)cbCookie < sizeof(uint64))
	{
		RejectConnection(address, iClientChallenge, "#GameUI_ServerRejectInvalidSteamCertLen");
		return NULL;
	}

	char ipString[32];
	V_snprintf(ipString, sizeof(ipString), "%u.%u.%u.%u", address.ip[0], address.ip[1], address.ip[2], address.ip[3]);

	char passwordBuffer[255];
	V_strncpy(passwordBuffer, pchPassword, sizeof(passwordBuffer));
	uint64 ullSteamID = *(uint64 *)pCookie;

	void *pvTicket = (void *)((intptr_t)pCookie + sizeof(uint64));
	int cbTicket = cbCookie - sizeof(uint64);

	g_bEndAuthSessionOnRejectConnection = true;
	g_lastClientSteamID = CSteamID(ullSteamID);

	char aSteamID[32];
	strncpy(aSteamID, g_lastClientSteamID.Render(), sizeof(aSteamID));

	// If client is in async state remove the old object and fake an async retVal
	// This can happen if the async ClientPreConnectEx takes too long to be called
	// and the client auto-retries.
	bool AsyncWaiting = false;
	bool ExistingSteamid = false;
	ConnectClientStorage Storage(address, nProtocol, iChallenge, iClientChallenge, nAuthProtocol, pchName, pchPassword, pCookie, cbCookie);
	if(g_ConnectClientStorage.retrieve(aSteamID, &Storage))
	{
		ExistingSteamid = true;
		g_ConnectClientStorage.remove(aSteamID);
		EndAuthSession(g_lastClientSteamID);

		// Only wait for async on auto-retry, manual retries should go through the full chain
		// Don't want to leave the client waiting forever if something breaks in the async forward
		if(Storage.iClientChallenge == iClientChallenge)
			AsyncWaiting = true;
	}

	bool NoSteam = g_SvNoSteam->GetInt() || !BLoggedOn();
	bool SteamAuthFailed = false;
	EBeginAuthSessionResult result = BeginAuthSession(pvTicket, cbTicket, g_lastClientSteamID);
	if(result != k_EBeginAuthSessionResultOK)
	{
		if(!NoSteam)
		{
			RejectConnection(address, iClientChallenge, "#GameUI_ServerRejectSteam");
			return NULL;
		}
		Storage.SteamAuthFailed = SteamAuthFailed = true;
	}

	if(ExistingSteamid && !AsyncWaiting)
	{
		// Another player trying to spoof a Steam ID or game crashed?
		if(memcmp(address.ip, Storage.address.ip, sizeof(address.ip)) != 0)
		{
			if (g_SvLogging->GetInt())
			{
				char ipConnectedString[32];
				V_snprintf(ipConnectedString, sizeof(ipConnectedString), "%u.%u.%u.%u", Storage.address.ip[0], Storage.address.ip[1], Storage.address.ip[2], Storage.address.ip[3]);
				g_pSM->LogMessage(myself, "Spoof Alert: Connecting address: %s | Connected address: %s", ipString, ipConnectedString);
			}

			// Reject NoSteam players
			if(SteamAuthFailed)
			{
				RejectConnection(address, iClientChallenge, "Steam ID already in use.");
				return NULL;
			}

			// Kick existing player
			if(Storage.pClient)
			{
				// Client ip changed during the game ?
				if (!Storage.SteamLegal)
					Storage.pClient->Disconnect("Same Steam ID connected.");
			}
			else
			{
				RejectConnection(address, iClientChallenge, "Please try again later.");
				return NULL;
			}
		}
	}

	char rejectReason[255];
	cell_t retVal = 1;

	if(AsyncWaiting)
		retVal = -1; // Fake async return code when waiting for async call
	else
	{
		g_pConnectForward->PushString(pchName);
		g_pConnectForward->PushStringEx(passwordBuffer, sizeof(passwordBuffer), SM_PARAM_STRING_UTF8 | SM_PARAM_STRING_COPY, SM_PARAM_COPYBACK);
		g_pConnectForward->PushString(ipString);
		g_pConnectForward->PushString(aSteamID);
		g_pConnectForward->PushStringEx(rejectReason, sizeof(rejectReason), SM_PARAM_STRING_UTF8 | SM_PARAM_STRING_COPY, SM_PARAM_COPYBACK);
		g_pConnectForward->Execute(&retVal);
		pchPassword = passwordBuffer;
	}

	if (g_SvLogging->GetInt())
		g_pSM->LogMessage(myself, "%s SteamAuthFailed: %d (%d) | retVal = %d", aSteamID, SteamAuthFailed, result, retVal);

	if(retVal == k_OnClientPreConnectEx_Reject)
	{
		g_ConnectClientStorage.remove(aSteamID);
		RejectConnection(address, iClientChallenge, rejectReason);
		return NULL;
	}

	Storage.pThis = this;
	Storage.ullSteamID = ullSteamID;
	Storage.SteamAuthFailed = SteamAuthFailed;

	if(!g_ConnectClientStorage.replace(aSteamID, Storage))
	{
		RejectConnection(address, iClientChallenge, "Internal error.");
		return NULL;
	}

	if(retVal == k_OnClientPreConnectEx_Async)
	{
		return NULL;
	}

	// k_OnClientPreConnectEx_Accept
	g_bSuppressCheckChallengeType = true;
	IClient *pClient = DETOUR_MEMBER_CALL(CBaseServer__ConnectClient)(address, nProtocol, iChallenge, iClientChallenge, nAuthProtocol, pchName, pchPassword, pCookie, cbCookie);

	Storage.pClient = pClient;
	g_ConnectClientStorage.replace(aSteamID, Storage);

	if(pClient && SteamAuthFailed)
	{
		ValidateAuthTicketResponse_t Response;
		Response.m_SteamID = g_lastClientSteamID;
		Response.m_eAuthSessionResponse = k_EAuthSessionResponseAuthTicketInvalid;
		Response.m_OwnerSteamID = Response.m_SteamID;
		DETOUR_MEMBER_MCALL_CALLBACK(CSteam3Server__OnValidateAuthTicketResponse, g_pSteam3Server)(&Response);
	}

	return pClient;
}

DETOUR_DECL_MEMBER3(CBaseServer__RejectConnection, void, netadr_t &, address, int, iClientChallenge, const char *, pchReason)
{
	if(g_bEndAuthSessionOnRejectConnection)
	{
		EndAuthSession(g_lastClientSteamID);
		g_bEndAuthSessionOnRejectConnection = false;
	}

	return DETOUR_MEMBER_CALL(CBaseServer__RejectConnection)(address, iClientChallenge, pchReason);
}

DETOUR_DECL_MEMBER7(CBaseServer__CheckChallengeType, bool, CBaseClient *, pClient, int, nUserID, netadr_t &, address, int, nAuthProtocol, const char *, pCookie, int, cbCookie, int, iClientChallenge)
{
	if(g_bSuppressCheckChallengeType)
	{
		g_bEndAuthSessionOnRejectConnection = false;

		SetSteamID(pClient, g_lastClientSteamID);

		g_bSuppressCheckChallengeType = false;
		return true;
	}

	return DETOUR_MEMBER_CALL(CBaseServer__CheckChallengeType)(pClient, nUserID, address, nAuthProtocol, pCookie, cbCookie, iClientChallenge);
}


bool Connect::SDK_OnLoad(char *error, size_t maxlen, bool late)
{
	char conf_error[255] = "";
	if(!gameconfs->LoadGameConfigFile("connect2.games", &g_pGameConf, conf_error, sizeof(conf_error)))
	{
		if(conf_error[0])
		{
			snprintf(error, maxlen, "Could not read connect2.games.txt: %s\n", conf_error);
		}
		return false;
	}

	if(!g_pGameConf->GetMemSig("CBaseServer__RejectConnection", (void **)(&g_pRejectConnectionFunc)) || !g_pRejectConnectionFunc)
	{
		snprintf(error, maxlen, "Failed to find CBaseServer__RejectConnection function.\n");
		return false;
	}

	if(!g_pGameConf->GetMemSig("CBaseClient__SetSteamID", (void **)(&g_pSetSteamIDFunc)) || !g_pSetSteamIDFunc)
	{
		snprintf(error, maxlen, "Failed to find CBaseClient__SetSteamID function.\n");
		return false;
	}

#ifndef WIN32
	if(!g_pGameConf->GetMemSig("Steam3Server", (void **)(&g_pSteam3ServerFunc)) || !g_pSteam3ServerFunc)
	{
		snprintf(error, maxlen, "Failed to find Steam3Server function.\n");
		return false;
	}
#else
	void *address;
	if(!g_pGameConf->GetMemSig("CBaseServer__CheckMasterServerRequestRestart", &address) || !address)
	{
		snprintf(error, maxlen, "Failed to find CBaseServer__CheckMasterServerRequestRestart function.\n");
		return false;
	}

	//META_CONPRINTF("CheckMasterServerRequestRestart: %p\n", address);
	address = (void *)((intptr_t)address + 1); // Skip CALL opcode
	intptr_t offset = (intptr_t)(*(void **)address); // Get offset

	g_pSteam3ServerFunc = (Steam3ServerFunc)((intptr_t)address + offset + sizeof(intptr_t));
	//META_CONPRINTF("Steam3Server: %p\n", g_pSteam3ServerFunc);
#endif

	g_pSteam3Server = Steam3Server();
	if(!g_pSteam3Server)
	{
		snprintf(error, maxlen, "Unable to get Steam3Server singleton.\n");
		return false;
	}

	/*
	META_CONPRINTF("ISteamGameServer: %p\n", g_pSteam3Server->m_pSteamGameServer);
	META_CONPRINTF("ISteamUtils: %p\n", g_pSteam3Server->m_pSteamGameServerUtils);
	META_CONPRINTF("ISteamMasterServerUpdater: %p\n", g_pSteam3Server->m_pSteamMasterServerUpdater);
	META_CONPRINTF("ISteamNetworking: %p\n", g_pSteam3Server->m_pSteamGameServerNetworking);
	META_CONPRINTF("ISteamGameServerStats: %p\n", g_pSteam3Server->m_pSteamGameServerStats);
	*/

	CDetourManager::Init(g_pSM->GetScriptingEngine(), g_pGameConf);

	g_Detour_CBaseServer__ConnectClient = DETOUR_CREATE_MEMBER(CBaseServer__ConnectClient, "CBaseServer__ConnectClient");
	if(!g_Detour_CBaseServer__ConnectClient)
	{
		snprintf(error, maxlen, "Failed to detour CBaseServer__ConnectClient.\n");
		return false;
	}
	g_Detour_CBaseServer__ConnectClient->EnableDetour();

	g_Detour_CBaseServer__RejectConnection = DETOUR_CREATE_MEMBER(CBaseServer__RejectConnection, "CBaseServer__RejectConnection");
	if(!g_Detour_CBaseServer__RejectConnection)
	{
		snprintf(error, maxlen, "Failed to detour CBaseServer__RejectConnection.\n");
		return false;
	}
	g_Detour_CBaseServer__RejectConnection->EnableDetour();

	g_Detour_CBaseServer__CheckChallengeType = DETOUR_CREATE_MEMBER(CBaseServer__CheckChallengeType, "CBaseServer__CheckChallengeType");
	if(!g_Detour_CBaseServer__CheckChallengeType)
	{
		snprintf(error, maxlen, "Failed to detour CBaseServer__CheckChallengeType.\n");
		return false;
	}
	g_Detour_CBaseServer__CheckChallengeType->EnableDetour();

	g_Detour_CSteam3Server__OnValidateAuthTicketResponse = DETOUR_CREATE_MEMBER(CSteam3Server__OnValidateAuthTicketResponse, "CSteam3Server__OnValidateAuthTicketResponse");
	if(!g_Detour_CSteam3Server__OnValidateAuthTicketResponse)
	{
		snprintf(error, maxlen, "Failed to detour CSteam3Server__OnValidateAuthTicketResponse.\n");
		return false;
	}
	g_Detour_CSteam3Server__OnValidateAuthTicketResponse->EnableDetour();

	g_pConnectForward = g_pForwards->CreateForward("OnClientPreConnectEx", ET_LowEvent, 5, NULL, Param_String, Param_String, Param_String, Param_String, Param_String);
	g_pOnValidateAuthTicketResponse = g_pForwards->CreateForward("OnValidateAuthTicketResponse", ET_Ignore, 4, NULL, Param_Cell, Param_Cell, Param_Cell, Param_String);

	AutoExecConfig(g_pCVar, true);

	return true;
}

bool Connect::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_ANY(GetServerFactory, gamedll, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameEvents, IGameEventManager2, INTERFACEVERSION_GAMEEVENTSMANAGER2);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);

	ConVar_Register(0, this);

	return true;
}

void Connect::SDK_OnUnload()
{
	if(g_pConnectForward)
		g_pForwards->ReleaseForward(g_pConnectForward);

	if(g_pOnValidateAuthTicketResponse)
		g_pForwards->ReleaseForward(g_pOnValidateAuthTicketResponse);

	if(g_Detour_CBaseServer__ConnectClient)
	{
		g_Detour_CBaseServer__ConnectClient->Destroy();
		g_Detour_CBaseServer__ConnectClient = NULL;
	}
	if(g_Detour_CBaseServer__RejectConnection)
	{
		g_Detour_CBaseServer__RejectConnection->Destroy();
		g_Detour_CBaseServer__RejectConnection = NULL;
	}
	if(g_Detour_CBaseServer__CheckChallengeType)
	{
		g_Detour_CBaseServer__CheckChallengeType->Destroy();
		g_Detour_CBaseServer__CheckChallengeType = NULL;
	}
	if(g_Detour_CSteam3Server__OnValidateAuthTicketResponse)
	{
		g_Detour_CSteam3Server__OnValidateAuthTicketResponse->Destroy();
		g_Detour_CSteam3Server__OnValidateAuthTicketResponse = NULL;
	}

	gameconfs->CloseGameConfigFile(g_pGameConf);
}

bool Connect::RegisterConCommandBase(ConCommandBase *pVar)
{
	/* Always call META_REGCVAR instead of going through the engine. */
	return META_REGCVAR(pVar);
}

cell_t ClientPreConnectEx(IPluginContext *pContext, const cell_t *params)
{
	char *pSteamID;
	pContext->LocalToString(params[1], &pSteamID);

	int retVal = params[2];

	char *rejectReason;
	pContext->LocalToString(params[3], &rejectReason);

	ConnectClientStorage Storage;
	if(!g_ConnectClientStorage.retrieve(pSteamID, &Storage))
		return 1;

	if(retVal == 0)
	{
		RejectConnection(Storage.address, Storage.iClientChallenge, rejectReason);
		g_ConnectClientStorage.remove(pSteamID);
		return 0;
	}

	g_bSuppressCheckChallengeType = true;
	IClient *pClient = DETOUR_MEMBER_MCALL_ORIGINAL(CBaseServer__ConnectClient, Storage.pThis)(Storage.address, Storage.nProtocol, Storage.iChallenge, Storage.iClientChallenge,
		Storage.nAuthProtocol, Storage.pchName, Storage.pchPassword, Storage.pCookie, Storage.cbCookie);

	if(!pClient)
	{
		g_ConnectClientStorage.remove(pSteamID);
		return 1;
	}

	bool force = g_SvNoSteam->GetInt() || g_SvForceSteam->GetInt() || !BLoggedOn();

	if(Storage.SteamAuthFailed && force && !Storage.GotValidateAuthTicketResponse)
	{
		if (g_SvLogging->GetInt())
			g_pSM->LogMessage(myself, "%s Force ValidateAuthTicketResponse", pSteamID);

		Storage.ValidateAuthTicketResponse.m_SteamID = CSteamID(Storage.ullSteamID);
		Storage.ValidateAuthTicketResponse.m_eAuthSessionResponse = k_EAuthSessionResponseOK;
		Storage.ValidateAuthTicketResponse.m_OwnerSteamID = Storage.ValidateAuthTicketResponse.m_SteamID;
		Storage.GotValidateAuthTicketResponse = true;
	}

	// Make sure this is always called in order to verify the client on the server
	if(Storage.GotValidateAuthTicketResponse)
	{
		if (g_SvLogging->GetInt())
			g_pSM->LogMessage(myself, "%s Replay ValidateAuthTicketResponse", pSteamID);

		DETOUR_MEMBER_MCALL_ORIGINAL(CSteam3Server__OnValidateAuthTicketResponse, g_pSteam3Server)(&Storage.ValidateAuthTicketResponse);
	}

	g_ConnectClientStorage.remove(pSteamID);

	return 0;
}

cell_t SteamClientAuthenticated(IPluginContext *pContext, const cell_t *params)
{
	char *pSteamID;
	pContext->LocalToString(params[1], &pSteamID);

	ConnectClientStorage Storage;
	if(g_ConnectClientStorage.retrieve(pSteamID, &Storage))
	{
		if (g_SvLogging->GetInt())
			g_pSM->LogMessage(myself, "%s SteamClientAuthenticated: %d", pSteamID, Storage.SteamLegal);

		return Storage.SteamLegal;
	}
	if (g_SvLogging->GetInt())
		g_pSM->LogMessage(myself, "%s SteamClientAuthenticated: FALSE!", pSteamID);

	return false;
}

cell_t SteamClientGotValidateAuthTicketResponse(IPluginContext *pContext, const cell_t *params)
{
	char *pSteamID;
	pContext->LocalToString(params[1], &pSteamID);

	ConnectClientStorage Storage;
	g_ConnectClientStorage.retrieve(pSteamID, &Storage);

	if (g_SvLogging->GetInt())
		g_pSM->LogMessage(myself, "%s SteamClientGotValidateAuthTicketResponse: %d", pSteamID, Storage.GotValidateAuthTicketResponse);

	return Storage.GotValidateAuthTicketResponse;
}

const sp_nativeinfo_t MyNatives[] =
{
	{ "ClientPreConnectEx", ClientPreConnectEx },
	{ "SteamClientAuthenticated", SteamClientAuthenticated },
	{ "SteamClientGotValidateAuthTicketResponse", SteamClientGotValidateAuthTicketResponse},
	{ NULL, NULL }
};

void Connect::SDK_OnAllLoaded()
{
	sharesys->AddNatives(myself, MyNatives);
}
