#!/bin/bash

set -eux

previousRelease=${1:-"stable"}
nextRelease=${2:-"dev"}
changelogWithCommits="$(git log "${previousRelease}..${nextRelease}" --oneline --pretty='format:%h %s')"

# Figure out version to bump
isMajor=0
isMinor=0
isPatch=0

while read -r commit type title; do
	# commit=$(awk '{print $1}' <<< $line)
	type=$(sed -E 's/\([^)]*\)//' <<< "${type}")

	case ${type,,} in
		fix:) isPatch=1
			;;
		feat:) isMinor=1
			;;
		feat!:) isMajor=1
			break
			;;
		fix!:) isMajor=1
			break
			;;
		"breaking change:") isMajor=1
			;;
		*)
			;;
	esac

	# Check for extended description whether it has "BREAKING CHANGE: "
	if eval git log "${commit}" -1 --pretty='%b' | tr '\n' ' ' | grep -q 'BREAKING CHANGE: '; then
		isMajor=1
		break
	fi
done <<< "${changelogWithCommits}"

lastVersion=$(sed -nE 's/^set\(DEFAULT_MIN_VERSION "([^"]+)"\).*/\1/p' ./CMakeLists.txt)

lastMajor="$(cut -d . -f 1 <<< "${lastVersion}")"
lastMinor="$(cut -d . -f 2 <<< "${lastVersion}")"
lastPatch="$(cut -d . -f 3 <<< "${lastVersion}")"

newMajor=$lastMajor
newMinor=$lastMinor
newPatch=$lastPatch

if [[ ${isMajor} == 1 ]]; then
	((newMajor++))
	newMinor=0
	newPatch=0
elif [[ ${isMinor} == 1 ]]; then
	((newMinor++))
	newPatch=0
elif [[ ${isPatch} == 1 ]]; then
	((newPatch++))
else
	exit 1
fi

echo -n "${newMajor}.${newMinor}.${newPatch}"
# git switch dev
# git cherry-pick last commit from newVersionTag-rc1
