# SQL Rewriter

## HTTPS Clone

```shell
git clone --recurse-submodules -b main --depth 1 --shallow-submodules https://github.com/sentioxyz/sql-rewriter.git
cd clickHouse && git submodule update --init --depth=1
```

## SSH Clone

```shell
git clone --recurse-submodules -b main --depth 1 --shallow-submodules git@github.com:sentioxyz/sql-rewriter.git
cd clickHouse && git submodule update --init --depth=1
```

## Install prerequisites

### Linux/MacOS

```shell
./script.sh prepare
```

### ref
Linux:
<https://clickhouse.com/docs/en/development/build>

MacOS:
<https://clickhouse.com/docs/en/development/build-osx>


## Build

### Linux/MacOS

```shell
./script.sh build
```
