#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${ANNDATAPP_PROBE_COMPILER_TIME_LOG:-}" ]]; then
    exec "$@"
fi

probe_label="${ANNDATAPP_PROBE_COMPILER_LABEL:-unlabeled}"
exec /usr/bin/time \
    -f "probe_label=${probe_label} compiler_elapsed_s=%e compiler_max_rss_kib=%M command=%C" \
    -o "$ANNDATAPP_PROBE_COMPILER_TIME_LOG" -a -- "$@"
