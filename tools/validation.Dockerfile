FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config libsdl2-dev libgl1-mesa-dri \
    xvfb xauth g++-mingw-w64-x86-64-posix ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
CMD ["sh", "tools/validate-platforms.sh"]
