#include <stdio.h>
#include "antidll.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"
#include "raytrace.h"
#include "webhook_queue.h"
#include "mysql_manager.h"
#include <igameevents.h>
#include <fstream>
#include <ctime>
#include <algorithm>

//==============================================================================
// Globals & SDK plumbing
//==============================================================================

AntiDLL g_AntiDLL;
PLUGIN_EXPOSE(AntiDLL, g_AntiDLL);
IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars* gpGlobals = nullptr;

IUtilsApi* g_pUtils = nullptr;
IPlayersApi* g_pPlayers = nullptr;

static std::string  g_MapName = "unknown";
static bool         g_RoundActive = true;
static std::string  g_DetectedIp   = "0.0.0.0";
static int          g_DetectedPort = 27015;

//==============================================================================
// Detection categories
//==============================================================================

enum DetCategory
{
    DET_DLL_EVENT = 0,
    DET_HIDDEN_TARGET,
    DET_AIM_SNAP,
    DET_TRIGGER,
    DET_COMPOSITE,
    DET_COUNT
};

static const char* DetCategoryStr(DetCategory c)
{
    switch (c)
    {
    case DET_DLL_EVENT:     return "DLL/Event Abuse";
    case DET_HIDDEN_TARGET: return "Visibility/Hidden Target Tracking";
    case DET_AIM_SNAP:      return "Aim Pattern";
    case DET_TRIGGER:       return "Trigger Pattern";
    case DET_COMPOSITE:     return "Composite Risk";
    default:                return "Unknown";
    }
}

//==============================================================================
// Config
//==============================================================================

struct Config
{
    // --- Core ---
    bool  enabled        = true;
    bool  debugMode      = false;
    bool  ignoreBots     = true;
    bool  ignoreAdmins   = true;

    // --- Violation Points ---
    int   violationThreshold = 300;
    int   pointsPerDetection = 50;
    int   pointDecay         = 10;
    float decayInterval      = 30.0f;

    // --- Punishment ---
    int          punishType   = 0;
    std::string  punishCommand;
    std::string  chatMessage;

    // --- RayTrace Visibility ---
    bool  raytraceRequired  = true;
    bool  raytraceDebugBeam = false;

    // --- Hidden-Target Tracking ---
    bool  whEnabled          = false;
    bool  whDebugOnly        = true;
    float whCheckInterval    = 0.5f;
    float whTrackingThreshold = 4.0f;
    float whResetGap         = 0.75f;
    float whCooldown         = 20.0f;
    float whMaxDistance      = 2500.0f;
    int   whMinSamples       = 8;
    int   whViolationPoints  = 25;
    float whDotThreshold     = 0.995f;

    // --- DLL detection (event-listener probe) ---
    float dllInterval = 5.0f;

    // --- Aim Pattern Detection ---
    bool  aimEnabled           = true;
    bool  aimDebugOnly         = true;
    float aimCheckInterval     = 0.25f;
    int   aimMinSamples        = 5;
    float aimSnapAngleThreshold = 30.0f;
    float aimSnapDotThreshold  = 0.96f;
    float aimCooldown          = 30.0f;
    int   aimPoints            = 15;

    // --- Trigger Pattern Detection ---
    bool  triggerEnabled       = true;
    bool  triggerDebugOnly     = true;
    int   triggerMinSamples    = 5;
    float triggerMaxReactionSec = 0.5f;
    float triggerCooldown      = 30.0f;
    int   triggerPoints        = 15;

    // --- Composite Risk ---
    bool  compositeEnabled       = true;
    int   compositeMinCategories = 3;
    int   compositeBonusPoints   = 50;

    // --- Logging ---
    bool         logs    = true;
    std::string  logFile = "addons/AntiDLL/logs/antidll.log";

    // --- Discord ---
    bool         webhookEnabled     = false;
    std::string  webhookUrl;
    int          webhookMinPoints   = 25;
    bool         webhookPluginLoad  = true;
    bool         webhookViolations  = true;
    bool         webhookPunishments = true;

    // --- Database ---
    bool         dbEnabled     = false;
    std::string  dbConfigName  = "antidll";

    // --- Server Identity ---
    bool         identityEnabled         = true;
    std::string  identityTable           = "antidll_servers";
    std::string  detectionTable          = "antidll_detections";
    float        identityRefreshInterval = 300.0f;
    std::string  identityFailName        = "Unknown Server";

    // Populated from configs/databases.cfg at load time
    MySQLConfig  mysql;
};

static Config g_Cfg;
static std::vector<std::string> g_vEvents;
static WebhookQueue g_Webhook;
static MySQLManager g_MySQL;

//==============================================================================
// PlayerState
//==============================================================================

struct PlayerState
{
    uint64_t    steamId = 0;
    std::string name;
    int         points = 0;
    double      lastViolationTime = 0.0;
    bool        punished = false;
    std::vector<std::string> history;
    int         evidenceCount[DET_COUNT] = {};

    // Hidden-target tracking
    int    whTargetSlot      = -1;
    double whTrackStart      = 0.0;
    double whLastSample      = 0.0;
    int    whSampleCount     = 0;
    double whLastViolationTime = 0.0;

    // Aim pattern tracking
    QAngle prevAngles;
    double prevAngleTime     = 0.0;
    bool   hasPrevAngles     = false;
    int    prevShotsFired    = 0;
    int    aimSnapEvidence   = 0;
    double aimLastViolationTime = 0.0;

    // Trigger pattern tracking
    struct TargetVis { bool wasVisible = false; double changeTime = 0.0; };
    std::unordered_map<int, TargetVis> targetVis;
    int    triggerEvidence    = 0;
    double triggerLastViolationTime = 0.0;
    int    triggerPrevShotsFired = 0;
};

static std::unordered_map<int, PlayerState> g_States;
static std::mutex g_StateMutex;

static PlayerState& StateFor(int slot) { return g_States[slot]; }

static void ResetPlayer(int slot)
{
    std::lock_guard<std::mutex> lock(g_StateMutex);
    g_States.erase(slot);
}

//==============================================================================
// Math & entity access
//==============================================================================

static bool IsZeroVector(const Vector& v)
{
    return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f;
}

static float VecLength(const Vector& v)
{
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static float VecDot(const Vector& a, const Vector& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vector VecSub(const Vector& a, const Vector& b)
{
    return Vector(a.x - b.x, a.y - b.y, a.z - b.z);
}

static Vector VecNormalize(const Vector& v)
{
    float len = VecLength(v);
    if (len < 0.0001f) return Vector(0, 0, 0);
    return Vector(v.x / len, v.y / len, v.z / len);
}

static Vector AnglesToForward(const QAngle& angles)
{
    float pitch = angles.x * (float)(M_PI / 180.0);
    float yaw   = angles.y * (float)(M_PI / 180.0);
    return Vector(cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), -sinf(pitch));
}

static float AngleDelta(const QAngle& a, const QAngle& b)
{
    float dp = a.x - b.x;
    float dy = a.y - b.y;
    while (dy >  180.0f) dy -= 360.0f;
    while (dy < -180.0f) dy += 360.0f;
    return sqrtf(dp * dp + dy * dy);
}

namespace schema {
    int32_t GetServerOffset(const char* pszClassName, const char* pszPropName);
}

static CEntityInstance* GetPlayerPawn(int slot)
{
    if (!g_pEntitySystem) return nullptr;
    CEntityInstance* ctrl = g_pEntitySystem->GetEntityInstance(CEntityIndex(slot + 1));
    if (!ctrl) return nullptr;

    static int32_t off = schema::GetServerOffset("CBasePlayerController", "m_hPawn");
    if (off == -1) return nullptr;

    uint32_t handle = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(ctrl) + off);
    if (handle == 0xFFFFFFFF) return nullptr;

    return g_pGameEntitySystem->GetEntityInstance(CEntityIndex(handle & 0x7FFF));
}

static Vector GetPlayerPosition(int slot)
{
    CEntityInstance* pawn = GetPlayerPawn(slot);
    if (!pawn) return Vector(0, 0, 0);

    static int32_t bodyOff = schema::GetServerOffset("CBaseEntity", "m_CBodyComponent");
    if (bodyOff == -1) return Vector(0, 0, 0);
    void* body = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pawn) + bodyOff);
    if (!body) return Vector(0, 0, 0);

    static int32_t sceneOff = schema::GetServerOffset("CBodyComponent", "m_pSceneNode");
    if (sceneOff == -1) return Vector(0, 0, 0);
    void* scene = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(body) + sceneOff);
    if (!scene) return Vector(0, 0, 0);

    static int32_t originOff = schema::GetServerOffset("CGameSceneNode", "m_vecAbsOrigin");
    if (originOff == -1) return Vector(0, 0, 0);
    return *reinterpret_cast<Vector*>(reinterpret_cast<uintptr_t>(scene) + originOff);
}

