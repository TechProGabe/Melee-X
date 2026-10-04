# Phase 0A handoff (paused 2026-10-04, branch lan-0a-probes, nothing committed)

Worktree: `C:\Users\Bingo\Documents\GitHub\Melee-X\.claude\worktrees\agent-a798cfa17b0d47d10`, branched from dev 3e565e4.
Scratch (outside repo): `C:\xemu\run-0a\` (own xemu.toml + HDD/EEPROM copies, `run.sh` slot-locking runner,
`chain1.sh`, `chain2.sh`, `verbose.sh`, `dumpdiff.py`, `simrows.py`, logs in `logs/`, XBEs+maps in `xbe/`).
Base build of dev: worktree `C:\xemu\base-0a` (built, autopad and plain).

## Done (files)
- `src/sysdolphin/baselib/random.c`: PORT: `xsdk_rand_draw(seed, __builtin_return_address(0))` in HSD_Rand/HSD_Randf
  (HSD_Randi inlines HSD_Rand: checked with llvm-objdump).
- `xbox/src/sdk/simhash.c`: RNG trace (`env MX_RAND_TRACE=1`: `[RAND] tick N: D draws, hash H, callers C` per tick,
  batched log calls; `env MX_RAND_DUMP=a-b`: `[RANDD]` callers, r:/t: for render pass/off thread); `[NETM]` at ticks 600/3600.
- `xbox/src/sdk/os.c`: `xsdk_jitter` (`env MX_JITTER=<seed>`, 0-2 ms), `xsdk_netm` (heap live cells + copy probe).
- `vi.c` (jitter at frame boundary, `xsdk_in_render`), `dvd.c` (jitter before completion), `sdl3_audio.c` (jitter in mixer
  loop), `xsdk.h` (declarations; outside the plan's file list), `xbox/include/xhw.h` + `xbox/src/hw/xhw_sys.c`
  (`xhw_mem_copy_probe`, wbinvd before each copy).
- `tools/xbox/simh_diff.py` (first count / callers / order / simh difference, dump at the parting tick, --map).
- `tools/xbox/console_round.py`: round 7 (19 runs: warm-up, det x6 with trace + dump 4300-4700 at 720p, 8 v1-* at 720p,
  v1-fod/ps/fd at 480p, det at 480i), scenario files loaded from tools/xbox/scenarios, `[NETM]` summary in `report`.
- Scenarios `det`, `v1-fod v1-ps v1-ys v1-dl v1-bf v1-fd v1-corn v1-pc`.
- Docs: testing.md (switch rows, `[RAND]`/`[NETM]` lines, "Where two runs part"), decisions.md ("Determinism probes"
  section + random.c edit line), README.md (lan-plan row), handoff.md (scenario rows). lan-plan.md NOT yet updated.

## Verified
- Host tests (MSYS2): all pass except environment-only: test_lower (same disc_access mismatch on base 3e565e4: MinGW GCC
  oracle), test_vp_encoder (no nv2a_vsh module on this PC), vp_policy --check (prints ok, then Windows temp-file error).
- G0 gl, -icount, no shots: `icount_report.py base-gl.log new-gl.log`: [SIMH] 62/62 equal, sim/tick +0.4%, others <=0.4%.
  (that new-gl was the pre-batching binary; rerun `new-gl2` on the final binary was queued in chain2, not run.)
- base-gl vs new-gl-trace (MX_RAND_TRACE=1): [SIMH] 62 lines identical (trace observes only).
- Lockstep gl shots base vs new (final binary v2): 4/4 byte-identical, [SIMH] equal.
- G0 fodperf: [SIMH] 72/72 equal, BUT sim/tick -4.4% (others within 0.6%); suspected host load on base run
  (sim absorbs worker-thread preemption). Not resolved: rerun base-fodperf2/new-fodperf2 (in chain2).
- [BEAT] free: new 8 KB below base (gl). Plain release XBE size unchanged: 4,888,576 bytes both; autopad +4096.
- Round 7 staged locally: `MX_HW=/c/xemu/hw console_round.py 7 stage /c/xemu/hw/builds-r7` -> `C:\xemu\hw\stage-r7`
  (build lan0a.xbe = current branch autopad build v3, map beside it). Not uploaded.

## Findings (for lan-plan.md F5/F6/Status)
- `[NETM]` in xemu: OSAlloc heap 1 is 17.5 MB with ~6.5-6.7 MB live (gl and Fountain 4-CPU, ticks 600/3600), heap 0
  171 KB: a heap snapshot is ~6.7 MB, not 2-3 MB -> ~7.4 MB with statics, 8 slots ~60 MB: strengthens D2. Copy time:
  xemu meaningless (525 us/MB); console number pending S1.
- Item 10 REPRODUCES IN XEMU UNDER -icount: det twice (det-ic1/2): [SIMH] parts at tick 4500, first draw-count
  difference at tick 4447 = 8 extra draws from `ifStatus_802F4B84` (HUD damage-digit shake) in one run only, with all
  148 earlier draws of that tick identical: a hit/damage happened without any RNG difference before it ->
  hypothesis F6 item 3 (off-screen damage tick reads `is_offscreen` written by a GX callback, ifmagnify.c:645-705).
  Earlier "-icount never parts" claims came from 60-70 s matches that end before tick 4440.
- Particle system (`hsd_8039930C`, `hsd_80398F8C`) draws in a different order between -icount runs from ~tick 16-19
  (same count and callers): harmless to [SIMH], but it poisons a rolling hash (hence the callers sum).
- Per tick: mean 111 draws, max 340 (Fountain 4-CPU); none in render pass or off thread in xemu.

## Left
- Gate items not met/not run: "det twice under -icount equal through 7200" FAILS (item 10 in xemu, above: report, do
  not weaken); det x4 real time with MX_JITTER=1..4 (chain2, needs both slots/no other xemu); new-gl2, new-gl-trace2,
  fodperf rerun pair, new-reldef-gl (release-default build `xbe/new-reldef.xbe` built but not run), base-det1/2 control.
- det-verb1 finished (`logs/det-verb1.log`, MX_SIMH_VERBOSE to tick 4529); det-verb2 was killed: rerun it and diff
  per-fighter fields around 4440-4447 to confirm the 1% off-screen damage hypothesis.
- Update lan-plan.md (Status row, F5, F6, the gate result), then S1 hand-over (five-line instruction, audio check:
  NETM 2 MB alloc + wbinvd at ticks 600/3600; trace log writes; jitter code is a no-op without MX_JITTER).
- Don't redo: builds in `C:\xemu\run-0a\xbe` (base-autopad, base-plain, new-autopad=v3, new-plain, new-reldef) and the
  chain1 logs are valid; game-code addresses are the same in all branch builds.
