#!/bin/bash

set -eux

gitChangelog=$(./ci/get-changelog.sh)
newVersion=$(./ci/new-version.sh)


newsEntry="\n * SaunaFS (${newVersion}) ($(date '+%Y-%m-%d' -u))\n"

while read -r _ title message; do
	case $title in
		chore*|style*|refactor*|test*|"Merge"|ci*)
			continue
		;;
	esac
	newsEntry+=" - $title $message\n"
done <<< "${gitChangelog}"

# Insert new NEWS entry after the first line
{
    read -r firstLine
    echo "${firstLine}"
    echo -e -n "${newsEntry}"
    cat
} < NEWS > "/tmp/newsTemp"

mv "/tmp/newsTemp" NEWS
