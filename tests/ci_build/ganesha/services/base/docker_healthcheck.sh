#!/usr/bin/env bash
set -eux -o pipefail

# run any docker entrypoint script from /docker-entrypoint.d/ if exists
# (e.g. for ganesha) and then exec "$@" (e.g. leil-tests)
if [ -d /healthcheck.d/ ]; then
	for f in $(find /healthcheck.d/ -name '*.sh' -print | sort -n); do
		[ -f "${f}" ] || continue
		echo "Running healthcheck script ${f}"
		# shellcheck disable=SC1090
		. "${f}"
	done
fi
exec "$@"
