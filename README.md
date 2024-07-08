# SQL Rewriter

## Clone
```shell
git clone --recurse-submodules -b main --depth 1 --shallow-submodules https://github.com/sentioxyz/sql-rewriter.git
cd clickHouse && git submodule update --init --depth=1
```

## Install prerequisites
Linux:
https://clickhouse.com/docs/en/development/build

MacOS:
https://clickhouse.com/docs/en/development/build-osx 


## Build
### Linux
```shell
export CC=clang-18
export CXX=clang++-18
sudo update-alternatives --config c++
cmake -Bbuild -H. -GNinja
cmake --build build
```
### MacOS
```shell
cmake -Bbuild -H. -GNinja
cmake --build build
```
