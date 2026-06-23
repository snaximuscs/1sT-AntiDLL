#pragma once

#include <ISmmPlugin.h>
#include <entity2/entitysystem.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>

// Хэрэв таны cpp дээр ашиглагдаж байгаа бол эдгээр API-г урьдчилан зарлана
class IUtilsApi;
class IPlayersApi;

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

// Классын нэрийг таны cpp-тэй ижил "AntiDLL" болгон өөрчлөв
class AntiDLL : public ISmmPlugin, public IMetamodListener
{
public:
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
    bool Unload(char* error, size_t maxlen);
    
    // Metamod Hooks
    void AllPluginsLoaded();
    const char* GetAuthor();
    const char* GetName();
    const char* GetDescription();
    const char* GetURL();
    const char* GetLicense();
    const char* GetVersion();
    const char* GetDate();
    const char* GetLogTag();

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

extern AntiDLL g_AntiDLL;
PLUGIN_GLOBALVARS();