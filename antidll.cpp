#include <stdio.h>
#include "antidll.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"
#include "CCSPlayerController.h"
#include "raytrace.h"
#include "webhook_queue.h"
#include <igameevents.h>
#include <fstream>
#include <ctime>
#include <algorithm>

//==============================================================================
// AntiDLLCore — globals & SDK plumbing
//==============================================================================

AntiDLL g_AntiDLL;
PLUGIN_EXPOSE(AntiDLL, g_AntiDLL);
IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars* gpGlobals = nullptr;

IUtilsApi* g_pUtils = nullptr;
IPlayersApi* g_pPlayers = nullptr;

//==============================================================================
// ConfigManager
//==============================================================================

struct Config
{
    // --- Core ---
    bool  enabled        = true;
    bool  debugMode      = false;
    bool  ignoreBots     = true;
    bool  ignoreAdmins   = true;   // TODO: no admin API exposed by IPlayersApi; currently a no-op

    // --- Violation Points ---
    int   violationThreshold = 300;
    int   pointsPerDetection = 50;   // legacy DLL detection points
    int   pointDecay         = 10;
    float decayInterval      = 30.0f;

    // --- RayTrace Visibility ---
    bool  raytraceRequired  = true;
    bool  raytraceDebugBeam = false;

    // --- Suspicious Hidden-Target Tracking ---
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

    // --- Legacy DLL detection (event-listener probe) ---
    int          punishType   = 0;
    std::string  punishCommand;
    std::string  chatMessage;
    float        dllInterval  = 5.0f;

    // --- Logging ---
    bool         logs    = true;
    std::string  logFile = "addons/AntiDLL/logs/antidll.log";

    // --- Discord ---
    bool         webhookEnabled  = false;
    std::string  webhookUrl;
    int          webhookMinPoints = 25;
};

static Config g_Cfg;
static std::vector<std::string> g_vEvents;      // legacy event-listener blacklist
static WebhookQueue g_Webhook;

//==============================================================================
// PlayerStateManager
//==============================================================================

struct PlayerState
{
    uint64_t    steamId = 0;
    std::string name;
    int         points = 0;
    double      lastViolationTime = 0.0;
    bool        punished = false;
    std::vector<std::string> history;   // recent suspicion reasons

