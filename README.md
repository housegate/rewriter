# SQL Rewriter

## Clone
```shell
git clone --recurse-submodules -b main --depth 1 --shallow-submodules https://github.com/sentioxyz/sql-rewriter.git
cd clickHouse && git submodule update --init --depth=1
```

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
mkdir build
cmake --build build
```
