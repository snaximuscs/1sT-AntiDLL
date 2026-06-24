# 1sT-AntiDLL — Ray-Trace integration & build notes

## What's compiled by default

The plugin **always builds without Ray-Trace.** Hidden-target visibility checks are gated behind
the compile flag `ANTIDLL_HAVE_RAYTRACE`, which is **off by default**. With the flag off:

- `RayTrace_Load()` returns `false` and logs a warning.
- `RayTrace_IsAvailable()` is `false`.
- Every `RayTrace_LineBlockedByWorld()` returns "not blocked" (fail-safe).
- With `raytrace_required "1"`, the hidden-target detector **never flags anyone** (it cannot
  verify visibility), so there are zero false positives until Ray-Trace is wired up.

## How to enable real Ray-Trace visibility

1. **Get the headers.** Clone `FUNPLAY-pro-CS2/Ray-Trace` (GPLv3 — review the license before
   shipping a combined binary). You only need its `public/` directory.

2. **Edit `AMBuilder`** — uncomment the two marked lines:
   ```python
   binary.compiler.cxxincludes += [ os.environ.get('RAYTRACE_PUBLIC', os.path.join(builder.sourcePath, '..', 'Ray-Trace', 'public')) ]
   binary.compiler.defines += ['ANTIDLL_HAVE_RAYTRACE']
   ```
   > If `binary.compiler.defines` is rejected by your AMBuild version, use the cxxflag form:
   > `binary.compiler.cxxflags += ['-DANTIDLL_HAVE_RAYTRACE']`

3. **(CI)** add a clone step next to the other SDK clones in `.github/workflows/build.yml`:
   ```bash
   git clone --depth 1 https://github.com/FUNPLAY-pro-CS2/Ray-Trace.git ../Ray-Trace
   ```
   and export `RAYTRACE_PUBLIC="$GITHUB_WORKSPACE/../Ray-Trace/public"` for the build step.

4. **Verify the TODO-marked API** in `raytrace.cpp` against the real `craytraceinterface.h`:
   - `RAYTRACE_INTERFACE_VERSION` string
   - `TraceOptions` fields (`InteractsWith`, `DrawBeam`) and the mask constant
   - `TraceResult` field (`Fraction` vs `fraction`)
   - `TraceEndShape(start, end, ignoreEntity, opts, out)` signature

   These names come from the project README, not a verified header — fix whatever the CI
   compiler reports. All of it is confined to `raytrace.cpp`.

## Outstanding TODOs (safe fallbacks in place)

| Area | Current fallback | Proper fix |
|------|------------------|-----------|
| Eye angles | `GetAbsAngles()` (body angles) | schema field `m_angEyeAngles` |
| Eye height | fixed `+64` | schema field `m_vecViewOffset` |
| Enemy alive check | not checked | schema field `m_iHealth` / pawn life state |
| Warmup/freeze gate | not checked | `CCSGameRules` schema |
| Map / server name in logs | `"unknown"` | `gpGlobals` map name / `hostname` cvar |
| `ignore_admins` | no-op | needs an admin API (none exposed by IPlayersApi) |

None of these block the build; each fails safe (never produces a false flag).

## Safe testing procedure

1. Build & install. Confirm the plugin loads (`meta list`).
2. In `settings.ini`:
   ```
   "wh_detection_enabled"  "1"
   "wh_debug_only"         "1"     // critical: log only, no punishment
   "raytrace_required"     "1"
   "debug_mode"            "1"     // optional: see live evidence in chat
   ```
3. Run `antidll_status` in server console — confirm `raytrace=1` once the header is wired.
4. Collect `addons/AntiDLL/logs/antidll.log` for 1–2 days. Review `type=SUSPICION` lines.
5. Only if false positives are acceptably rare, set `"wh_debug_only" "0"` to enforce.

## Admin commands (server console / rcon only)

| Command | Effect |
|---------|--------|
| `antidll_reload` | Reload `settings.ini` + `events.txt` |
| `antidll_status` | Print config + Ray-Trace + tracked-player summary |
| `antidll_player <slot>` | Print one player's violation state |
| `antidll_reset <slot>` | Clear one player's state |
| `antidll_test_webhook` | Queue a test Discord message |
| `antidll_debug <0/1>` | Toggle debug output |