static Vector GetPlayerEyePosition(int slot)
{
    Vector origin = GetPlayerPosition(slot);
    if (IsZeroVector(origin)) return origin;

    CEntityInstance* pawn = GetPlayerPawn(slot);
    if (!pawn) return Vector(0, 0, 0);

    static int32_t off = schema::GetServerOffset("CBaseModelEntity", "m_vecViewOffset");
    if (off == -1) { origin.z += 64.0f; return origin; }

    Vector view = *reinterpret_cast<Vector*>(reinterpret_cast<uintptr_t>(pawn) + off);
    return Vector(origin.x + view.x, origin.y + view.y, origin.z + view.z);
}

static QAngle GetPlayerEyeAngles(int slot)
{
    CEntityInstance* pawn = GetPlayerPawn(slot);
    if (!pawn) return QAngle(0, 0, 0);

    static int32_t off = schema::GetServerOffset("CCSPlayerPawn", "m_angEyeAngles");
    if (off == -1) return QAngle(0, 0, 0);
    return *reinterpret_cast<QAngle*>(reinterpret_cast<uintptr_t>(pawn) + off);
}

static int GetPlayerTeam(int slot)
{
    CEntityInstance* pawn = GetPlayerPawn(slot);
    if (!pawn) return 0;

    static int32_t off = schema::GetServerOffset("CBaseEntity", "m_iTeamNum");
    if (off == -1) return 0;
    return *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pawn) + off);
}

static bool IsPlayerAlive(int slot)
{
    CEntityInstance* pawn = GetPlayerPawn(slot);
    if (!pawn) return false;

    static int32_t off = schema::GetServerOffset("CBaseEntity", "m_lifeState");
    if (off == -1) return false;
    return *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pawn) + off) == 0;
}

static int GetPlayerShotsFired(int slot)
{
    CEntityInstance* pawn = GetPlayerPawn(slot);
    if (!pawn) return 0;

    static int32_t off = schema::GetServerOffset("CCSPlayerPawn", "m_iShotsFired");
    if (off == -1) return 0;
    return *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pawn) + off);
}

static bool IsValidHumanPlayer(int slot)
{
    if (slot < 0 || slot >= 64) return false;
    if (!g_pPlayers) return false;
    if (g_pPlayers->IsFakeClient(slot)) return false;
    if (!g_pPlayers->IsInGame(slot)) return false;
    if (g_pPlayers->GetSteamID64(slot) == 0) return false;
    return true;
}

static bool IsWarmupPeriod()
{
    CCSGameRules* rules = g_pUtils ? g_pUtils->GetCCSGameRules() : nullptr;
    if (!rules) return false;
    static int32_t off = schema::GetServerOffset("CCSGameRules", "m_bWarmupPeriod");
    if (off == -1) return false;
    return *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(rules) + off);
}

static bool IsFreezePeriod()
{
    CCSGameRules* rules = g_pUtils ? g_pUtils->GetCCSGameRules() : nullptr;
    if (!rules) return false;
    static int32_t off = schema::GetServerOffset("CCSGameRules", "m_bFreezePeriod");
    if (off == -1) return false;
    return *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(rules) + off);
}

static int GetPlayerCount()
{
    int n = 0;
    if (!g_pPlayers) return 0;
    for (int i = 0; i < 64; i++)
        if (g_pPlayers->IsInGame(i)) n++;
    return n;
}

static int GetMaxClients()
{
    return gpGlobals ? gpGlobals->maxClients : 0;
}

//==============================================================================
// Server address auto-detection (reads /proc/self/cmdline on Linux)
//==============================================================================

static void DetectServerAddress()
{
    g_DetectedPort = 27015;
    g_DetectedIp   = "0.0.0.0";

    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    if (!f.is_open()) return;

    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    std::vector<std::string> args;
    size_t start = 0;
    for (size_t i = 0; i <= data.size(); i++)
    {
        if (i == data.size() || data[i] == '\0')
        {
            if (i > start)
                args.push_back(data.substr(start, i - start));
            start = i + 1;
        }
    }

    for (size_t i = 0; i + 1 < args.size(); i++)
    {
        if ((args[i] == "-port" || args[i] == "+hostport") )
        {
            int p = atoi(args[i + 1].c_str());
            if (p > 0) g_DetectedPort = p;
        }
        if (args[i] == "-ip" || args[i] == "+ip" || args[i] == "+net_public_adr")
        {
            if (!args[i + 1].empty() && args[i + 1][0] != '-' && args[i + 1][0] != '+')
                g_DetectedIp = args[i + 1];
        }
        if (args[i] == "+hostip")
        {
            long hostip = strtol(args[i + 1].c_str(), nullptr, 10);
            if (hostip > 0)
            {
                char buf[32];
                snprintf(buf, sizeof(buf), "%ld.%ld.%ld.%ld",
                    (hostip >> 24) & 0xFF, (hostip >> 16) & 0xFF,
                    (hostip >> 8) & 0xFF, hostip & 0xFF);
                g_DetectedIp = buf;
            }
        }
    }
}

//==============================================================================
// Server identity helpers
//==============================================================================

static ServerIdentity GetServerIdentity()
{
    if (g_MySQL.IsLoaded())
        return g_MySQL.GetIdentity();

    ServerIdentity id;
    id.serverName = g_Cfg.identityFailName;
    id.ip         = g_DetectedIp;
    id.port       = g_DetectedPort;
    return id;
}

static std::string GetServerDisplayName()
{
    return GetServerIdentity().serverName;
}

static std::string GetServerAddress()
{
    ServerIdentity id = GetServerIdentity();
    return id.ip + ":" + std::to_string(id.port);
}

static std::string GetPlayerCountStr()
{
    return std::to_string(GetPlayerCount()) + "/" + std::to_string(GetMaxClients());
}

//==============================================================================
// RayTrace visibility
//==============================================================================

static bool IsEnemyVisibleByRayTrace(int viewerSlot, int enemySlot)
{
    if (!RayTrace_IsAvailable())
        return true;

    CEntityInstance* viewer = g_pGameEntitySystem
        ? g_pGameEntitySystem->GetEntityInstance(CEntityIndex(viewerSlot + 1))
        : nullptr;
    if (!viewer)
        return true;

    Vector eye = GetPlayerEyePosition(viewerSlot);
    Vector origin = GetPlayerPosition(enemySlot);
    if (IsZeroVector(eye) || IsZeroVector(origin))
        return true;

    const Vector points[3] = {
        Vector(origin.x, origin.y, origin.z + 72.0f),
        Vector(origin.x, origin.y, origin.z + 52.0f),
        Vector(origin.x, origin.y, origin.z + 36.0f),
    };

    for (const Vector& p : points)
    {
        if (!RayTrace_LineBlockedByWorld(eye, p, viewer))
            return true;
    }
    return false;
}

//==============================================================================
// Logger
//==============================================================================

static std::string NowString()
{
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

static void LogStructured(const std::string& line)
{
    if (!g_Cfg.logs) return;

    std::string full = "[" + NowString() + "] " + line;

    std::ofstream f(g_Cfg.logFile, std::ios::app);
    if (f.is_open())
    {
        f << full << "\n";
        f.close();
    }
    else if (g_pUtils)
    {
        g_pUtils->LogToFile("AntiDLL", "%s\n", full.c_str());
    }

    if (g_Cfg.debugMode && g_pUtils)
        g_pUtils->PrintToChatAll(" \x02[1sT-AntiDLL]\x01 %s", line.c_str());
}

//==============================================================================
// Discord webhook helpers — rich embed format
//==============================================================================

static std::string SqlEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        if (c == '\'') { out += "''"; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\0') { /* drop */ }
        else out.push_back(c);
    }
    return out;
}

static std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { /* drop */ }
        else out.push_back(c);
    }
    return out;
}

struct WebhookField
{
    std::string name;
    std::string value;
    bool        isInline = true;
};