    // Hidden-target tracking working state
    int    whTargetSlot = -1;
    double whTrackStart = 0.0;
    double whLastSample = 0.0;
    int    whSampleCount = 0;
    double whLastViolationTime = 0.0;   // cooldown anchor
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
// Utils — math & entity access
//==============================================================================

static CEntityInstance* GetEntity(int slot)
{
    if (!g_pGameEntitySystem) return nullptr;
    return g_pGameEntitySystem->GetEntityInstance(CEntityIndex(slot + 1));
}

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

static CCSPlayerPawn* GetPlayerPawn(int slot)
{
    CCSPlayerController* ctrl = CCSPlayerController::FromSlot(slot);
    if (!ctrl) return nullptr;
    return ctrl->GetPlayerPawn();
}

static Vector GetPlayerPosition(int slot)
{
    CCSPlayerPawn* pawn = GetPlayerPawn(slot);
    if (!pawn) return Vector(0, 0, 0);
    return pawn->GetAbsOrigin();
}

static Vector GetPlayerEyePosition(int slot)
{
    CCSPlayerPawn* pawn = GetPlayerPawn(slot);
    if (!pawn) return Vector(0, 0, 0);
    return pawn->GetEyePosition();
}

static QAngle GetPlayerEyeAngles(int slot)
{
    CCSPlayerPawn* pawn = GetPlayerPawn(slot);
    if (!pawn) return QAngle(0, 0, 0);
    return pawn->m_angEyeAngles();
}

static int GetPlayerTeam(int slot)
{
    CCSPlayerPawn* pawn = GetPlayerPawn(slot);
    if (!pawn) return 0;
    return pawn->m_iTeamNum();
}

static bool IsPlayerAlive(int slot)
{
    CCSPlayerPawn* pawn = GetPlayerPawn(slot);
    if (!pawn) return false;
    return pawn->IsAlive();
}

static bool IsValidHumanPlayer(int slot)
{
    if (slot < 0 || slot >= 64) return false;
    if (!g_pPlayers) return false;
    if (g_pPlayers->IsFakeClient(slot)) return false;   // covers ignore_bots
    if (!g_pPlayers->IsInGame(slot)) return false;
    if (g_pPlayers->GetSteamID64(slot) == 0) return false;
    return true;
}

// Body sample points used for visibility tracing (head / chest / pelvis; feet optional).
static std::vector<Vector> GetPlayerBodyPoints(int slot)
{
    Vector o = GetPlayerPosition(slot);
    return {
        Vector(o.x, o.y, o.z + 72.0f), // head
        Vector(o.x, o.y, o.z + 52.0f), // chest
        Vector(o.x, o.y, o.z + 36.0f), // pelvis
    };
}

//==============================================================================
// RayTraceVisibility — high-level enemy visibility
//==============================================================================

// Returns true if ANY checked body point of the enemy is visible from the viewer's eye.
// Fail-safe: if Ray-Trace or entity data is missing, returns true (visible) so we never flag.
static bool IsEnemyVisibleByRayTrace(int viewerSlot, int enemySlot)
{
    if (!RayTrace_IsAvailable())
        return true;

    CEntityInstance* viewer = GetEntity(viewerSlot);
    if (!viewer)
        return true;

    Vector eye = GetPlayerEyePosition(viewerSlot);
    Vector origin = GetPlayerPosition(enemySlot);
    if (IsZeroVector(eye) || IsZeroVector(origin))
        return true;

    const Vector points[3] = {
        Vector(origin.x, origin.y, origin.z + 72.0f), // head
        Vector(origin.x, origin.y, origin.z + 52.0f), // chest
        Vector(origin.x, origin.y, origin.z + 36.0f), // pelvis
    };

    for (const Vector& p : points)
    {
        if (!RayTrace_LineBlockedByWorld(eye, p, viewer))
            return true; // a clear line exists -> enemy is visible
    }
    return false; // every checked point blocked by world geometry -> hidden
}

//==============================================================================
// Logger — structured, single-line records
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

static std::string GetMapName()
{
    // TODO: read gpGlobals/engine map name once the exact CS2 field is confirmed.
    return "unknown";
}

static std::string GetServerName()
{
    // TODO: read the "hostname" cvar once ConVar value access is wired.
    return "unknown";
}

static void LogStructured(const std::string& line)
{
    if (!g_Cfg.logs)
        return;

    std::string full = "[" + NowString() + "] " + line;

    // Best-effort write to the configured file; fall back to the Utils logger.
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
// DiscordWebhookQueue helpers
//==============================================================================

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

static void WebhookSend(const std::string& title, const std::string& description, int color)
{
    if (!g_Cfg.webhookEnabled || g_Cfg.webhookUrl.empty())
        return;

    std::string payload = "{\"embeds\":[{\"title\":\"" + JsonEscape(title) +
        "\",\"description\":\"" + JsonEscape(description) +
        "\",\"color\":" + std::to_string(color) + "}]}";
    g_Webhook.Enqueue(payload);
}

//==============================================================================
// ConfigManager — load with safety clamps
//==============================================================================

// Required by SchemaEntity (schemasystem.cpp calls GameEntitySystem() by name).
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

static bool LoadConfig()
{
    KeyValues* pKV = new KeyValues("Config");
    if (!pKV->LoadFromFile(g_pFullFileSystem, "addons/configs/AntiDLL/settings.ini"))
    {
        g_pUtils->ErrorLog("[%s] Failed to load config addons/configs/AntiDLL/settings.ini", g_PLAPI->GetLogTag());
        WebhookSend("1sT-AntiDLL", "config reload failed", 0xE67E22);
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

    // RayTrace
    c.raytraceRequired  = pKV->GetBool("raytrace_required", true);
    c.raytraceDebugBeam = pKV->GetBool("raytrace_debug_beam", false);

    // WH tracking
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

    // Legacy DLL detection
    c.punishType    = pKV->GetInt("punish_type", 0);
    c.punishCommand = pKV->GetString("punish_command", "");
    c.chatMessage   = pKV->GetString("chat_message", "");
    c.dllInterval   = pKV->GetFloat("interval", 5.0f);

    // Logging
    c.logs    = pKV->GetBool("logs", true);
    c.logFile = pKV->GetString("log_file", "addons/AntiDLL/logs/antidll.log");

    // Discord
    c.webhookEnabled   = pKV->GetBool("webhook_enabled", false);
    c.webhookUrl       = pKV->GetString("webhook_url", "");
    c.webhookMinPoints = pKV->GetInt("webhook_min_points", 25);

    // --- Safety clamps ---
    if (c.whCheckInterval    < 0.25f) c.whCheckInterval = 0.25f;
    if (c.whTrackingThreshold < 2.0f) c.whTrackingThreshold = 2.0f;
    if (c.whDotThreshold     < 0.98f) c.whDotThreshold = 0.98f;
    if (c.whViolationPoints  > 50)    c.whViolationPoints = 50;
    if (c.violationThreshold < 100)   c.violationThreshold = 100;
    if (c.whCooldown         < 5.0f)  c.whCooldown = 5.0f;
    if (c.decayInterval      < 1.0f)  c.decayInterval = 1.0f;
    if (c.whMinSamples       < 1)     c.whMinSamples = 1;

    g_Cfg = c;
    RayTrace_SetDebugBeam(g_Cfg.raytraceDebugBeam);
    return true;
}

//==============================================================================
// ViolationManager
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
    WebhookSend("1sT-AntiDLL — punishment executed",
        std::string("**Player:** ") + szName + "\\n**SteamID64:** " + szSteamID64 + "\\n**Reason:** " + reason,
        0xFF0000);

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

// Adds violation points and punishes only if the running total crosses the threshold.
static void AddViolation(int slot, uint64 steamId, int points, const std::string& reason)
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

        if (st.points >= g_Cfg.violationThreshold && !st.punished)
        {
            st.punished = true;
            doPunish = true;
        }
    }

    char szSteamID64[64];
    g_SMAPI->Format(szSteamID64, sizeof(szSteamID64), "%llu", steamId);
    LogStructured(std::string("type=VIOLATION player=") + g_pPlayers->GetPlayerName(slot) +
        " steamid=" + szSteamID64 + " reason=\"" + reason + "\"" +
        " points_before=" + std::to_string(before) + " points_after=" + std::to_string(after));

    if (points >= g_Cfg.webhookMinPoints)
        WebhookSend("1sT-AntiDLL — violation",
            std::string("**Player:** ") + g_pPlayers->GetPlayerName(slot) + "\\n**SteamID64:** " + szSteamID64 +
            "\\n**Reason:** " + reason + "\\n**Points:** " + std::to_string(after) + "/" + std::to_string(g_Cfg.violationThreshold),
            0xF1C40F);

    if (doPunish)
    {
        WebhookSend("1sT-AntiDLL — threshold reached",
            std::string("**Player:** ") + g_pPlayers->GetPlayerName(slot) + " reached " + std::to_string(after) + " points",
            0xFF0000);
        Punish(slot, steamId, reason);
    }
}

static void DecayPoints()
{
    std::lock_guard<std::mutex> lock(g_StateMutex);
    for (auto it = g_States.begin(); it != g_States.end();)
    {
        PlayerState& st = it->second;
        if (st.punished) { ++it; continue; }
        st.points = std::max(0, st.points - g_Cfg.pointDecay);
        if (st.points == 0 && st.whTargetSlot < 0)
            it = g_States.erase(it);
        else
            ++it;
    }
}

//==============================================================================
// DetectionManager — legacy DLL probe (unchanged behavior) + hidden-target tracking
//==============================================================================

// Original event-listener DLL detection. Kept intact; only the points routing changed.
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
                AddViolation(i, steamId, g_Cfg.pointsPerDetection, std::string("Illegal event listener: ") + ev);
                break;
            }
        }
    }
}

