# Roadmap

- [x] Toolchain: nxdk + LLVM 21, `disc_lower` for DISC_STRUCT, game triple chosen and
      ABI-checked against nxdk (`docs/toolchain.md`)
- [x] All 1008 game translation units compile to i386 COFF
- [ ] Platform layer, first pass (links, stubs where needed)
  - [ ] OS: arena at a fixed VA, heaps, alarms, time, interrupts (mutex)
  - [ ] DVD: FST over the user's .iso/.gcm/.ciso, async reads on a thread
  - [ ] AR/ARQ: ARAM buffer (disc-backed later)
  - [ ] PAD: four ports, GC-like layout, rumble
  - [ ] VI: vblank pacing at 60 Hz, 480/720p mode select
  - [ ] AX/AI: melee-pc mixer -> AC97 (OpenCrossing's polled driver)
  - [ ] CARD: GCI files in `E:\UDATA\<title id>\`
  - [ ] MTX: C implementations
- [ ] GX -> NV2A
  - [ ] state, immediate mode, display lists (big-endian, indexed arrays)
  - [ ] matrix palette skinning in the vertex program
  - [ ] TEV -> register combiners (OpenCrossing's `xbox_tev_rc.c`)
  - [ ] textures (GC formats -> swizzled A8R8G8B8, TLUTs), EFB copies
  - [ ] 720p 16:9 through melee-pc's widescreen
- [ ] default.xbe links; boots to the title screen in xemu
- [ ] VS mode with 4 players on hardware at 60 fps
- [ ] THP movies, memory fit on 64 MB, release packaging