static void WebhookSendRich(const std::string& title, const std::vector<WebhookField>& fields, int color)
{
    if (!g_Cfg.webhookEnabled || g_Cfg.webhookUrl.empty()) return;

    std::string fj;
    for (size_t i = 0; i < fields.size(); i++)
    {
        if (i > 0) fj += ",";
        fj += "{\"name\":\"" + JsonEscape(fields[i].name) +
              "\",\"value\":\"" + JsonEscape(fields[i].value) +
              "\",\"inline\":" + (fields[i].isInline ? "true" : "false") + "}";
    }

    std::string payload = "{\"embeds\":[{\"title\":\"" + JsonEscape(title) +
        "\",\"color\":" + std::to_string(color) +
        ",\"fields\":[" + fj + "]}]}";
    g_Webhook.Enqueue(payload);
}

static void WebhookSend(const std::string& title, const std::string& description, int color)
{
    if (!g_Cfg.webhookEnabled || g_Cfg.webhookUrl.empty()) return;

    std::string payload = "{\"embeds\":[{\"title\":\"" + JsonEscape(title) +
        "\",\"description\":\"" + JsonEscape(description) +
        "\",\"color\":" + std::to_string(color) + "}]}";
    g_Webhook.Enqueue(payload);
}

static std::vector<WebhookField> BuildServerFields()
{
    ServerIdentity id = GetServerIdentity();
    std::vector<WebhookField> fields = {
        {"Server",  id.serverName, true},
        {"Address", id.ip + ":" + std::to_string(id.port), true},
    };
    if (!id.serverGroup.empty())
        fields.push_back({"Group", id.serverGroup, true});
    if (!id.region.empty())
        fields.push_back({"Region", id.region, true});
    fields.push_back({"Map",     g_MapName,          true});
    fields.push_back({"Players", GetPlayerCountStr(), true});
    return fields;
}

//==============================================================================
// Config loading
//==============================================================================

CGameEntitySystem* GameEntitySystem()
{
    return g_pUtils->GetCGameEntitySystem();
}

static void StartupServer()
{
    g_pGameEntitySystem = GameEntitySystem();
    g_pEntitySystem = g_pUtils->GetCEntitySystem();
    gpGlobals = g_pUtils->GetCGlobalVars();
}

static void LoadEventFile()
{
    g_vEvents.clear();
    char szPath[512];
    g_SMAPI->Format(szPath, sizeof(szPath), "%s/addons/configs/AntiDLL/events.txt", g_SMAPI->GetBaseDir());
    std::ifstream file(szPath);
    if (!file.is_open())
    {
        if (g_pUtils)
            g_pUtils->ErrorLog("[%s] Failed to load events file", g_PLAPI->GetLogTag());
        return;
    }
    std::string line;
    while (std::getline(file, line))
    {
        line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        if (!line.empty())
            g_vEvents.push_back(line);
    }
    file.close();
}

static bool LoadDatabaseConfig(const std::string& configName, MySQLConfig& out)
{
    const char* paths[] = {
        "addons/counterstrikesharp/configs/databases.cfg",
        "configs/databases.cfg",
    };

    KeyValues* pKV = new KeyValues("Databases");
    bool loaded = false;
    const char* foundPath = nullptr;
    for (const char* path : paths)
    {
        if (pKV->LoadFromFile(g_pFullFileSystem, path))
        {
            loaded = true;
            foundPath = path;
            break;
        }
    }

    if (!loaded)
    {
        META_CONPRINTF("[1sT-AntiDLL] databases.cfg not found in any search path\n");
        delete pKV;
        return false;
    }

    KeyValues* pSection = pKV->FindKey(configName.c_str());
    if (!pSection)
    {
        META_CONPRINTF("[1sT-AntiDLL] Database config '%s' not found in %s\n",
            configName.c_str(), foundPath);
        delete pKV;
        return false;
    }

    out.host     = pSection->GetString("host", "127.0.0.1");
    out.port     = pSection->GetInt("port", 3306);
    out.user     = pSection->GetString("user", "root");
    out.password = pSection->GetString("pass", "");
    out.database = pSection->GetString("database", "antidll");
    out.enabled  = true;

    META_CONPRINTF("[1sT-AntiDLL] Loaded database config '%s' from %s (host=%s:%d db=%s)\n",
        configName.c_str(), foundPath, out.host.c_str(), out.port, out.database.c_str());

    delete pKV;
    return true;
}

static bool LoadConfig()
{
    KeyValues* pKV = new KeyValues("Config");
    if (!pKV->LoadFromFile(g_pFullFileSystem, "addons/configs/AntiDLL/settings.ini"))
    {
        if (g_pUtils)
            g_pUtils->ErrorLog("[%s] Failed to load config", g_PLAPI->GetLogTag());
        delete pKV;
        return false;
    }

    Config c;

    // Core
    c.enabled      = pKV->GetBool("enabled", true);
    c.debugMode    = pKV->GetBool("debug_mode", false);
    c.ignoreBots   = pKV->GetBool("ignore_bots", true);
    c.ignoreAdmins = pKV->GetBool("ignore_admins", true);

    // Violation points
    c.violationThreshold = pKV->GetInt("violation_threshold", 300);
    c.pointsPerDetection = pKV->GetInt("points_per_detection", 50);
    c.pointDecay         = pKV->GetInt("point_decay", 10);
    c.decayInterval      = pKV->GetFloat("decay_interval", 30.0f);

    // Punishment
    c.punishType    = pKV->GetInt("punish_type", 0);
    c.punishCommand = pKV->GetString("punish_command", "");
    c.chatMessage   = pKV->GetString("chat_message", "");

    // RayTrace
    c.raytraceRequired  = pKV->GetBool("raytrace_required", true);
    c.raytraceDebugBeam = pKV->GetBool("raytrace_debug_beam", false);

    // Hidden-target tracking
    c.whEnabled          = pKV->GetBool("wh_detection_enabled", false);
    c.whDebugOnly        = pKV->GetBool("wh_debug_only", true);
    c.whCheckInterval    = pKV->GetFloat("wh_check_interval", 0.5f);
    c.whTrackingThreshold = pKV->GetFloat("wh_tracking_threshold", 4.0f);
    c.whResetGap         = pKV->GetFloat("wh_reset_gap", 0.75f);
    c.whCooldown         = pKV->GetFloat("wh_cooldown", 20.0f);
    c.whMaxDistance      = pKV->GetFloat("wh_max_distance", 2500.0f);
    c.whMinSamples       = pKV->GetInt("wh_min_samples", 8);
    c.whViolationPoints  = pKV->GetInt("wh_violation_points", 25);
    c.whDotThreshold     = pKV->GetFloat("wh_dot_threshold", 0.995f);

    // DLL detection
    c.dllInterval = pKV->GetFloat("interval", 5.0f);

    // Aim pattern
    c.aimEnabled           = pKV->GetBool("aim_detection_enabled", true);
    c.aimDebugOnly         = pKV->GetBool("aim_debug_only", true);
    c.aimCheckInterval     = pKV->GetFloat("aim_check_interval", 0.25f);
    c.aimMinSamples        = pKV->GetInt("aim_min_samples", 5);
    c.aimSnapAngleThreshold = pKV->GetFloat("aim_snap_angle_threshold", 30.0f);
    c.aimSnapDotThreshold  = pKV->GetFloat("aim_snap_dot_threshold", 0.96f);
    c.aimCooldown          = pKV->GetFloat("aim_cooldown", 30.0f);
    c.aimPoints            = pKV->GetInt("aim_points", 15);

    // Trigger pattern
    c.triggerEnabled       = pKV->GetBool("trigger_detection_enabled", true);
    c.triggerDebugOnly     = pKV->GetBool("trigger_debug_only", true);
    c.triggerMinSamples    = pKV->GetInt("trigger_min_samples", 5);
    c.triggerMaxReactionSec = pKV->GetFloat("trigger_max_reaction_sec", 0.5f);
    c.triggerCooldown      = pKV->GetFloat("trigger_cooldown", 30.0f);
    c.triggerPoints        = pKV->GetInt("trigger_points", 15);

    // Composite risk
    c.compositeEnabled       = pKV->GetBool("composite_enabled", true);
    c.compositeMinCategories = pKV->GetInt("composite_min_categories", 3);
    c.compositeBonusPoints   = pKV->GetInt("composite_bonus_points", 50);

    // Logging
    c.logs    = pKV->GetBool("logs", true);
    c.logFile = pKV->GetString("log_file", "addons/AntiDLL/logs/antidll.log");

    // Discord
    c.webhookEnabled     = pKV->GetBool("webhook_enabled", false);
    c.webhookUrl         = pKV->GetString("webhook_url", "");
    c.webhookMinPoints   = pKV->GetInt("webhook_min_points", 25);
    c.webhookPluginLoad  = pKV->GetBool("webhook_plugin_load", true);
    c.webhookViolations  = pKV->GetBool("webhook_violations", true);
    c.webhookPunishments = pKV->GetBool("webhook_punishments", true);

    // Database
    c.dbEnabled    = pKV->GetBool("database_enabled", false);
    c.dbConfigName = pKV->GetString("database_config", "antidll");

    // Server identity
    c.identityEnabled         = pKV->GetBool("server_identity_enabled", true);
    c.identityTable           = pKV->GetString("server_identity_table", "antidll_servers");
    c.detectionTable          = pKV->GetString("detection_log_table", "antidll_detections");
    c.identityRefreshInterval = pKV->GetFloat("server_identity_refresh_interval", 300.0f);
    c.identityFailName        = pKV->GetString("server_identity_fail_name", "Unknown Server");

    // Load DB credentials from configs/databases.cfg (not from settings.ini)
    if (c.dbEnabled)
    {
        if (!LoadDatabaseConfig(c.dbConfigName, c.mysql))
        {
            c.dbEnabled = false;
            c.mysql.enabled = false;
        }
        else
        {
            c.mysql.serverTable    = c.identityTable;
            c.mysql.detectionTable = c.detectionTable;
            c.mysql.failName       = c.identityFailName;
        }
    }

    // Safety clamps
    if (c.whCheckInterval     < 0.25f) c.whCheckInterval = 0.25f;
    if (c.whTrackingThreshold < 2.0f)  c.whTrackingThreshold = 2.0f;
    if (c.whDotThreshold      < 0.98f) c.whDotThreshold = 0.98f;
    if (c.whViolationPoints   > 50)    c.whViolationPoints = 50;
    if (c.violationThreshold  < 100)   c.violationThreshold = 100;
    if (c.whCooldown          < 5.0f)  c.whCooldown = 5.0f;
    if (c.decayInterval       < 1.0f)  c.decayInterval = 1.0f;
    if (c.whMinSamples        < 1)     c.whMinSamples = 1;
    if (c.aimCheckInterval    < 0.1f)  c.aimCheckInterval = 0.1f;
    if (c.aimMinSamples       < 2)     c.aimMinSamples = 2;
    if (c.aimSnapAngleThreshold < 5.0f) c.aimSnapAngleThreshold = 5.0f;
    if (c.aimPoints           > 50)    c.aimPoints = 50;
    if (c.triggerMinSamples   < 2)     c.triggerMinSamples = 2;
    if (c.triggerPoints       > 50)    c.triggerPoints = 50;
    if (c.compositeBonusPoints > 100)  c.compositeBonusPoints = 100;
    if (c.identityRefreshInterval < 30.0f) c.identityRefreshInterval = 30.0f;

    delete pKV;
    g_Cfg = c;
    RayTrace_SetDebugBeam(g_Cfg.raytraceDebugBeam);
    return true;
}

