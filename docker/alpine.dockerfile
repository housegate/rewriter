# Use the official Alpine image as the base
FROM alpine:3.20.1

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

# Build sql rewriter
WORKDIR /usr/sql-rewriter

ARG PORT

RUN mkdir build && cmake -S . -B build \
    && cmake --build build

# Expose the port your app runs on
EXPOSE $PORT

# Set entrypoint
ENTRYPOINT ["./build/clickhouse_housegate_rewriter", $PORT]

