# Licensing

This repository mixes code under different terms. Read all of it before
redistributing any part.

## 1. Game code: not licensed

`src/melee/`, `src/sysdolphin/`, `src/Runtime/`, `src/thp/` and
`src/sdk_include/` are a decompilation of Super Smash Bros. Melee from
[doldecomp/melee](https://github.com/doldecomp/melee), as adapted by
[melee-pc](https://github.com/999sian/melee-pc) (divergences tagged
`/* PORT: ... */`). Super Smash Bros. Melee is copyright Nintendo / HAL
Laboratory. The upstream decompilation publishes no license and this
repository cannot grant one. No permission to copy, modify or redistribute
this code is offered or implied.

No game assets are in this repository. Everything the game draws or plays is
read at runtime from a disc image the user supplies.

## 2. Port code: GPL-3.0-or-later

`src/pc/` and `tools/lower/` come from melee-pc (GPL-3.0-or-later,
`COPYING`). The Xbox layer (`xbox/`, `tools/xbox/`) builds on them and is
released under the same terms.

Parts of `xbox/` are carried over from
[OpenCrossing-Xbox](https://github.com/GabeConway/OpenCrossing-Xbox) (MIT),
whose terms are compatible.

## 3. Third-party code

- `extern/aurora/include`: aurora's public Dolphin SDK headers, MIT
  (`extern/aurora/LICENSE`).
- `src/pc/libm`: musl, MIT (`src/pc/libm/LICENSE`).
- nxdk is not vendored; it is fetched at build time under its own licenses.
