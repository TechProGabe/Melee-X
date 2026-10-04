# Melee-X documentation

| document | what it covers |
|---|---|
| [architecture.md](architecture.md) | layers, the 32-bit data model, the memory budget, video and input at a glance |
| [toolchain.md](toolchain.md) | setup (Docker, native Windows), the game triple, DISC_STRUCT lowering, the build steps, releases, CI |
| [platform.md](platform.md) | the Dolphin SDK on the Xbox: boot, OS, DVD, ARAM, PAD, `settings.ini`, VI, audio, CARD |
| [renderer.md](renderer.md) | GX -> NV2A: vertex programs and their constants, depth and culling, TEV -> combiners, textures |
| [testing.md](testing.md) | host tests, running on an Xbox or xemu, logs, symbolizing crashes, console rounds, first-boot checklist |
| [decisions.md](decisions.md) | the choices the port rests on, edits to imported code, how to sync melee-pc, known risks |
| [roadmap.md](roadmap.md) | what's done, known issues and the numbered open items, next steps, console history, the performance plan, future features |
| [pgo.md](pgo.md) | the PGO profile `xbox/melee.profdata`: what it holds, why it may be committed, what must match, how to regenerate it |
| [fps-plan.md](fps-plan.md) | finished: the frame-rate work (2026-10-03/04), its outcome, console rounds 1-6, every item tried and its result |
| [lan-plan.md](lan-plan.md) | LAN play plan: two Xboxes, phases 0A-5; 0A/0B paused |
| [handoff.md](handoff.md) | current state (v52 on the two test consoles, paused branches), this PC's setup, scenarios, working notes for the next session |
