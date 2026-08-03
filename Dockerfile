# Dockerfile para build e execução do riserSim
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Instala dependências de compilação C++ e Python
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    python3 \
    python3-pip \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copia arquivos do projeto
COPY . /app/risersim

# Compila o projeto C++
RUN cmake -B /app/risersim/build /app/risersim && \
    cmake --build /app/risersim/build --config Release -j$(nproc)

CMD ["python3", "/app/risersim/tools/run_from_aml.py", "--help"]
