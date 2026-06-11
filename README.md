# AcyclicJoin SGX Benchmark

This repository contains an Intel SGX benchmark harness for acyclic join
algorithms over projected TPC-DS/TPC-H join trees. The executable is built as an
SGX untrusted app plus an enclave. The app loads integer-projected tables,
passes them into the enclave, and reports output cardinalities, timing
breakdowns, and optional differential-oblivious padding statistics.

## Algorithm Names

The command-line flags use the internal implementation names. In experiment
reports, use the following paper names:

| CLI flag | Internal label | Paper/report name | High-level role |
|---|---|---|---|
| `-JFYan` | `JFYan` | `JFYan` | Bottom-up filtering plus top-down materialization. |
| `-ParYan` | `ParYan` | `ParYan` | Two-phase filtering plus ObliViator-style join. |
| `-ObliYan` | `ObliYan` | `ObliYan` | Relaxed oblivious join baseline. |
| `--all` | all modes | all three | Runs `JFYan`, `ParYan`, and `ObliYan` in sequence. |

## Source Attribution

The `DOJoin/` directory contains code imported from
[`z46wu/DOJoin.git`](https://github.com/z46wu/DOJoin.git). It is kept as a
third-party reference/dependency area for differential-oblivious join related
components. Local benchmark code in this repository wraps and extends the SGX
acyclic join experiments around the implementations in `App/`, `Enclave/`,
`include/`, and `implement/`.

## Repository Layout

| Path | Purpose |
|---|---|
| `App/` | Untrusted SGX app. Parses arguments, loads projected tables, creates the enclave, and prints results. |
| `Enclave/` | SGX enclave entry points and protected execution wrapper. |
| `include/` | Shared headers for join algorithms and profiling helpers. |
| `implement/` | Join algorithm implementations and oblivious primitives. |
| `tpcds/` | TPC-DS tools plus scripts that generate projected benchmark tables. |
| `tpch/` | TPC-H data/tooling used by the TPCH9 benchmark path. |
| `DOJoin/` | Third-party code imported from `z46wu/DOJoin.git`. |
| `exec.sh` | Convenience build-and-run script. |

## Runtime Environment

The project is intended for Linux machines with Intel SGX support.

Recommended environment:

| Component | Requirement |
|---|---|
| OS | Linux x86_64. Ubuntu 20.04/22.04 style environments are typical. |
| SGX | Intel SGX driver/runtime installed and enabled. |
| SGX SDK | Intel SGX SDK, default path `/opt/intel/sgxsdk`. |
| Compiler | GCC/G++ with C++17 support. |
| Build system | CMake 3.15 or newer, Make/Ninja backend. |
| Scripting | Python 3 for TPC-DS projection scripts. |
| Shell | Bash for `exec.sh`. |

Before building, source the SGX SDK environment if your system requires it:

```bash
source /opt/intel/sgxsdk/environment
```

If the SDK is installed somewhere else, set `SGX_SDK` when invoking CMake or
`exec.sh`.

## Build and Run

The easiest path is to use `exec.sh`, which configures CMake, builds the app and
signed enclave, then runs the benchmark from the repository root.

```bash
./exec.sh [benchmark options]
```

Example using built-in sample data:

```bash
./exec.sh -JFYan --profile -t 16
```

Equivalent manual build:

```bash
cmake -S . -B build -DSGX_MODE=HW -DSGX_DEBUG=1 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/App/app -JFYan --profile -t 16
```

Simulation mode can be selected with:

```bash
SGX_MODE=SIM ./exec.sh -JFYan --profile
```

## Data Preparation

Projected datasets are expected to be directories containing `R*.tbl`,
`expected.txt`, and `stats.txt`. The `tpcds/prepare_*_scales.py` scripts generate
raw TPC-DS data when needed and export compact integer projections.

Examples:

```bash
python3 tpcds/prepare_query18_scales.py --scales 1
python3 tpcds/prepare_query64_scales.py --scales 10
python3 tpcds/prepare_query72_scales.py --scales 1 --threshold 50000000
python3 tpcds/prepare_query85_scales.py --scales 1
python3 tpcds/prepare_query85_returns_star_scales.py --scales 1
```

Then run with the corresponding dataset option:

```bash
./exec.sh --sql18 tpcds/sql18_projected/sf_1 --all --profile -t 16 -m 1 --print-limit 0
./exec.sh --sql64 tpcds/sql64_projected/sf_10 --all --profile -t 16 -m 1 --print-limit 0
./exec.sh --sql72 tpcds/sql72_projected/sf_1 --all --profile -t 16 -m 1 --print-limit 0
./exec.sh --sql85 tpcds/sql85_projected/sf_1 --all --profile -t 16 -m 1 --print-limit 0
./exec.sh --sql85-returns-star tpcds/sql85_returns_star_projected/sf_1 --all --profile -t 16 -m 1 --print-limit 0
```

For TPCH9, pass the prepared TPC-H directory:

```bash
./exec.sh --tpch9 tpch/sf_0p01 --all --profile -t 16 -m 1 --print-limit 0
```

TPCH9 enables materialized DO padding by default. To force a specific protected
output size, pass `-tau`:

```bash
./exec.sh --tpch9 tpch/sf_0p01 --all --profile -t 16 -m 1 -tau 15014300 --print-limit 0
```

## Command-Line Parameters

| Parameter | Value | Meaning |
|---|---:|---|
| `-JFYan` | flag | Run `JFYan` only. This is the default mode. |
| `-ParYan` | flag | Run `ParYan` only. |
| `-ObliYan` | flag | Run `ObliYan` only. |
| `--all` | flag | Run all three modes: `JFYan`, `ParYan`, `ObliYan`. |
| `--bench-only` | flag | Return only result dimensions, avoiding full result copy-out to the app. |
| `--no-result` | flag | Alias for `--bench-only`. |
| `--profile` | flag | Enable detailed enclave-side timing and primitive timing output. Also implies `--bench-only`. |
| `--stage-profile` | flag | Enable coarse stage timings without detailed enclave logs. Also implies `--bench-only`. |
| `-t` | integer | Number of OpenMP threads used inside the enclave. Default: `16`. |
| `--thread-sweep` | comma list | Reuse one enclave and run several thread counts, for example `8,16,24,32`. |
| `-m` | integer | Maximum number of result cells copied back to the app for non-benchmark runs. Default: `1000000`. |
| `-tau` | integer | Explicit output size/cap. With materialized DO padding, this requests at least this protected row count. Without padding, it is used as the join output materialization size. |
| `--do-epsilon` | float | Differential-oblivious padding epsilon when `-tau` is not supplied. Default: `1.0`. |
| `--do-delta` | float | Differential-oblivious padding delta when `-tau` is not supplied. Default: `1e-9`. |
| `--materialize-padding` | flag | Physically materialize the DO-protected output row count. |
| `--no-materialize-padding` | flag | Profile exact-size join output without physically adding DO padding. |
| `--print-limit` | integer | Maximum number of output rows printed in non-benchmark mode. Use `0` to print none. Default: `20`. |
| `--random-rows` | integer | Rows per table for synthetic generated trees. Default: `500`. |
| `--key-range` | integer | Random key range for synthetic generated trees. Default: `300`. |
| `--seed` | integer or `random` | Random seed for synthetic generated trees. Default: `1`. |
| `-h`, `--help` | flag | Print app usage. |

## Dataset Options

Only one dataset option may be used per run.

| Option | Argument | Join tree/data source |
|---|---|---|
| none | none | Built-in tiny sample data. |
| `--sql18` | projected dir | TPC-DS Q18 projected tree. |
| `--sql64` | projected dir | TPC-DS Q64 compact projected chain. |
| `--sql72` | projected dir | TPC-DS Q72 red-box star rooted at `catalog_sales`. |
| `--sql85` | projected dir | Full TPC-DS Q85 projected tree. |
| `--sql85-chain3` | projected dir | Three-table Q85 chain: `web_sales -> web_returns -> cd1`. |
| `--sql85-returns-star` | projected dir | Q85 subtree rooted at `web_returns` with `cd1`, `cd2`, `customer_address`, and `reason`. |
| `--tpch9` | TPC-H dir | TPC-H Q9-style tree. Enables materialized padding by default. |
| `--tpch-ternary-l3` | projected dir | Projected ternary level-3 TPC-H tree. |
| `--tpch-binary-l3` | projected dir | Projected binary level-3 TPC-H tree. |
| `--full-ternary-l3` | flag | Synthetic full ternary tree, controlled by random data options. |
| `--star15` | flag | Synthetic root with 15 leaf children. |
| `--star4` | flag | Synthetic root with 4 leaf children. |

## Output Notes

With `--profile` or `--stage-profile`, the app reports:

| Field | Meaning |
|---|---|
| `Result` | Returned row and column count. |
| `ECALL time` | Wall-clock time for the enclave call from the app side. |
| `Join-only time` | Sum of the main join stages when stage timings are available. |
| `Stage timings` | Enclave-side restore, setup, join, padding, and primitive timing metrics. |
| `DO output rows` | Real rows, protected rows, and padding rows when DO statistics are available. |

The expected real row count is read from `expected.txt` in projected dataset
directories and is used for a simple sanity check.
