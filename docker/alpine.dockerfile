# Use the official Alpine image as the base
FROM alpine:latest

# Install dependencies
RUN apk update && apk add --no-cache \
    git \
    cmake \
    ninja \
    clang \
    lld \
    nasm \
    yasm \
    gawk \
    rustup \
    python3 \
    py3-pip

# Install Rust
RUN rustup-init -y
ENV PATH="/root/.cargo/bin:${PATH}"
RUN rustup toolchain install nightly-2024-04-01 && rustup default nightly-2024-04-01 && rustup component add rust-src

# Clone ClickHouse repository
RUN git clone --recursive --shallow-submodules https://github.com/ClickHouse/ClickHouse.git

# Build ClickHouse
WORKDIR ClickHouse
RUN mkdir build && cd build \
    && cmake -S .. -B . \
    && cmake --build . --target clickhouse

# Set entrypoint
ENTRYPOINT ["./build/programs/clickhouse", "server"]

