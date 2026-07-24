#!/bin/bash

set -eux

headCommit="${2:-HEAD}"
gitChangelog=$(./ci/get-changelog.sh "${1:-stable}" "${headCommit}")
version=$(sed -nE 's/^set\(DEFAULT_MIN_VERSION "([^"]+)"\).*/\1/p' ./CMakeLists.txt)


newsEntry="* LeilFS (${version}) ($(date '+%Y-%m-%d' -u))\n"

isPartOfMerge() {
	commit="$1"
	commit=$(git rev-parse "$commit")

	if git rev-list --first-parent "$headCommit" | grep -qx "$commit"; then
		return 1;
	else
		return 0;
	fi
}

declare -a entries

while read -r shortHash title message; do
	case $title in
		chore*|style*|refactor*|test*|"Merge"|ci*)
			continue
		;;
	esac

	if [[ ! "$title $message" =~ \(#[0-9]+\) ]]; then
		if isPartOfMerge "$shortHash"; then
			# Ignore commits that are part of merges (they will be
			# summarized by the merge commit)
			continue
		fi
	fi

	entries+=(" - $title $message\n")
done <<< "${gitChangelog}"

IFS=$'\n' sortedEntries=($(sort <<<"${entries[*]}"))
unset IFS

for entry in "${sortedEntries[@]}"; do
	newsEntry+="${entry}"
done

sed -i "3i ${newsEntry}" NEWS