//==============================================================================
// Violation manager
//==============================================================================

static void ReplaceString(std::string& str, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos)
    {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
}

static void Punish(int slot, uint64 steamId, const std::string& reason)
{
    const char* szName = g_pPlayers->GetPlayerName(slot);
    char szSteamID64[64];
    g_SMAPI->Format(szSteamID64, sizeof(szSteamID64), "%llu", steamId);

    LogStructured(std::string("action=PUNISH player=") + szName + " steamid=" + szSteamID64 + " reason=\"" + reason + "\"");

    if (g_Cfg.webhookPunishments)
    {
        auto fields = BuildServerFields();
        fields.push_back({"Player", szName, true});
        fields.push_back({"SteamID64", szSteamID64, true});
        fields.push_back({"Reason", reason, false});
        fields.push_back({"Action", g_Cfg.punishType ? "ban" : "kick", true});
        WebhookSendRich("1sT-AntiDLL \xe2\x80\x94 punishment executed", fields, 0xFF0000);
    }

    if (!g_Cfg.chatMessage.empty())
    {
        std::string msg = g_Cfg.chatMessage;
        ReplaceString(msg, "{name}", szName);
        ReplaceString(msg, "{steamid}", szSteamID64);
        ReplaceString(msg, "{userid}", std::to_string(slot));
        g_pUtils->PrintToChatAll(msg.c_str());
    }

    if (g_Cfg.punishType)
    {
        std::string cmd = g_Cfg.punishCommand;
        ReplaceString(cmd, "{name}", szName);
        ReplaceString(cmd, "{steamid}", szSteamID64);
        ReplaceString(cmd, "{userid}", std::to_string(slot));
        engine->ServerCommand(cmd.c_str());
    }
    else
    {
        engine->DisconnectClient(slot, NETWORK_DISCONNECT_KICKED);
    }
}

static void AddViolation(int slot, uint64 steamId, int points, const std::string& reason,
                          DetCategory category, const std::string& evidenceJson = "")
{
    bool doPunish = false;
    int before = 0, after = 0;
    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        PlayerState& st = g_States[slot];
        st.steamId = steamId;
        st.name = g_pPlayers->GetPlayerName(slot);
        before = st.points;
        st.points += points;
        after = st.points;
        st.lastViolationTime = gpGlobals ? gpGlobals->curtime : 0.0;
        st.history.push_back(reason);
        if (st.history.size() > 16) st.history.erase(st.history.begin());
        if (category < DET_COUNT) st.evidenceCount[category]++;

        if (st.points >= g_Cfg.violationThreshold && !st.punished)
        {
            st.punished = true;
            doPunish = true;
        }
    }

    const char* szName = g_pPlayers->GetPlayerName(slot);
    char szSteamID64[64];
    g_SMAPI->Format(szSteamID64, sizeof(szSteamID64), "%llu", steamId);

    LogStructured(std::string("type=VIOLATION category=") + DetCategoryStr(category) +
        " player=" + szName + " steamid=" + szSteamID64 + " reason=\"" + reason + "\"" +
        " points=" + std::to_string(after) + "/" + std::to_string(g_Cfg.violationThreshold));

    // MySQL detection log
    if (g_MySQL.IsConnected())
    {
        ServerIdentity sid = GetServerIdentity();
        const char* playerIp = g_pPlayers->GetIpAddress(slot);
        std::string sql = "INSERT INTO " + g_Cfg.detectionTable +
            " (server_name, server_group, region, server_ip, server_port, map_name,"
            " player_name, steamid64, player_ip, detection_category, detection_reason,"
            " points_added, points_total, threshold_points, action_taken, evidence_json)"
            " VALUES ('" + SqlEscape(sid.serverName) + "','" + SqlEscape(sid.serverGroup) + "','" +
            SqlEscape(sid.region) + "','" + SqlEscape(sid.ip) + "'," +
            std::to_string(sid.port) + ",'" + SqlEscape(g_MapName) + "','" +
            SqlEscape(szName) + "','" + szSteamID64 + "','" +
            SqlEscape(playerIp ? playerIp : "") + "','" +
            SqlEscape(DetCategoryStr(category)) + "','" + SqlEscape(reason) + "'," +
            std::to_string(points) + "," + std::to_string(after) + "," +
            std::to_string(g_Cfg.violationThreshold) + ",'" +
            (doPunish ? (g_Cfg.punishType ? "ban" : "kick") : "none") + "','" +
            SqlEscape(evidenceJson) + "')";
        g_MySQL.QueueSQL(sql);
    }

    // Webhook
    if (g_Cfg.webhookViolations && points >= g_Cfg.webhookMinPoints)
    {
        auto fields = BuildServerFields();
        fields.push_back({"Player",    szName,                        true});
        fields.push_back({"SteamID64", szSteamID64,                   true});
        fields.push_back({"Reason",    reason,                        false});
        fields.push_back({"Category",  DetCategoryStr(category),       true});
        fields.push_back({"Points",    std::to_string(after) + "/" + std::to_string(g_Cfg.violationThreshold), true});
        fields.push_back({"Action",    doPunish ? (g_Cfg.punishType ? "ban" : "kick") : "none", true});
        if (!evidenceJson.empty())
            fields.push_back({"Evidence", evidenceJson, false});
        WebhookSendRich("1sT-AntiDLL \xe2\x80\x94 violation", fields, 0xF1C40F);
    }

    if (doPunish)
        Punish(slot, steamId, reason);
}

