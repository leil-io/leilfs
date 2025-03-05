#!/bin/bash

set -eux

gitChangelog=$(./ci/get-changelog.sh)
version=$(sed -nE 's/^set\(DEFAULT_MIN_VERSION "([^"]+)"\).*/\1/p' ./CMakeLists.txt)


newsEntry="* SaunaFS (${version}) ($(date '+%Y-%m-%d' -u))\n"

while read -r _ title message; do
	case $title in
		chore*|style*|refactor*|test*|"Merge"|ci*)
			continue
		;;
	esac
	newsEntry+=" - $title $message\n"
done <<< "${gitChangelog}"

sed -i "3i ${newsEntry}" NEWS