// SuspiciousAngleTracker + RayTraceVisibility: evidence-based hidden-target tracking.
static void RunHiddenTargetTracking()
{
    if (!g_Cfg.whEnabled) return;
    if (!gpGlobals) return;
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
            tpos.z += 52.0f; // chest
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

        // Expire stale tracking if too long since the last positive sample.
        if (st.whTargetSlot >= 0 && (now - st.whLastSample) > g_Cfg.whResetGap)
        {
            st.whTargetSlot = -1;
            st.whSampleCount = 0;
        }

        // No candidate inside the crosshair cone -> nothing to evaluate.
        if (bestTarget < 0 || bestDot < g_Cfg.whDotThreshold)
            continue;

        // Step 2: visibility check.
        if (g_Cfg.raytraceRequired && !RayTrace_IsAvailable())
            continue; // cannot verify visibility -> never flag (fail-safe)

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
        // Without Ray-Trace (and raytrace_required=0): angle-only mode.

        // Step 3: hidden target under the crosshair — accumulate evidence.
        if (st.whTargetSlot == bestTarget)
        {
            st.whSampleCount++;
        }
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

        // Cooldown between evidence events for the same player.
        if (now - st.whLastViolationTime < g_Cfg.whCooldown)
            continue;
        st.whLastViolationTime = now;

        const char* szName = g_pPlayers->GetPlayerName(i);
        const char* szTargetName = g_pPlayers->GetPlayerName(bestTarget);
        char szSteamID64[64], szTargetSteamID64[64];
        g_SMAPI->Format(szSteamID64, sizeof(szSteamID64), "%llu", steamId);
        g_SMAPI->Format(szTargetSteamID64, sizeof(szTargetSteamID64), "%llu", g_pPlayers->GetSteamID64(bestTarget));

        const std::string reason = "Suspicious hidden-target angle tracking";
        const char* visMode = RayTrace_IsAvailable() ? "raytrace" : "angle_only";
        LogStructured(std::string("type=SUSPICION map=") + GetMapName() + " server=" + GetServerName() +
            " player=" + szName + " steamid=" + szSteamID64 +
            " target=" + szTargetName + " target_steamid=" + szTargetSteamID64 +
            " dot=" + std::to_string(bestDot) + " distance=" + std::to_string(bestDist) +
            " visibility=" + visMode + " samples=" + std::to_string(st.whSampleCount) +
            " duration=" + std::to_string(duration) +
            " mode=" + (g_Cfg.whDebugOnly ? "debug_only" : "enforce"));

        // Reset the tracking window so we gather fresh evidence next time.
        st.whTargetSlot = -1;
        st.whSampleCount = 0;

        if (g_Cfg.whDebugOnly)
        {
            // Debug-only: never punish. Log + optional webhook for review.
            WebhookSend("1sT-AntiDLL — hidden target tracking evidence",
                std::string("**Player:** ") + szName + "\\n**SteamID64:** " + szSteamID64 +
                "\\n**Target:** " + szTargetName + "\\n**Dot:** " + std::to_string(bestDot) +
                "\\n**Distance:** " + std::to_string(bestDist) + "\\n*(debug_only — no punishment)*",
                0x3498DB);
        }
        else
        {
            AddViolation(i, steamId, g_Cfg.whViolationPoints, reason);
        }
    }
}