static void DecayPoints()
{
    std::lock_guard<std::mutex> lock(g_StateMutex);
    for (auto it = g_States.begin(); it != g_States.end();)
    {
        PlayerState& st = it->second;
        if (st.punished) { ++it; continue; }
        st.points = std::max(0, st.points - g_Cfg.pointDecay);
        if (st.points == 0 && st.whTargetSlot < 0 && st.aimSnapEvidence == 0 && st.triggerEvidence == 0)
            it = g_States.erase(it);
        else
            ++it;
    }
}

//==============================================================================
// Detection: legacy DLL event-listener probe
//==============================================================================

static void RunDllDetection()
{
    for (int i = 0; i < 64; i++)
    {
        if (!IsValidHumanPlayer(i)) continue;
        uint64 steamId = g_pPlayers->GetSteamID64(i);

        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            auto it = g_States.find(i);
            if (it != g_States.end() && it->second.punished) continue;
        }

        IGameEventListener2* pListener = g_pPlayers->GetLegacyGameEventListener(i);
        if (!pListener) continue;

        for (auto& ev : g_vEvents)
        {
            if (g_pUtils->GetGameEventManager()->FindListener(pListener, ev.c_str()))
            {
                std::string evidence = "{\"event\":\"" + ev + "\"}";
                AddViolation(i, steamId, g_Cfg.pointsPerDetection,
                    std::string("Illegal event listener: ") + ev, DET_DLL_EVENT, evidence);
                break;
            }
        }
    }
}

//==============================================================================
// Detection: hidden-target tracking (evidence-based)
//==============================================================================

static void RunHiddenTargetTracking()
{
    if (!g_Cfg.whEnabled) return;
    if (!gpGlobals) return;
    if (IsWarmupPeriod() || IsFreezePeriod() || !g_RoundActive) return;

    double now = gpGlobals->curtime;

    for (int i = 0; i < 64; i++)
    {
        if (!IsValidHumanPlayer(i)) continue;
        if (!IsPlayerAlive(i)) continue;
        uint64 steamId = g_pPlayers->GetSteamID64(i);

        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            auto it = g_States.find(i);
            if (it != g_States.end() && it->second.punished) continue;
        }

        Vector eye = GetPlayerEyePosition(i);
        Vector forward = AnglesToForward(GetPlayerEyeAngles(i));
        int myTeam = GetPlayerTeam(i);

        if (IsZeroVector(eye) || myTeam <= 1) continue;

        int bestTarget = -1;
        float bestDot = -2.0f;
        float bestDist = 0.0f;
        for (int j = 0; j < 64; j++)
        {
            if (i == j) continue;
            if (!IsValidHumanPlayer(j)) continue;
            if (!IsPlayerAlive(j)) continue;

            int targetTeam = GetPlayerTeam(j);
            if (targetTeam == myTeam || targetTeam <= 1) continue;

            Vector tpos = GetPlayerPosition(j);
            tpos.z += 52.0f;
            Vector delta = VecSub(tpos, eye);
            float dist = VecLength(delta);
            if (dist > g_Cfg.whMaxDistance || dist < 1.0f) continue;

            float dot = VecDot(forward, VecNormalize(delta));
            if (dot > bestDot)
            {
                bestDot = dot;
                bestTarget = j;
                bestDist = dist;
            }
        }

        PlayerState& st = StateFor(i);

        if (st.whTargetSlot >= 0 && (now - st.whLastSample) > g_Cfg.whResetGap)
        {
            st.whTargetSlot = -1;
            st.whSampleCount = 0;
        }

        if (bestTarget < 0 || bestDot < g_Cfg.whDotThreshold)
            continue;

        if (g_Cfg.raytraceRequired && !RayTrace_IsAvailable())
            continue;

        if (RayTrace_IsAvailable())
        {
            bool visible = IsEnemyVisibleByRayTrace(i, bestTarget);
            if (visible)
            {
                if (st.whTargetSlot == bestTarget)
                {
                    st.whTargetSlot = -1;
                    st.whSampleCount = 0;
                }
                continue;
            }
        }

        if (st.whTargetSlot == bestTarget)
            st.whSampleCount++;
        else
        {
            st.whTargetSlot = bestTarget;
            st.whTrackStart = now;
            st.whSampleCount = 1;
        }
        st.whLastSample = now;

        double duration = now - st.whTrackStart;
        if (duration < g_Cfg.whTrackingThreshold || st.whSampleCount < g_Cfg.whMinSamples)
            continue;

        if (now - st.whLastViolationTime < g_Cfg.whCooldown)
            continue;
        st.whLastViolationTime = now;

        const char* szName = g_pPlayers->GetPlayerName(i);
        const char* szTargetName = g_pPlayers->GetPlayerName(bestTarget);
        char szSteamID64[64], szTargetSteamID64[64];
        g_SMAPI->Format(szSteamID64, sizeof(szSteamID64), "%llu", steamId);
        g_SMAPI->Format(szTargetSteamID64, sizeof(szTargetSteamID64), "%llu", g_pPlayers->GetSteamID64(bestTarget));

        const char* visMode = RayTrace_IsAvailable() ? "raytrace" : "angle_only";
        std::string evidence = "{\"dot\":" + std::to_string(bestDot) +
            ",\"distance\":" + std::to_string(bestDist) +
            ",\"samples\":" + std::to_string(st.whSampleCount) +
            ",\"duration\":" + std::to_string(duration) +
            ",\"target\":\"" + szTargetName +
            "\",\"target_steamid64\":\"" + szTargetSteamID64 +
            "\",\"visibility\":\"" + visMode + "\"}";

        st.whTargetSlot = -1;
        st.whSampleCount = 0;

        const std::string reason = "Suspicious hidden-target angle tracking";

        LogStructured(std::string("type=SUSPICION category=") + DetCategoryStr(DET_HIDDEN_TARGET) +
            " player=" + szName + " steamid=" + szSteamID64 +
            " target=" + szTargetName + " dot=" + std::to_string(bestDot) +
            " distance=" + std::to_string(bestDist) + " samples=" + std::to_string(st.whSampleCount) +
            " duration=" + std::to_string(duration) +
            " mode=" + (g_Cfg.whDebugOnly ? "debug_only" : "enforce"));

        if (g_Cfg.whDebugOnly)
        {
            auto fields = BuildServerFields();
            fields.push_back({"Player",    szName,       true});
            fields.push_back({"SteamID64", szSteamID64,  true});
            fields.push_back({"Target",    szTargetName,  true});
            fields.push_back({"Dot",       std::to_string(bestDot),  true});
            fields.push_back({"Distance",  std::to_string((int)bestDist) + "u", true});
            fields.push_back({"Mode",      "debug_only (no punishment)", false});
            WebhookSendRich("1sT-AntiDLL \xe2\x80\x94 hidden target evidence", fields, 0x3498DB);
        }
        else
        {
            AddViolation(i, steamId, g_Cfg.whViolationPoints, reason, DET_HIDDEN_TARGET, evidence);
        }
    }
}

//==============================================================================
// Detection: aim snap pattern (evidence-based)
//==============================================================================

