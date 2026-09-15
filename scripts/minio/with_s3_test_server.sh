#!/usr/bin/env bash

set -e

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

cleanup() {
  "${SCRIPT_DIR}/cleanup_s3_test_server.sh"
}
trap cleanup EXIT

# shellcheck source=scripts/run_s3_test_server.sh
source "${SCRIPT_DIR}/run_s3_test_server.sh"
# shellcheck source=scripts/set_s3_test_server_variables.sh
source "${SCRIPT_DIR}/set_s3_test_server_variables.sh"

"$@"
