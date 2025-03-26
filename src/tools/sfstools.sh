#!/usr/bin/env bash

tool="$(basename $0)"
>&2 echo "Warning: ${tool} is deprecated. Use saunafs ${tool#sfs} instead."
exec saunafs "${tool#sfs}" "$@"
