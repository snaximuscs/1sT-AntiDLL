#pragma once

// RayTraceVisibility
// Thin, isolated wrapper around the FUNPLAY-pro-CS2/Ray-Trace Metamod interface.
//
// All coupling to <craytraceinterface.h> lives in raytrace.cpp behind the compile flag
// ANTIDLL_HAVE_RAYTRACE. When that flag is NOT defined (the default), this module still
// compiles and links, but reports Ray-Trace as unavailable and every trace is "not blocked"
// (fail-safe: the detector will never flag a player without a real visibility check).
//
// To enable real Ray-Trace visibility checks:
//   1. Clone FUNPLAY-pro-CS2/Ray-Trace and add its `public/` dir to the include path.
//   2. Build with -DANTIDLL_HAVE_RAYTRACE (see AMBuilder notes).
//   3. Verify the TODO-marked struct/field names in raytrace.cpp match the real header.

#include "antidll.h"

// Acquire the Ray-Trace interface via MetaFactory. Returns true on success.
bool RayTrace_Load();

// True only if the interface was successfully loaded.
bool RayTrace_IsAvailable();

// Toggle debug beam drawing for traces (server-side visual aid only).
void RayTrace_SetDebugBeam(bool enabled);

// Low-level visibility primitive: is the segment start->end blocked by world geometry?
// Returns false when Ray-Trace is unavailable (fail-safe — treated as "not blocked").
// ignoreEntity is passed as CEntityInstance* (CBaseEntity is incomplete in the public SDK);
// it is reinterpret_cast to CBaseEntity* only inside the guarded Ray-Trace call.
bool RayTrace_LineBlockedByWorld(const Vector& start, const Vector& end, CEntityInstance* ignoreEntity);