static void RunAimPatternDetection()
{
    if (!g_Cfg.aimEnabled) return;
    if (!gpGlobals) return;
    if (IsWarmupPeriod() || IsFreezePeriod() || !g_RoundActive) return;

    double now = gpGlobals->curtime;

    for (int i = 0; i < 64; i++)
    {
        if (!IsValidHumanPlayer(i)) continue;
        if (!IsPlayerAlive(i)) continue;
        uint64 steamId = g_pPlayers->GetSteamID64(i);

        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            auto it = g_States.find(i);
            if (it != g_States.end() && it->second.punished) continue;
        }

        QAngle curAngles = GetPlayerEyeAngles(i);
        int curShots = GetPlayerShotsFired(i);

        PlayerState& st = StateFor(i);

        if (!st.hasPrevAngles)
        {
            st.prevAngles = curAngles;
            st.prevAngleTime = now;
            st.prevShotsFired = curShots;
            st.hasPrevAngles = true;
            continue;
        }

        float delta = AngleDelta(curAngles, st.prevAngles);
        bool firedShots = (curShots > st.prevShotsFired) && (curShots > 0);

        st.prevAngles = curAngles;
        st.prevAngleTime = now;
        st.prevShotsFired = curShots;

        if (delta < g_Cfg.aimSnapAngleThreshold || !firedShots)
            continue;

        Vector eye = GetPlayerEyePosition(i);
        Vector forward = AnglesToForward(curAngles);
        int myTeam = GetPlayerTeam(i);
        if (IsZeroVector(eye) || myTeam <= 1) continue;

        bool snapToEnemy = false;
        float snapDot = 0.0f;
        float snapDist = 0.0f;
        for (int j = 0; j < 64; j++)
        {
            if (i == j) continue;
            if (!IsValidHumanPlayer(j)) continue;
            if (!IsPlayerAlive(j)) continue;

            int targetTeam = GetPlayerTeam(j);
            if (targetTeam == myTeam || targetTeam <= 1) continue;

            Vector tpos = GetPlayerPosition(j);
            tpos.z += 52.0f;
            Vector d = VecSub(tpos, eye);
            float dist = VecLength(d);
            if (dist < 100.0f || dist > g_Cfg.whMaxDistance) continue;

            float dot = VecDot(forward, VecNormalize(d));
            if (dot > g_Cfg.aimSnapDotThreshold)
            {
                snapToEnemy = true;
                snapDot = dot;
                snapDist = dist;
                break;
            }
        }

        if (!snapToEnemy) continue;

        st.aimSnapEvidence++;

        if (st.aimSnapEvidence < g_Cfg.aimMinSamples)
            continue;
        if (now - st.aimLastViolationTime < g_Cfg.aimCooldown)
            continue;
        st.aimLastViolationTime = now;
        int evidenceCount = st.aimSnapEvidence;
        st.aimSnapEvidence = 0;

        const char* szName = g_pPlayers->GetPlayerName(i);
        char szSteamID64[64];
        g_SMAPI->Format(szSteamID64, sizeof(szSteamID64), "%llu", steamId);

        std::string evidence = "{\"snap_angle\":" + std::to_string(delta) +
            ",\"dot\":" + std::to_string(snapDot) +
            ",\"distance\":" + std::to_string(snapDist) +
            ",\"evidence_count\":" + std::to_string(evidenceCount) + "}";

        const std::string reason = "Suspicious snap aim pattern";

        LogStructured(std::string("type=SUSPICION category=") + DetCategoryStr(DET_AIM_SNAP) +
            " player=" + szName + " steamid=" + szSteamID64 +
            " snap_angle=" + std::to_string(delta) + " dot=" + std::to_string(snapDot) +
            " evidence=" + std::to_string(evidenceCount) +
            " mode=" + (g_Cfg.aimDebugOnly ? "debug_only" : "enforce"));

        if (g_Cfg.aimDebugOnly)
        {
            auto fields = BuildServerFields();
            fields.push_back({"Player",     szName,       true});
            fields.push_back({"SteamID64",  szSteamID64,  true});
            fields.push_back({"Snap Angle", std::to_string((int)delta) + "\xc2\xb0", true});
            fields.push_back({"Evidence",   std::to_string(evidenceCount) + " snaps", true});
            fields.push_back({"Mode",       "debug_only (no punishment)", false});
            WebhookSendRich("1sT-AntiDLL \xe2\x80\x94 aim snap evidence", fields, 0x9B59B6);
        }
        else
        {
            AddViolation(i, steamId, g_Cfg.aimPoints, reason, DET_AIM_SNAP, evidence);
        }
    }
}

//==============================================================================
// Detection: triggerbot reaction pattern (evidence-based, requires RayTrace)
//==============================================================================

static void RunTriggerDetection()
{
    if (!g_Cfg.triggerEnabled) return;
    if (!RayTrace_IsAvailable()) return;
    if (!gpGlobals) return;
    if (IsWarmupPeriod() || IsFreezePeriod() || !g_RoundActive) return;

    double now = gpGlobals->curtime;

    for (int i = 0; i < 64; i++)
    {
        if (!IsValidHumanPlayer(i)) continue;
        if (!IsPlayerAlive(i)) continue;
        uint64 steamId = g_pPlayers->GetSteamID64(i);

        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            auto it = g_States.find(i);
            if (it != g_States.end() && it->second.punished) continue;
        }

        int myTeam = GetPlayerTeam(i);
        if (myTeam <= 1) continue;

        int curShots = GetPlayerShotsFired(i);
        PlayerState& st = StateFor(i);
        bool firedThisTick = (curShots > st.triggerPrevShotsFired) && (curShots > 0);
        st.triggerPrevShotsFired = curShots;

        for (int j = 0; j < 64; j++)
        {
            if (i == j) continue;
            if (!IsValidHumanPlayer(j)) continue;
            if (!IsPlayerAlive(j)) continue;

            int targetTeam = GetPlayerTeam(j);
            if (targetTeam == myTeam || targetTeam <= 1) continue;

            Vector tpos = GetPlayerPosition(j);
            Vector eye = GetPlayerEyePosition(i);
            if (IsZeroVector(tpos) || IsZeroVector(eye)) continue;

            float dist = VecLength(VecSub(tpos, eye));
            if (dist < 100.0f || dist > g_Cfg.whMaxDistance) continue;

            bool visibleNow = IsEnemyVisibleByRayTrace(i, j);

            auto& tv = st.targetVis[j];
            if (visibleNow && !tv.wasVisible)
            {
                tv.changeTime = now;
            }

            if (firedThisTick && visibleNow && tv.changeTime > 0.0)
            {
                double reaction = now - tv.changeTime;
                if (reaction > 0.0 && reaction <= g_Cfg.triggerMaxReactionSec)
                {
                    st.triggerEvidence++;
                    tv.changeTime = 0.0;
                }
            }

            tv.wasVisible = visibleNow;
        }

        if (st.triggerEvidence < g_Cfg.triggerMinSamples)
            continue;
        if (now - st.triggerLastViolationTime < g_Cfg.triggerCooldown)
            continue;
        st.triggerLastViolationTime = now;
        int evidenceCount = st.triggerEvidence;
        st.triggerEvidence = 0;

        const char* szName = g_pPlayers->GetPlayerName(i);
        char szSteamID64[64];
        g_SMAPI->Format(szSteamID64, sizeof(szSteamID64), "%llu", steamId);

        std::string evidence = "{\"trigger_events\":" + std::to_string(evidenceCount) + "}";
        const std::string reason = "Suspicious triggerbot-like reaction pattern";

        LogStructured(std::string("type=SUSPICION category=") + DetCategoryStr(DET_TRIGGER) +
            " player=" + szName + " steamid=" + szSteamID64 +
            " evidence=" + std::to_string(evidenceCount) +
            " mode=" + (g_Cfg.triggerDebugOnly ? "debug_only" : "enforce"));

        if (g_Cfg.triggerDebugOnly)
        {
            auto fields = BuildServerFields();
            fields.push_back({"Player",    szName,       true});
            fields.push_back({"SteamID64", szSteamID64,  true});
            fields.push_back({"Events",    std::to_string(evidenceCount) + " trigger-like reactions", true});
            fields.push_back({"Mode",      "debug_only (no punishment)", false});
            WebhookSendRich("1sT-AntiDLL \xe2\x80\x94 trigger pattern evidence", fields, 0xE91E63);
        }
        else
        {
            AddViolation(i, steamId, g_Cfg.triggerPoints, reason, DET_TRIGGER, evidence);
        }
    }
}

//==============================================================================
// Detection: composite risk scoring
//==============================================================================

