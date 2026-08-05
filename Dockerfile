# ============================================================================
# Stage 1: builder -- compiles the C++ core (risersim_test_main and friends).
# Everything here (gcc/g++, HDF5 built from source, Catch2/pybind11 FetchContent
# trees, CMake object files) is build-time-only bulk that has no business
# being in the final image.
# ============================================================================
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    python3 \
    python3-pip \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# CMake atualizado via pip (a versão do apt do Ubuntu 22.04 é antiga demais
# para alguns recursos usados no projeto). É a única dependência Python que
# o build em si precisa -- h5py/moorpy são runtime-only, ver stage 2.
RUN pip3 install --no-cache-dir cmake

WORKDIR /app
COPY . /app/risersim

RUN /usr/local/bin/cmake -B /app/risersim/build /app/risersim && \
    /usr/local/bin/cmake --build /app/risersim/build --config Release -j$(nproc)

# ============================================================================
# Stage 2: runtime -- só o binário compilado + as ferramentas Python (ver
# risersim/tools/), sem o toolchain de build inteiro. Mesma imagem base do
# builder (ubuntu:22.04), não python:*-slim: garante compatibilidade de ABI
# glibc/libstdc++ com o binário recém-compilado sem depender de duas
# distribuições diferentes serem compatíveis o bastante -- mais seguro que
# economizar mais alguns MB numa base Debian-slim diferente.
# ============================================================================
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# h5py/moorpy: dependências reais dos scripts em tools/ (ver run_from_aml.py,
# moorpy_warm_start.py) -- nada aqui precisa do toolchain C++.
RUN pip3 install --no-cache-dir h5py moorpy

WORKDIR /app/risersim

# Só o necessário para rodar o pipeline: o binário compilado (run_from_aml.py
# procura em build/bin/risersim_test_main por padrão -- ver find_executable()
# em tools/run_from_aml.py), as ferramentas Python, e o código do MoorPy
# warm-start (spikes/mooring_validation/, importado por moorpy_warm_start.py).
COPY --from=builder /app/risersim/build/bin/risersim_test_main ./build/bin/risersim_test_main
COPY tools/ ./tools/
COPY spikes/mooring_validation/ ./spikes/mooring_validation/

CMD ["python3", "tools/run_from_aml.py", "--help"]
