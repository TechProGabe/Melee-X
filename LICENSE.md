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

## 4. Linked into default.xbe from nxdk

The XBE links nxdk's network library (`libnxdk_net.lib`, `xbox/src/hw/xhw_net.c`),
so a build distributes the following in binary form. Their notices:

**lwIP 2.2.1** and nxdk's lwIP port (`lib/net/lwip`, `lib/net/nforceif`,
`lib/net/nvnetdrv/nvnetdrv_lwip.c`): Copyright (c) 2001-2004 Swedish
Institute of Computer Science; Copyright (c) 2001-2004 Leon Woestenberg,
Axon Digital Design B.V.; Copyright (c) 2007 Dominik Spies; Copyright (c)
2018 Jasper Verschueren; Copyright (c) 2015 Matt Borgerson; Copyright (c)
2022 Stefan Schmidt, Ryan Wendland; and the other authors named in lwIP's
source files. All rights reserved.

    Redistribution and use in source and binary forms, with or without modification,
    are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.
    3. The name of the author may not be used to endorse or promote products
       derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
    WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
    SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
    EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
    OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
    IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
    OF SUCH DAMAGE.

**nvnetdrv** (nxdk's NIC driver, `lib/net/nvnetdrv/nvnetdrv.c`): Copyright
(c) 2022 Stefan Schmidt, Ryan Wendland. MIT:

    Permission is hereby granted, free of charge, to any person obtaining a copy of
    this software and associated documentation files (the "Software"), to deal in
    the Software without restriction, including without limitation the rights to
    use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
    the Software, and to permit persons to whom the Software is furnished to do so,
    subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
    FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
    COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
    IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
