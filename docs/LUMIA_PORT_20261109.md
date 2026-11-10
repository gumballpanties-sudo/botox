# Lumia aim logic and optional Auto Align

Botox baseline: `99e3d14b21fe44c7cdef2a78d63a6dc42b6b51fb`.
Lumia reference: `D:/code/lumia/lumia`, captured on 2026-11-09. Exact reference
hashes and normalized algorithm hashes are in
`tools/tests/lumia_port_reference.json`.

## Aim

The existing Botox aim menu, config names and weapon/hitbox group mappings remain.
The aim path now uses Lumia's fresh native SetupBones mask/time, actual-slot target
enumeration, nearest eligible angular FOV selection, finite/clamped FOV math,
absolute recoil compensation and time-based smoothing. Smoothed acquisition keeps
its selected native hitbox while valid, settles on the actual hitbox, and carries
silent-command progress plus new mouse input across consecutive commands. The
first visible shot no longer bypasses smoothing. Silent aim respects the existing
RCS strength and pitch/yaw settings instead of forcing full compensation.

Botox's smoothness range remains 1–30: it maps monotonically to Lumia's 0–100
smoothing percentage, with 1 giving an immediate aim and 30 the slowest setting.
The camera-frame camlock uses the same timing and pauses on a zero frame interval.
This is an adaptation of existing settings, not a copied Lumia settings/profile UI.

Botox's existing trigger/aim-key activation, no-switch option, player-list/team
filtering, scope FOV scaling, per-weapon overrides, seven hitbox groups, unscoped
sniper body selection, autowall/minimum damage, RCS start-bullet, standalone RCS
and only-with-aimbot gate remain available. Existing nospread, Zeus, grenade aim
and backtrack methods remain intact. Their publication/shot/movement integration
stays in Botox's original command hook. Visible shots still use Botox's existing
backtrack selection on the actual published shot; silent aim commits its selected
record through the existing API. No triggerbot, smart-silent, player-profile or
other Lumia-only controls were added.

## Auto Align

The existing Auto Align popup now has **type: botox / lumia**. Botox is the
default, including when an older full config lacks the new appended integer
`m_auto_align_type`. Config save/load retains an explicit choice; visual-only
configs cannot change it. Existing registry indexes are preserved.

The Botox backend's entire original search function is unchanged after its new
dispatch line. The Lumia backend is the normal `impl_t::auto_align` search from
Lumia's movement.cpp, excluding the alternate Botox backend and diagnostics.
Wall traces, candidate ordering, one/two-step contact pushes, freelook hold,
standing/crouched jump candidates and native pin comparisons remain faithful.
The existing Botox pixeljump-priority option is reused before this search.

The adapter uses Botox's native prediction and engine tick/gravity pin formula.
Lumia's portable native datamap/snapshot helpers restore caller player state,
high-offset prediction fields, ordinary cached fields, command pointers, bounds,
prediction view and speed/face metadata after each search. It borrows no engine
history slots. Lumia's custom prediction implementation/settings were not copied.

## Validation and artifacts

Four suites pass:

- `lumia_aim_port.test.py`: actual aim/scanner execution in debug and optimized
  x86 fixtures. Fresh poses/FOV, obstruction/penetration, silent progress/step,
  first-shot smoothing, recoil percentages/start bullet/standalone gate, camlock
  frames/pause, no-switch/slot reuse, physical key release and history routing.
- `lumia_align_port.test.py`: actual backend/adapter at 64/128 tick and several
  views. Contact pushes, freelook, standing/crouched jump candidates, complete
  caller/high-offset/private state restoration, guards and pixeljump routing.
- `lumia_port_config.test.py`: actual registry/save/load with temporary IO,
  selector round-trip, old/wrong-type config handling, visual isolation and
  existing aim/priority settings.
- `lumia_port_fidelity.test.py`: pinned Lumia smoothing/RCS/search hashes;
  unchanged Botox alignment, 14 feature wrappers, aim menu, command hook,
  grenade/backtrack code and existing variable registry/defaults.

These are controlled checks, not live-server aim/catch or FPS measurements.
The first native test assertion incorrectly assumed that recoil-aware selection
would always pick the head. The fixture was corrected to explicitly select the
head for RCS checks. Its CRT dialog was closed and all test harnesses now route
assertions to stderr with crash dialogs disabled. An alignment fixture's initial
clearance exceeded the reference search's two-tick reach at 128 tick; feasible
clearances exercise the unchanged algorithm. The initial build's missing M_PI
macro was replaced with the identical float constant.

Final Release/x86 DLL: `csgo-sdk/release/botox.dll`, 15,441,408 bytes,
SHA256 `cf32abf6e85c1449c8b661e66a1b839a0fbd7c036bc6f57adc56aa7502bb7284`.
Matching PDB SHA256
`a1186d4faef650d6cd8a8d722aafd9729895aeca24b775d8c855af16d24591b8`,
raw GUID `28d21bb15b3212449714b09d03ec2a93`, age 3. Final build has zero
errors and compiler/linker warnings. All 912 recorded build inputs are stable.
No game or loader was launched/injected, and no live configs/points were written.

Before/after source and binaries, exact Lumia references, source diff, controlled
fixtures, test/build logs and verification manifests:
`C:/backups/botox-lumia-aim-align-20261109`.
An independent concurrent Lumia change added floor-level command calls while
this port was running; the referenced Auto Align function and aim algorithms are
unchanged. This task only writes Botox and its backup/test artifacts.