// Composite risk: finds players with evidence in multiple categories and adds bonus violation points.
static void ProcessCompositeViolations()
{
    if (!g_Cfg.compositeEnabled) return;
    if (!gpGlobals) return;

    struct Pending { int slot; uint64 steamId; std::string evidence; };
    std::vector<Pending> pending;

    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        for (auto& kv : g_States)
        {
            int slot = kv.first;
            PlayerState& st = kv.second;
            if (st.punished) continue;
            if (!IsValidHumanPlayer(slot)) continue;

            int categoriesWithEvidence = 0;
            for (int c = 0; c < DET_COUNT; c++)
            {
                if (c == DET_COMPOSITE) continue;
                if (st.evidenceCount[c] > 0) categoriesWithEvidence++;
            }

            if (categoriesWithEvidence < g_Cfg.compositeMinCategories)
                continue;

            std::string evidence = "{\"categories\":" + std::to_string(categoriesWithEvidence) +
                ",\"dll\":" + std::to_string(st.evidenceCount[DET_DLL_EVENT]) +
                ",\"hidden_target\":" + std::to_string(st.evidenceCount[DET_HIDDEN_TARGET]) +
                ",\"aim_snap\":" + std::to_string(st.evidenceCount[DET_AIM_SNAP]) +
                ",\"trigger\":" + std::to_string(st.evidenceCount[DET_TRIGGER]) + "}";

            pending.push_back({slot, st.steamId, evidence});

            for (int c = 0; c < DET_COUNT; c++)
            {
                if (c != DET_COMPOSITE) st.evidenceCount[c] = 0;
            }
        }
    }

    for (auto& p : pending)
    {
        AddViolation(p.slot, p.steamId, g_Cfg.compositeBonusPoints,
            "Composite risk: multiple detection categories triggered", DET_COMPOSITE, p.evidence);
    }
}

//==============================================================================
// Admin commands — native ConCommands (server console / rcon)
//==============================================================================

CON_COMMAND_F(antidll_reload, "Reload 1sT-AntiDLL config and events", FCVAR_GAMEDLL | FCVAR_LINKED_CONCOMMAND)
{
    bool ok = LoadConfig();
    LoadEventFile();
    META_CONPRINTF("[1sT-AntiDLL] config reload %s\n", ok ? "OK" : "FAILED");
}

CON_COMMAND_F(antidll_status, "Show 1sT-AntiDLL status", FCVAR_GAMEDLL | FCVAR_LINKED_CONCOMMAND)
{
    std::lock_guard<std::mutex> lock(g_StateMutex);
    META_CONPRINTF("[1sT-AntiDLL] v%s\n", g_AntiDLL.GetVersion());
    META_CONPRINTF("  Server: %s (%s)\n", GetServerDisplayName().c_str(), GetServerAddress().c_str());
    META_CONPRINTF("  Map: %s  Players: %s\n", g_MapName.c_str(), GetPlayerCountStr().c_str());
    META_CONPRINTF("  MySQL: %s  RayTrace: %s\n",
        g_MySQL.IsConnected() ? "connected" : (g_MySQL.IsLoaded() ? "loaded" : "disabled"),
        RayTrace_IsAvailable() ? "loaded" : "missing");
    META_CONPRINTF("  DLL Detection: on  WH Detection: %s  Aim: %s  Trigger: %s\n",
        g_Cfg.whEnabled ? "on" : "off",
        g_Cfg.aimEnabled ? "on" : "off",
        g_Cfg.triggerEnabled ? "on" : "off");
    META_CONPRINTF("  Debug-only: wh=%d aim=%d trigger=%d\n",
        g_Cfg.whDebugOnly, g_Cfg.aimDebugOnly, g_Cfg.triggerDebugOnly);
    META_CONPRINTF("  Webhook: %s  Queue running: %d\n",
        g_Cfg.webhookEnabled ? "on" : "off", g_Webhook.IsRunning());
    META_CONPRINTF("  Violation threshold: %d  Tracked players: %zu\n",
        g_Cfg.violationThreshold, g_States.size());
}

CON_COMMAND_F(antidll_player, "Show player state: antidll_player <slot>", FCVAR_GAMEDLL | FCVAR_LINKED_CONCOMMAND)
{
    if (args.ArgC() < 2) { META_CONPRINTF("[1sT-AntiDLL] usage: antidll_player <slot>\n"); return; }
    int target = atoi(args[1]);
    std::lock_guard<std::mutex> lock(g_StateMutex);
    auto it = g_States.find(target);
    if (it == g_States.end())
    {
        META_CONPRINTF("[1sT-AntiDLL] no state for slot %d\n", target);
        return;
    }
    const PlayerState& st = it->second;
    META_CONPRINTF("[1sT-AntiDLL] slot=%d name=%s steamid=%llu points=%d/%d punished=%d\n",
        target, st.name.c_str(), st.steamId, st.points, g_Cfg.violationThreshold, st.punished);
    META_CONPRINTF("  Evidence: dll=%d wh=%d aim=%d trigger=%d composite=%d\n",
        st.evidenceCount[DET_DLL_EVENT], st.evidenceCount[DET_HIDDEN_TARGET],
        st.evidenceCount[DET_AIM_SNAP], st.evidenceCount[DET_TRIGGER],
        st.evidenceCount[DET_COMPOSITE]);
    META_CONPRINTF("  WH tracking: target=%d samples=%d  Aim snaps: %d  Trigger events: %d\n",
        st.whTargetSlot, st.whSampleCount, st.aimSnapEvidence, st.triggerEvidence);
}

CON_COMMAND_F(antidll_reset, "Reset player state: antidll_reset <slot>", FCVAR_GAMEDLL | FCVAR_LINKED_CONCOMMAND)
{
    if (args.ArgC() < 2) { META_CONPRINTF("[1sT-AntiDLL] usage: antidll_reset <slot>\n"); return; }
    int target = atoi(args[1]);
    ResetPlayer(target);
    META_CONPRINTF("[1sT-AntiDLL] reset state for slot %d\n", target);
}

CON_COMMAND_F(antidll_test_webhook, "Queue a test Discord webhook", FCVAR_GAMEDLL | FCVAR_LINKED_CONCOMMAND)
{
    auto fields = BuildServerFields();
    fields.push_back({"Version", g_AntiDLL.GetVersion(), true});
    fields.push_back({"Test", "antidll_test_webhook", true});
    WebhookSendRich("1sT-AntiDLL \xe2\x80\x94 test", fields, 0x2ECC71);
    META_CONPRINTF("[1sT-AntiDLL] test webhook queued (enabled=%d)\n", g_Cfg.webhookEnabled);
}

CON_COMMAND_F(antidll_debug, "Toggle debug mode: antidll_debug <0|1>", FCVAR_GAMEDLL | FCVAR_LINKED_CONCOMMAND)
{
    if (args.ArgC() < 2) { META_CONPRINTF("[1sT-AntiDLL] debug_mode=%d\n", g_Cfg.debugMode); return; }
    g_Cfg.debugMode = (atoi(args[1]) != 0);
    META_CONPRINTF("[1sT-AntiDLL] debug_mode=%d\n", g_Cfg.debugMode);
}

CON_COMMAND_F(antidll_server_identity, "Show server identity info", FCVAR_GAMEDLL | FCVAR_LINKED_CONCOMMAND)
{
    ServerIdentity id = GetServerIdentity();
    META_CONPRINTF("[1sT-AntiDLL] Server Identity:\n");
    META_CONPRINTF("  Detected address: %s:%d\n", g_DetectedIp.c_str(), g_DetectedPort);
    META_CONPRINTF("  Server name: %s\n", id.serverName.c_str());
    META_CONPRINTF("  Group: %s  Region: %s\n", id.serverGroup.c_str(), id.region.c_str());
    META_CONPRINTF("  MySQL connected: %s  MySQL matched: %s\n",
        id.mysqlConnected ? "yes" : "no",
        id.mysqlMatched ? "yes" : "no");
    if (!id.mysqlMatched && id.mysqlConnected)
        META_CONPRINTF("  (no row in %s for %s:%d)\n",
            g_Cfg.identityTable.c_str(), g_DetectedIp.c_str(), g_DetectedPort);
}

CON_COMMAND_F(antidll_mysql_status, "Show MySQL connection status", FCVAR_GAMEDLL | FCVAR_LINKED_CONCOMMAND)
{
    META_CONPRINTF("[1sT-AntiDLL] Database:\n");
    META_CONPRINTF("  Enabled: %d  Config: '%s'\n", g_Cfg.dbEnabled, g_Cfg.dbConfigName.c_str());
    META_CONPRINTF("  Library loaded: %d  Connected: %d\n", g_MySQL.IsLoaded(), g_MySQL.IsConnected());
    if (g_Cfg.dbEnabled)
        META_CONPRINTF("  Host: %s:%d  DB: %s  User: %s\n",
            g_Cfg.mysql.host.c_str(), g_Cfg.mysql.port,
            g_Cfg.mysql.database.c_str(), g_Cfg.mysql.user.c_str());
    META_CONPRINTF("  Identity table: %s  Detection table: %s\n",
        g_Cfg.identityTable.c_str(), g_Cfg.detectionTable.c_str());
}

