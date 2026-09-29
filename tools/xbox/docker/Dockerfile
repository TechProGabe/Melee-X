# Melee-X SDK image: nxdk built from source (upstream ships amd64/386 images
# only; this builds natively on arm64 hosts too). The first part matches
# OpenCrossing-Xbox's SDK image line for line, so the two share layers.
# LLVM 21 from apt.llvm.org: Debian's clang 19.1.7 is in the 19.x-20.1.2 range
# nxdk warns about (llvm/llvm-project#134607, miscompiles at -O).
FROM debian:trixie
ARG LLVM=21
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake git bison flex python3 ca-certificates wget gnupg \
      pkg-config file xxd ninja-build \
 && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key > /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
 && echo "deb http://apt.llvm.org/trixie/ llvm-toolchain-trixie-${LLVM} main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      clang-${LLVM} lld-${LLVM} llvm-${LLVM} \
 && rm -rf /var/lib/apt/lists/*
ENV PATH=/usr/lib/llvm-${LLVM}/bin:$PATH
ARG NXDK_SHA=58427c078b4ccb0121f359b6b6c536b3b2914976
RUN git clone --recursive https://github.com/XboxDev/nxdk.git /usr/src/nxdk \
 && cd /usr/src/nxdk && git checkout -q $NXDK_SHA && git submodule update --init --recursive
ENV NXDK_DIR=/usr/src/nxdk
RUN cd /usr/src/nxdk && make tools -j$(nproc)
RUN bash -c 'eval $(/usr/src/nxdk/bin/activate -s); cd /usr/src/nxdk && make NXDK_ONLY=y NXDK_SDL=y NXDK_CXX=y -j$(nproc)'
# nxdk's Cg compiler (tools/cg/linux/cgc) is an i386 binary; on arm64 hosts it
# runs under the VM's qemu-i386 binfmt and only needs the i386 runtime.
RUN dpkg --add-architecture i386 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends libc6:i386 libstdc++6:i386 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
# Melee-X extras: clang/LLVM dev files for tools/lower/disc_lower (LibTooling),
# and GCC (tools/lower/test_lower.py uses its scalar_storage_order as the
# oracle). extract-xiso for xemu test discs comes with nxdk's tools.
ARG LLVM=21
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libclang-${LLVM}-dev libclang-cpp${LLVM}-dev llvm-${LLVM}-dev libzstd-dev zlib1g-dev libxml2-dev python3-pip \
 && rm -rf /var/lib/apt/lists/*
ENV LLVM=/usr/lib/llvm-${LLVM}
