#!/bin/bash
set -eu -o pipefail

# Define file paths
news_file="NEWS"
changelog_file="debian/changelog"

# Function to format the current date for changelog entries
function get_current_date() {
    echo "$(date -R)"
}

# Get user inputs
echo "Enter version (e.g., 4.0.2):"
read version
echo "Enter urgency (low, medium, high):"
read urgency

# Current date for changelog
current_date="$(get_current_date)"

# Prepare temporary file for editing
tmp_file="/tmp/changelog_${version}"

# Pre-fill template in temporary file
echo "# Write your changes here. One change per line. These entries will be formatted for both the NEWS and Debian changelog files." > "${tmp_file}"

# Open user's preferred editor to edit the file
${EDITOR:-nano} "${tmp_file}"

# Read changes and format them for NEWS and Debian changelog
news_entry="\n * SaunaFS (${version}) ($(date '+%Y-%m-%d'))\n"
changelog_entry="saunafs (${version}) stable; urgency=${urgency}\n"

while IFS= read -r line; do
    # Skip lines starting with '#' (comments) or empty lines
    if [[ $line =~ ^#.* ]] || [[ -z $line ]]; then
        continue
    fi
    # Check if line starts with '(' indicating a new item
    if [[ ${line} == \(* ]]; then
        news_entry+=" - $line\n"
        changelog_entry+="  * $line\n"
    else
        news_entry+="   $line\n"
        changelog_entry+="   $line\n"
    fi
done < "$tmp_file"

changelog_entry+="\n -- LeilFS Team <contact@leil.io>  ${current_date}\n"

# Insert new NEWS entry after the first line
{
    read -r first_line
    echo "${first_line}"
    echo -e -n "${news_entry}"
    cat
} < "${news_file}" > "/tmp/news_temp"
mv "/tmp/news_temp" "${news_file}"

# Insert changelog entry at the top of the changelog file
{
    echo -e "${changelog_entry}"
    cat
} < "${changelog_file}" > "/tmp/changelog_temp"
mv "/tmp/changelog_temp" "${changelog_file}"

# Clean up temporary file
rm "${tmp_file}"

echo "Updated files:"
echo ${news_file}
echo ${changelog_file}
