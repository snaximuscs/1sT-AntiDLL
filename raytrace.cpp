#include "raytrace.h"

#ifdef ANTIDLL_HAVE_RAYTRACE
#include "CBaseEntity.h"
#include "craytraceinterface.h"
static CRayTraceInterface* g_pRayTrace = nullptr;
#endif

static bool g_bRayTraceLoaded = false;
static bool g_bRayTraceDebugBeam = false;

// g_pUtils is defined in antidll.cpp; we only use it for logging here.
extern IUtilsApi* g_pUtils;

bool RayTrace_Load()
{
#ifdef ANTIDLL_HAVE_RAYTRACE
    int ret = 0;
    g_pRayTrace = static_cast<CRayTraceInterface*>(
        g_SMAPI->MetaFactory(RAYTRACE_INTERFACE_VERSION, &ret, nullptr));

    if (ret == META_IFACE_FAILED || !g_pRayTrace)
    {
        g_bRayTraceLoaded = false;
        if (g_pUtils)
            g_pUtils->ErrorLog("[1sT-AntiDLL] Ray-Trace interface not found. Hidden-target visibility checks disabled.");
        return false;
    }

    g_bRayTraceLoaded = true;
    if (g_pUtils)
        g_pUtils->LogToFile("AntiDLL", "[1sT-AntiDLL] Ray-Trace interface loaded successfully.\n");
    return true;
#else
    // Built without Ray-Trace support. Visibility detection stays disabled by design.
    g_bRayTraceLoaded = false;
    if (g_pUtils)
        g_pUtils->ErrorLog("[1sT-AntiDLL] Built without ANTIDLL_HAVE_RAYTRACE. Hidden-target visibility checks disabled.");
    return false;
#endif
}

bool RayTrace_IsAvailable()
{
    return g_bRayTraceLoaded;
}

void RayTrace_SetDebugBeam(bool enabled)
{
    g_bRayTraceDebugBeam = enabled;
}

bool RayTrace_LineBlockedByWorld(const Vector& start, const Vector& end, CEntityInstance* ignoreEntity)
{
#ifdef ANTIDLL_HAVE_RAYTRACE
    if (!g_bRayTraceLoaded || !g_pRayTrace)
        return false; // fail-safe: unknown visibility -> treated as not blocked

    // CBaseEntity derives from CEntityInstance (single inheritance, same address), but CBaseEntity
    // is incomplete in the public SDK so we reinterpret_cast here, inside the guarded block.
    CBaseEntity* ignore = reinterpret_cast<CBaseEntity*>(ignoreEntity);

    // TODO: confirm the exact TraceOptions / TraceResult struct + field names and the
    // TraceEndShape() signature against the real public/craytraceinterface.h. The names
    // below follow the project README; adjust if the CI build reports mismatches.
    TraceOptions opts{};
    opts.InteractsWith = static_cast<uint64_t>(MASK_SHOT); // TODO: confirm world/solid mask constant
    opts.DrawBeam = g_bRayTraceDebugBeam ? 1 : 0;

    TraceResult out{};
    bool hit = g_pRayTrace->TraceEndShape(&start, &end, ignore, &opts, &out);
    if (!hit)
        return false;

    // Fraction < ~1.0 means something solid was hit before reaching the target point.
    return out.Fraction < 0.98f; // TODO: confirm field name (Fraction vs fraction)
#else
    (void)start;
    (void)end;
    (void)ignoreEntity;
    return false; // Ray-Trace not compiled in -> never "blocked" -> detector cannot flag
#endif
}
