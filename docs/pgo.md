# The PGO profile (`xbox/melee.profdata`)

Profile-guided optimization (`docs/fps-plan.md` A4; `docs/toolchain.md`
"PGO") compiles the game and sdk code using a profile of how often each
branch is taken. The profile is a file in the repository:

| file | what |
|---|---|
| `xbox/melee.profdata` | LLVM indexed profile (binary, ~2.4 MB): per function, its name, a hash of its control flow, and a count per instrumented block |
| `xbox/melee.profdata.txt` | where it came from: the commit it was trained on, the scenarios, the LLVM version, totals |

A build uses it with `XBOX_PGO=xbox/melee.profdata` (a path relative to
the checkout). Without `XBOX_PGO` it is not read at all.

## Why it can be committed

It holds no game data. Its contents are the names of functions (the same
identifiers as in `src/`, which is already in the repository), hashes of
their compiled control flow, and execution counts measured in xemu. No
bytes of the disc, the DOL, textures, sounds or saves are in it; the
`.gci` save and the disc image the training runs need stay outside the
repository (`MX_CARD`, `MX_ISO`), as `CLAUDE.md` requires. The counts
are measurements of our own build running, like the numbers in
`docs/fps-plan.md`. (Checked 2026-10-03; the user approved committing it.)

## Why a stale profile is safe

A profile never changes what the code does: it only steers the
optimizer's choices (inlining, block layout, hot/cold splitting), as
`-O2` itself does. The simulation check (`[SIMH]`, `docs/testing.md`)
still gates every build that uses it.

When the code changes, functions whose control flow changed no longer
match their hash and are compiled as if unprofiled; new functions have no
counts; renamed ones lose theirs. The build still succeeds (clang's
warnings for this are off with the rest of `-Wno-everything`). A stale
profile only costs speed, never correctness: regenerate it after large
changes to the game's hot code, and before a release that should be fast.
`llvm-profdata show --all-functions xbox/melee.profdata | grep -c Hash`
against the function count of a fresh `XBOX_PGO=gen` build gives a rough
idea of how much still matches.

## What must match between training and use

- **LLVM version.** An indexed profile is read by the clang that made it
  or a newer one. Training and use are both LLVM 21.1.8 (Docker image,
  MSYS2, GitHub Actions). After an LLVM upgrade, regenerate.
- **`-mllvm -static-func-full-module-prefix=false`** in both builds
  (`compile_game.py`, `xbox/CMakeLists.txt`): static functions are named
  by their file's name alone, because the lowered sources' paths differ
  between the instrumented build's object folder (`game-pgogen`) and the
  optimized one's (`game-pgo`). Dropping it on one side unprofiles every
  static function. Two static functions with the same name in files with
  the same name (different folders) share an entry; harmless.
- **The same compile flags otherwise** (`COMPILE_FLAGS`): a flag that
  changes the code before instrumentation changes the hashes.
- **Value profiling off** (`-mllvm -disable-vp` in the instrumented
  build): the image has no compiler-rt to record values, and the profile
  has none.

## Regenerating it

```sh
MX_CARD=/path/to/folder-with-card_a tools/xbox/pgo_train.sh   # + xemu_run.sh's MX_* environment
git add xbox/melee.profdata xbox/melee.profdata.txt
```

`pgo_train.sh` builds the instrumented image (`XBOX_PGO=gen`), plays
`gl`, `fodperf`, `fd2`, `ps` and `corn` in xemu with `-icount` (the
counts don't depend on xemu's speed, and branch counts are the same on
the console), converts each run's `[PGOC]` dump with
`tools/xbox/pgo_raw.py`, merges them and writes the provenance file. It
takes about 20 minutes on the Windows PC. Other scenarios can be named on
its command line. One xemu at a time: it refuses to start beside another.

How the pieces work: the instrumented image has no compiler-rt;
`xbox/src/hw/xhw_pgo.c` defines `__llvm_profile_runtime` and writes the
counter section (`.lprfc`) as `[PGOC]` lines at every scene's exit and
the match's end (cumulative since boot). `pgo_raw.py` rebuilds a raw
profile (version 10, 32-bit) from the last complete dump plus the linked
image's `.lprfd` (function records) and `.lprfn` (names) sections and its
`__llvm_profile_raw_version` word, which the link keeps with
`-include:` for this.

## Release builds

The GitHub `build` workflow builds with whatever `XBOX_LTO`/`XBOX_PGO`
it sets (`docs/toolchain.md` "Release"); the profile is in the checkout,
so nothing has to be downloaded. A release made with PGO should say in
`xbox/melee.profdata.txt` which commit trained it, and its `[SIMH]` and
screenshots gate like any other build.
