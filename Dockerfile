# Dockerfile para build e execução do riserSim
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Instala dependências de compilação C++ e Python
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    python3 \
    python3-pip \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Instala versão mais recente do CMake e h5py via pip
RUN pip3 install --no-cache-dir cmake h5py

WORKDIR /app

# Copia arquivos do projeto
COPY . /app/risersim

# Compila o projeto C++ usando o cmake atualizado do pip
RUN /usr/local/bin/cmake -B /app/risersim/build /app/risersim && \
    /usr/local/bin/cmake --build /app/risersim/build --config Release -j$(nproc)

CMD ["python3", "/app/risersim/tools/run_from_aml.py", "--help"]
