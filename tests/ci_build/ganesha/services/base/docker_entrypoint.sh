#!/usr/bin/env bash
set -eu -o pipefail

# run any docker entrypoint script from /docker-entrypoint.d/ if exists
# (e.g. for ganesha) and then exec "$@" (e.g. leil-tests)
if [ -d /docker-entrypoint.d/ ]; then
	for f in $(find /docker-entrypoint.d/ -name '*.sh' -print | sort -n); do
		[ -f "${f}" ] || continue
		echo "Running ${f}"
		# shellcheck disable=SC1090
		. "${f}"
	done
fi
exec "$@"
