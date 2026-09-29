# Roadmap

- [x] Toolchain: nxdk + LLVM 21, `disc_lower` for DISC_STRUCT, game triple chosen and
      ABI-checked against nxdk (`docs/toolchain.md`)
- [x] All 1008 game translation units compile to i386 COFF
- [x] Platform layer, first pass
  - [x] OS: MEM1 at a fixed VA, SDK heaps, alarms, time, interrupts (recursive lock)
  - [x] DVD: FST over the user's .iso/.gcm/.ciso, async reads on a worker
  - [x] AR/ARQ: ARAM buffer
  - [x] PAD: four ports by physical port, GC-like layout, dead zones, rumble, settings.ini
  - [x] VI: 60.000 Hz pacing; 720p / 480p / 480i chosen from the dashboard
  - [x] AX/AI: melee-pc's mixer -> AC97 (OpenCrossing's polled driver; APU voice in xemu)
  - [x] CARD: slot A as .gci files in `E:\UDATA\4d580001\card_a\`
  - [x] MTX: aurora's C implementations
- [x] GX -> NV2A, first pass
  - [x] state, immediate mode, display lists (big-endian, indexed arrays)
  - [x] generated vertex programs: a0-indexed skinning, GX lighting (spot, distance,
        specular), texgen; encoder checked bit for bit against nv2a-vsh
  - [x] TEV -> register combiners (from OpenCrossing's compiler, 8 stages, 4 units)
  - [x] textures: CMPR -> DXT1, I/IA -> AY8/A8Y8, RGB565 native, the rest A8R8G8B8
        (NPOT resampled), TLUTs, EFB copies (CPU)
  - [x] 720p 16:9 content rect for melee-pc's hor+ widescreen
- [x] default.xbe links
- [x] boots to the title screen in xemu ("TechProGabe Presents..." card first)
- [x] attract-demo VS matches run in xemu (slowly: about 5 fps there)
- [x] memory fit, first pass: native texture formats; MEM1 and ARAM committed on demand
- [ ] memory fit measured on hardware (boot.log `[MEM]`); disc-backed ARAM if needed
- [ ] fog, indirect texturing, TEV swap tables beyond alpha broadcast, GPU EFB copies
- [ ] VS mode with 4 players on hardware at 60 fps
- [x] movie frames decoded (`thp.c`)
- [ ] release packaging
