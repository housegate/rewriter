# SQL Rewriter

## Clone
```shell
git clone --recurse-submodules -b main --depth 1 --shallow-submodules https://github.com/sentioxyz/sql-rewriter.git
cd clickHouse && git submodule update --init --depth=1
```

## Build
### Linux
```shell
cmake -Bbuild-cmake -H. -GNinja
cmake --build build-cmake
```
### MacOS
```shell
mkdir build
cmake --build build
```