//==============================================================================
// AdminCommands (console-only — server console / rcon)
//==============================================================================

static int ParseSlotArg(const char* content)
{
    if (!content) return -1;
    return atoi(content);
}

static void RegisterAdminCommands()
{
    g_pUtils->RegCommand(g_PLID, {"antidll_reload"}, {}, [](int slot, const char* content) -> bool {
        bool ok = LoadConfig();
        LoadEventFile();
        g_pUtils->PrintToConsole(slot, "[1sT-AntiDLL] config reload %s\n", ok ? "OK" : "FAILED");
        return true;
    });

    g_pUtils->RegCommand(g_PLID, {"antidll_status"}, {}, [](int slot, const char* content) -> bool {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        g_pUtils->PrintToConsole(slot,
            "[1sT-AntiDLL] enabled=%d wh=%d debug_only=%d raytrace=%d raytrace_required=%d vis_mode=%s tracked=%zu\n",
            g_Cfg.enabled, g_Cfg.whEnabled, g_Cfg.whDebugOnly, RayTrace_IsAvailable(), g_Cfg.raytraceRequired,
            RayTrace_IsAvailable() ? "raytrace" : (g_Cfg.raytraceRequired ? "disabled" : "angle_only"),
            g_States.size());
        return true;
    });

    g_pUtils->RegCommand(g_PLID, {"antidll_player"}, {}, [](int slot, const char* content) -> bool {
        int target = ParseSlotArg(content);
        std::lock_guard<std::mutex> lock(g_StateMutex);
        auto it = g_States.find(target);
        if (it == g_States.end())
        {
            g_pUtils->PrintToConsole(slot, "[1sT-AntiDLL] no state for slot %d\n", target);
            return true;
        }
        const PlayerState& st = it->second;
        g_pUtils->PrintToConsole(slot,
            "[1sT-AntiDLL] slot=%d name=%s points=%d punished=%d history=%zu\n",
            target, st.name.c_str(), st.points, st.punished, st.history.size());
        return true;
    });

    g_pUtils->RegCommand(g_PLID, {"antidll_reset"}, {}, [](int slot, const char* content) -> bool {
        int target = ParseSlotArg(content);
        ResetPlayer(target);
        g_pUtils->PrintToConsole(slot, "[1sT-AntiDLL] reset state for slot %d\n", target);
        return true;
    });

    g_pUtils->RegCommand(g_PLID, {"antidll_test_webhook"}, {}, [](int slot, const char* content) -> bool {
        WebhookSend("1sT-AntiDLL — test", "Test webhook from antidll_test_webhook", 0x2ECC71);
        g_pUtils->PrintToConsole(slot, "[1sT-AntiDLL] test webhook queued (enabled=%d)\n", g_Cfg.webhookEnabled);
        return true;
    });

    g_pUtils->RegCommand(g_PLID, {"antidll_debug"}, {}, [](int slot, const char* content) -> bool {
        g_Cfg.debugMode = (ParseSlotArg(content) != 0);
        g_pUtils->PrintToConsole(slot, "[1sT-AntiDLL] debug_mode=%d\n", g_Cfg.debugMode);
        return true;
    });
}

