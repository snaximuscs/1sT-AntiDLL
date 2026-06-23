<<<<<<< HEAD
﻿#pragma once

#include <ISmmPlugin.h>
#include <engine/iserverplugin.h>
#include <entity2/entitysystem.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>

// Тоглогчийн төлөвийг хадгалах бүтэц
struct PlayerState {
    int slot;
    std::string name;
    uint64_t steamId;
    int violationPoints;
    int lastPing;
    double lastCheckTime;
};

// Тохиргооны бүтэц
struct AntiDllConfig {
    bool pluginEnabled = true;
    bool debugMode = false;
    float checkInterval = 10.0f;
    int maxViolationThreshold = 100;
    int pointsPerDetection = 50;
    int pointDecay = 10;
    std::string punishmentType = "kick";
    std::string webhookUrl = "";
    bool ignoreAdmins = true;
    bool ignoreBots = true;
    int maxPingTolerance = 150;
    std::vector<std::string> blacklistedCvars;
};

class AntiDllPlugin : public ISmmPlugin, public IServerPluginCallbacks
{
public:
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
    bool Unload(char* error, size_t maxlen);
    
    // Metamod Hooks
    void AllPluginsLoaded();
    const char* GetAuthor() { return "Snaximusss+"; }
    const char* GetName() { return "1sT-AntiDLL"; }
    const char* GetDescription() { return "Server-side defensive anti-cheat plugin"; }
    const char* GetURL() { return ""; }
    const char* GetLicense() { return "GPL"; }
    const char* GetVersion() { return "2.0.0-C++"; }
    const char* GetDate() { return __DATE__; }
    const char* GetLogTag() { return "ANTIDLL"; }

    // Game Frame Handler
    void GameFrame(bool simulating);

private:
    void LoadConfig();
    void CheckPlayers(double currentTime);
    void HandleViolation(int slot, const std::string& reason);
    void ExecutePunishment(int slot, const std::string& reason);
    void SendAsyncWebhook(const std::string& title, const std::string& message, int color);

    AntiDllConfig m_Config;
    std::unordered_map<int, PlayerState> m_PlayerStates;
    std::mutex m_StateMutex;
    double m_LastDecayTime = 0.0;
    double m_LastCheckTime = 0.0;
};

extern AntiDllPlugin g_AntiDllPlugin;
PLUGIN_GLOBALVARS();
=======
﻿#ifndef _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
#define _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_

#include <ISmmPlugin.h>
#include <sh_vector.h>
#include "utlvector.h"
#include "ehandle.h"
#include <iserver.h>
#include <entity2/entitysystem.h>
#include "igameevents.h"
#include "vector.h"
#include <deque>
#include <functional>
#include <utlstring.h>
#include <KeyValues.h>
#include "CGameRules.h"
#include "CCSPlayerController.h"
#include "include/menus.h"

class AntiDLL final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
	bool Unload(char* error, size_t maxlen);
	void AllPluginsLoaded();
private:
	const char* GetAuthor();
	const char* GetName();
	const char* GetDescription();
	const char* GetURL();
	const char* GetLicense();
	const char* GetVersion();
	const char* GetDate();
	const char* GetLogTag();
};

#endif //_INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
>>>>>>> 70a0dc4ddd27e79e48f3b3e85109de1731d7aeb3