//==============================================================================
// Lifecycle
//==============================================================================

static void OnClientAuth(int slot, uint64 steamId)
{
    ResetPlayer(slot);
}

bool AntiDLL::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();

    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

    g_SMAPI->AddListener(this, this);
    return true;
}

bool AntiDLL::Unload(char* error, size_t maxlen)
{
    g_Webhook.Stop();
    g_MySQL.Stop();
    ConVar_Unregister();
    return true;
}

void AntiDLL::AllPluginsLoaded()
{
    char err[64];
    int ret;

    g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
    if (ret == META_IFACE_FAILED)
    {
        g_SMAPI->Format(err, sizeof(err), "Missing Utils system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), err);
        std::string buf = "meta unload " + std::to_string(g_PLID);
        engine->ServerCommand(buf.c_str());
        return;
    }
    g_pPlayers = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
    if (ret == META_IFACE_FAILED)
    {
        g_SMAPI->Format(err, sizeof(err), "Missing Players system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), err);
        std::string buf = "meta unload " + std::to_string(g_PLID);
        engine->ServerCommand(buf.c_str());
        return;
    }

    if (FILE* mk = popen("mkdir -p addons/AntiDLL/logs", "r"))
        pclose(mk);

    LoadEventFile();
    LoadConfig();

    // Auto-detect server IP:port from process command line
    DetectServerAddress();
    META_CONPRINTF("[1sT-AntiDLL] Detected server address: %s:%d\n", g_DetectedIp.c_str(), g_DetectedPort);

    // Ray-Trace (optional)
    if (!RayTrace_Load())
    {
        if (g_Cfg.webhookEnabled)
            WebhookSend("1sT-AntiDLL \xe2\x80\x94 Ray-Trace missing",
                "Hidden-target visibility checks and trigger detection are disabled.", 0xE67E22);
    }

    // MySQL (optional) — server identity is resolved from DB, not config
    if (g_Cfg.dbEnabled)
    {
        if (g_MySQL.Init(g_Cfg.mysql))
        {
            g_MySQL.RequestIdentityRefresh(g_DetectedIp, g_DetectedPort);
        }
    }

    // Webhook
    if (g_Cfg.webhookEnabled && !g_Cfg.webhookUrl.empty())
        g_Webhook.Start(g_Cfg.webhookUrl);

    g_pPlayers->HookOnClientAuthorized(g_PLID, OnClientAuth);
    g_pUtils->StartupServer(g_PLID, StartupServer);

    // Round state hooks
    g_pUtils->HookEvent(g_PLID, "round_start", [](const char*, IGameEvent*, bool) {
        g_RoundActive = true;
    });
    g_pUtils->HookEvent(g_PLID, "round_end", [](const char*, IGameEvent*, bool) {
        g_RoundActive = false;
    });

    // Plugin loaded webhook
    if (g_Cfg.webhookPluginLoad)
    {
        auto fields = BuildServerFields();
        fields.push_back({"Version",            GetVersion(),               true});
        fields.push_back({"MySQL",              g_MySQL.IsConnected() ? "connected" : (g_Cfg.dbEnabled ? "connecting..." : "disabled"), true});
        fields.push_back({"RayTrace",           RayTrace_IsAvailable() ? "loaded" : "missing", true});
        fields.push_back({"WH Detection",       g_Cfg.whEnabled ? "on" : "off", true});
        fields.push_back({"Aim Detection",      g_Cfg.aimEnabled ? "on" : "off", true});
        fields.push_back({"Trigger Detection",  g_Cfg.triggerEnabled ? "on" : "off", true});
        fields.push_back({"Debug Only",         (g_Cfg.whDebugOnly || g_Cfg.aimDebugOnly || g_Cfg.triggerDebugOnly) ? "yes" : "no", true});
        fields.push_back({"Punishment",         g_Cfg.punishType ? "ban" : "kick", true});
        fields.push_back({"Threshold",          std::to_string(g_Cfg.violationThreshold), true});
        WebhookSendRich("1sT-AntiDLL \xe2\x80\x94 plugin loaded", fields, 0x2ECC71);
    }

    // Console startup banner
    META_CONPRINTF("[1sT-AntiDLL] ------------------------------------------------------------\n");
    META_CONPRINTF("[1sT-AntiDLL] ------------------------------------------------------------\n");
    META_CONPRINTF("[1sT-AntiDLL] --------------------ANTIDLL LOADED-------------------------\n");
    META_CONPRINTF("[1sT-AntiDLL] ------------------------------------------------------------\n");
    META_CONPRINTF("[1sT-AntiDLL] ------------------------------------------------------------\n");
    META_CONPRINTF("[1sT-AntiDLL] Version:      %s\n", GetVersion());
    META_CONPRINTF("[1sT-AntiDLL] Server:       %s\n", GetServerDisplayName().c_str());
    META_CONPRINTF("[1sT-AntiDLL] Address:      %s\n", GetServerAddress().c_str());
    META_CONPRINTF("[1sT-AntiDLL] Map:          %s\n", g_MapName.c_str());
    META_CONPRINTF("[1sT-AntiDLL] MySQL:        %s\n",
        g_MySQL.IsConnected() ? "connected" : (g_Cfg.dbEnabled ? "loading" : "disabled"));
    META_CONPRINTF("[1sT-AntiDLL] RayTrace:     %s\n", RayTrace_IsAvailable() ? "loaded" : "missing");
    META_CONPRINTF("[1sT-AntiDLL] WH Detection: %s\n", g_Cfg.whEnabled ? "on" : "off");
    META_CONPRINTF("[1sT-AntiDLL] Debug Only:   %s\n",
        (g_Cfg.whDebugOnly || g_Cfg.aimDebugOnly || g_Cfg.triggerDebugOnly) ? "on" : "off");

    // --- Timers ---

    // Point decay
    g_pUtils->CreateTimer(g_Cfg.decayInterval, []() {
        DecayPoints();
        return g_Cfg.decayInterval;
    });

    // DLL event-listener probe
    g_pUtils->CreateTimer(g_Cfg.dllInterval, []() {
        if (g_Cfg.enabled)
            RunDllDetection();
        return g_Cfg.dllInterval;
    });

    // Hidden-target tracking
    if (g_Cfg.whEnabled)
    {
        g_pUtils->CreateTimer(g_Cfg.whCheckInterval, []() {
            if (g_Cfg.enabled)
                RunHiddenTargetTracking();
            return g_Cfg.whCheckInterval;
        });
    }

    // Aim + trigger pattern detection
    if (g_Cfg.aimEnabled || g_Cfg.triggerEnabled)
    {
        g_pUtils->CreateTimer(g_Cfg.aimCheckInterval, []() {
            if (g_Cfg.enabled)
            {
                RunAimPatternDetection();
                RunTriggerDetection();
            }
            return g_Cfg.aimCheckInterval;
        });
    }

    // Composite risk check (slower interval)
    if (g_Cfg.compositeEnabled)
    {
        g_pUtils->CreateTimer(10.0f, []() {
            if (g_Cfg.enabled)
                ProcessCompositeViolations();
            return 10.0f;
        });
    }

    // MySQL server identity refresh (periodic re-query)
    if (g_Cfg.dbEnabled && g_Cfg.identityEnabled)
    {
        g_pUtils->CreateTimer(g_Cfg.identityRefreshInterval, []() {
            if (g_MySQL.IsLoaded())
                g_MySQL.RequestIdentityRefresh(g_DetectedIp, g_DetectedPort);
            return g_Cfg.identityRefreshInterval;
        });
    }
}

//==============================================================================
// Plugin metadata
//==============================================================================

const char* AntiDLL::GetLicense()     { return "GPL"; }
const char* AntiDLL::GetVersion()     { return ANTIDLL_VERSION; }
const char* AntiDLL::GetDate()        { return __DATE__; }
const char* AntiDLL::GetLogTag()      { return "1sT-AntiDLL"; }
const char* AntiDLL::GetAuthor()      { return "Snaximusss+"; }
const char* AntiDLL::GetDescription() { return "1sT-AntiDLL - Anti-DLL injection & behavioral detection for CS2"; }
const char* AntiDLL::GetName()        { return "1sT-AntiDLL"; }
const char* AntiDLL::GetURL()         { return "https://discord.gg/g798xERK5Y"; }