//==============================================================================
// Lifecycle
//==============================================================================

static void OnClientAuth(int slot, uint64 steamId)
{
    ResetPlayer(slot); // clean slate on (re)connect
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

    // Make sure the log directory exists (best-effort, once).
    if (FILE* mk = popen("mkdir -p addons/AntiDLL/logs", "r"))
        pclose(mk);

    LoadEventFile();
    LoadConfig();

    // Ray-Trace is optional; if missing, hidden-target visibility stays disabled.
    if (!RayTrace_Load())
        WebhookSend("1sT-AntiDLL — Ray-Trace missing", "Hidden-target visibility checks are disabled.", 0xE67E22);

    if (g_Cfg.webhookEnabled && !g_Cfg.webhookUrl.empty())
        g_Webhook.Start(g_Cfg.webhookUrl);

    g_pPlayers->HookOnClientAuthorized(g_PLID, OnClientAuth);
    g_pUtils->StartupServer(g_PLID, StartupServer);
    RegisterAdminCommands();

    WebhookSend("1sT-AntiDLL — plugin loaded",
        std::string("Version ") + GetVersion() + " loaded. wh_detection=" +
        (g_Cfg.whEnabled ? "on" : "off") + ", debug_only=" + (g_Cfg.whDebugOnly ? "on" : "off"),
        0x2ECC71);

    // Point decay timer.
    g_pUtils->CreateTimer(g_Cfg.decayInterval, []() {
        DecayPoints();
        return g_Cfg.decayInterval;
    });

    // Legacy DLL event-listener detection timer.
    g_pUtils->CreateTimer(g_Cfg.dllInterval, []() {
        if (g_Cfg.enabled)
            RunDllDetection();
        return g_Cfg.dllInterval;
    });

    // Hidden-target tracking timer.
    if (g_Cfg.whEnabled)
    {
        g_pUtils->CreateTimer(g_Cfg.whCheckInterval, []() {
            if (g_Cfg.enabled)
                RunHiddenTargetTracking();
            return g_Cfg.whCheckInterval;
        });
    }
}

//==============================================================================
// Plugin metadata
//==============================================================================

const char* AntiDLL::GetLicense()     { return "GPL"; }
const char* AntiDLL::GetVersion()     { return "1.0.0"; }
const char* AntiDLL::GetDate()        { return __DATE__; }
const char* AntiDLL::GetLogTag()      { return "1sT-AntiDLL"; }
const char* AntiDLL::GetAuthor()      { return "Snaximusss+"; }
const char* AntiDLL::GetDescription() { return "1sT-AntiDLL - Anti-DLL injection & defensive hidden-target tracking for CS2"; }
const char* AntiDLL::GetName()        { return "1sT-AntiDLL"; }
const char* AntiDLL::GetURL()         { return "https://discord.gg/g798xERK5Y"; }
