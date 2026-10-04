# Handoff: AC97 silent boot (roadmap item 9), branch fix-ac97-boot (off dev 3620bd2), uncommitted

## Verified
- Evidence log `C:/xemu/hw/logs-audio-20261004-1153/boot_prev.log`: `AC97 polled` with global status 00300100 (codec ready),
  `stuck: civ 0 lvi 6 sr 00/00` from restart 1 = PCM and S/PDIF bus masters running (DCH 0), never fetching; 10 cold resets, no recovery.
- Original driver (`xbox/src/hw/xhw_audio.c`): boot does cold reset + RR bus-master reset only, never touches the APU or the PCI config;
  recovery = restart every ~100 ms, cold reset every 3 dead restarts, up to 32; RR wait loop spins up to 1M MMIO polls (likely the ~1 s freezes).
- APU register map, from xemu `hw/xbox/mcpx/apu/apu_regs.h` + `apu.c` + `dsp/gp_ep.c` (fetched): ISTS 0x1000, IEN 0x1004, FECTL 0x1100,
  SECTL 0x2000 (XCNTMODE 0x18, 0 = off: no frames), XGSCNT 0x200C, GPOFCUR0 0x302C, EPOFBASE0/END0/CUR0 0x4024/28/2C,
  GP region at +0x30000, EP at +0x50000, GPRST/EPRST at region+0xFFFC: DSP runs only with bits 0 and 1 set, writing 0 resets it.
  xboxdevwiki "APU": EP output goes to the ACI via EP FIFO channels 0 (PCM) / 1 (SPDIF), i.e. DSP FIFO DMA to memory, AC97 bus masters play it.
- PCI: ACI = bus 0 dev 6, APU = dev 5 (xemu xbox.c); nxdk `HalReadWritePCISpace(bus, dev|fn<<5, reg, buf, len, write)` (pbkit uses it).

## Hypothesis (inferred, not verified)
The previous XBE (dashboard DirectSound, or our crashed instance) leaves the ACI bus masters and/or APU DSP/FIFO DMA running or half set up;
quick reboot resets neither, AC-link cold reset doesn't reset bus-master/PCI state. Candidates the new boot log will tell apart:
ACI PCI bus-master enable off, a wedged bus master (RR never clears: see `bus-master reset N polls`), codec powered down (reg 0x26 PR bits),
or APU DMA. SMC soft power still possible.

## Done (diff, builds clean; host tests pass: tex_convert, fog, rc, anim/pobj_mtx, tex_cache, dl_cull, audio_mix, mplib, card_endian;
## test_lower/test_vp_encoder/vp_policy failed with missing file/module in this MSYS env, unrelated, not checked further)
- `xhw_audio.c`: `audio_found()` (3 `[AUDIO] found` lines: ACI PCI cmd, global ctl/sta, both BMs bd/civ/lvi/sr/picb/cr read twice 5 ms apart;
  APU via MmMapIoSpace of its BAR; codec regs via CAS semaphore), `audio_idle()` (ACI PCI mem+BM enable if off, halt both BMs with 20 ms DCH wait,
  APU IEN/SECTL/GPRST/EPRST = 0 + ISTS clear on the AC97 path only), codec 0x26 power-up if PR1-PR5 set, `[AUDIO] idle` line;
  give-up `aci_give_up()` when nothing ever played and a 2nd recovery cold reset would start: `[AUDIO] stuck since boot`, BMs halted, pump drains
  ring in real time (`aci_drain`), on-screen notice; `-DXHW_AUDIO_TEST` bits 1 (leave engine running at relaunch) / 2 (CIV forced 0).
- `nv2a.c` `xhw_notice()` + `xhw_overlay.c` multi-row HINT: 10 s two-line notice. `xhw_main.c` `lastexit.txt` record
  (`xhw_exit_reason`/`xhw_exit_write`, `[BOOT] previous exit`), hooks in crash, watchdog, pad.c reset combo, menu.c restart, os.c OSResetSystem, fatal.
- `tools/xbox/scenarios/relaunch/autopad.txt`; docs: testing.md (switch, lastexit.txt, "Audio at boot"), handoff.md, platform.md, decisions.md.

## Not done / next steps
1. roadmap.md item 9 state not updated. Convert edited files back to CRLF if wanted (written LF; git normalizes).
2. xemu (none run; both lock slots were busy): builds saved in `C:/xemu/run-audio/builds/` (`ac97.xbe` = AUTOPAD+APU=0; `test1` built in
   build-xbox/xbe/default.xbe, copy it). Run `MX_RUN=C:/xemu/run-audio MX_STAGE_EXTRA=tools/xbox/scenarios/relaunch` with test1 (boot 2 must show
   `found ... cr 01`, `idle ... halt pcm ok`, then AC97 polled, no stuck), test2 (`stuck since boot`, notice in the frame-420 shot), and a default
   `-DXHW_AUTOPAD=1` gl smoke.
3. Console test build (`-DXHW_AUTOPAD=1` or `-DXHW_PROF=1`) staged to `C:/xemu/hw/stage-audio` + user instructions: power off mid-match,
   dashboard, launch; quit to dashboard, launch. Confirm: `[AUDIO] found/idle` lines on every boot; a silent boot must show which register is odd,
   and then one `stuck since boot` + notice instead of the cold-reset loop.
4. Review: APU MMIO writes on hardware are new (risk: low, DSound does the same); codec reads over the AC-link at boot are new on hardware.
