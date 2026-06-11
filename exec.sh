#!/usr/bin/env bash
set -euo pipefail

# Build settings can be overridden from the environment, for example:
# SGX_MODE=SIM BUILD_TYPE=Debug ./exec.sh -JFYan --profile
BUILD_DIR="${BUILD_DIR:-build}"
SGX_MODE="${SGX_MODE:-HW}"
SGX_DEBUG="${SGX_DEBUG:-1}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

cmake -S . -B "${BUILD_DIR}" \
  -DSGX_MODE="${SGX_MODE}" \
  -DSGX_DEBUG="${SGX_DEBUG}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

# Run from the project root so relative dataset paths such as
# tpcds/sql18_projected/sf_1 work naturally.
"${BUILD_DIR}/App/app" "$@"
