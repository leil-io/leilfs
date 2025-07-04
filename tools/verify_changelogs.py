#!/usr/bin/env python3

"""
SaunaFS Changelog Verification Tool

This script parses SaunaFS changelog filenames, verifies their components against
file content (first/last IDs and derived timestamps), and checks for
sequence integrity (gaps, overlaps) across multiple changelog files in a directory.
"""

import argparse
import os
import re
from datetime import datetime, timezone, timedelta

# Filename format:
# changelog_ml.sfs.<CLUSTER_ID>.<FIRST_ID>.<LAST_ID>.<BEGIN_EPOCH_UTC>Z[.LIVE | .<END_EPOCH_UTC>Z][.<SUFFIX>]
# SUFFIX: Optional: ".ml.<hostname>" for metaloggers, or just ".<hostname>"
# Example Master: changelog.sfs.alpha01.00000000683DB60E.0000000068F844BB.2025-06-05T1423Z.2025-06-22T1555Z.mds01
# Example ML Live: changelog_ml.sfs.alpha01.00000000683DB60E.UNDEF.2025-06-05T1423Z.LIVE.ml.metalogger1

FILENAME_REGEX = re.compile(
    r"^(changelog(?:_ml)?\.sfs)\."  # 1: base_prefix (changelog.sfs or changelog_ml.sfs)
    r"([^.]+)\."                     # 2: cluster_id
    r"([0-9A-F]{16})\."              # 3: first_id_hex
    r"([0-9A-F]{16}|UNDEF)\."        # 4: last_id_hex ("UNDEF" is kChangelogKeywordUndef)
    r"(\d{4}-\d{2}-\d{2}T\d{4}Z)\."  # 5: begin_epoch_utc
    r"(LIVE|(?:\d{4}-\d{2}-\d{2}T\d{4}Z))"  # 6: state_part ("LIVE" is kChangelogKeywordLive or end_epoch_utc)
    r"\.(mds|mls)"                   # 7: role_suffix (mds or mls, without leading dot)
    r"(?:\.([^.]+))?$"               # 8: optional_hostname_group (includes leading dot if hostname exists, e.g. ".hostname1")
                                     # 9: hostname (if present, captured from group 8)
)
# Constants for keywords (though regex handles them directly)
K_CHANGELOG_KEYWORD_UNDEF = "UNDEF"
K_CHANGELOG_KEYWORD_LIVE = "LIVE"
K_CHANGELOG_SUFFIX_MDS = ".mds"
K_CHANGELOG_SUFFIX_MLS = ".mls"


def parse_epoch_utc_to_datetime(epoch_utc_str):
    """Converts YYYY-MM-DDTHHMMZ string to datetime object."""
    if not epoch_utc_str:
        return None
    try:
        # Python's strptime doesn't have a direct Z for UTC, so we parse and then add tzinfo
        dt = datetime.strptime(epoch_utc_str, "%Y-%m-%dT%H%MZ")
        return dt.replace(tzinfo=timezone.utc)
    except ValueError:
        return None

def epoch_seconds_to_utc_string(epoch_seconds):
    """Converts epoch seconds to YYYY-MM-DDTHHMMZ string."""
    if epoch_seconds is None:
        return ""
    try:
        dt = datetime.fromtimestamp(epoch_seconds, timezone.utc)
        return dt.strftime("%Y-%m-%dT%H%MZ")
    except ValueError:
        return "ERR_TS_CONVERT"

def get_first_last_ids_from_file(filepath):
    """Reads the first and last entry IDs from a changelog file."""
    first_id = None
    last_id = None
    first_line_read = False

    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            last_line_content = ""
            for line_num, line_content in enumerate(f):
                line_content = line_content.strip()
                if not line_content:
                    continue

                if not first_line_read:
                    try:
                        colon_pos = line_content.find(':')
                        if colon_pos != -1:
                            first_id = int(line_content[:colon_pos])
                            first_line_read = True
                    except ValueError:
                        print(f"Warning: Could not parse ID from first line of {filepath}: {line_content}")

                # Keep track of the last valid line for last_id
                last_line_content = line_content

            if last_line_content: # Process the very last non-empty line
                try:
                    colon_pos = last_line_content.find(':')
                    if colon_pos != -1:
                        last_id = int(last_line_content[:colon_pos])
                except ValueError:
                     print(f"Warning: Could not parse ID from last line of {filepath}: {last_line_content}")

    except IOError as e:
        print(f"Error reading file {filepath}: {e}")
        return None, None
    except Exception as e:
        print(f"Unexpected error processing file {filepath}: {e}")
        return None, None

    return first_id, last_id


class ChangelogFile:
    def __init__(self, filepath, components):
        self.filepath = filepath
        self.base_prefix = components.get('base_prefix')
        self.cluster_id = components.get('cluster_id')
        self.first_id_hex = components.get('first_id_hex')
        self.last_id_hex = components.get('last_id_hex')
        self.begin_epoch_utc_str = components.get('begin_epoch_utc_str')
        self.state_part = components.get('state_part')

        self.role_suffix_str = "." + components.get('role_suffix_str') # Store with leading dot, e.g. ".mds"
        self.hostname = components.get('hostname_str') if components.get('hostname_str') else ""

        self.is_metalogger = (self.role_suffix_str == K_CHANGELOG_SUFFIX_MLS)
        # Base prefix check against role suffix for consistency
        if self.is_metalogger and "_ml" not in self.base_prefix:
            self.errors.append(f"Role suffix is '{self.role_suffix_str}' but base prefix '{self.base_prefix}' is not for metalogger.")
        if not self.is_metalogger and "_ml" in self.base_prefix:
            self.errors.append(f"Role suffix is '{self.role_suffix_str}' but base prefix '{self.base_prefix}' is for metalogger.")


        self.first_id_val = int(self.first_id_hex, 16) if self.first_id_hex else None
        self.last_id_val = None
        if self.last_id_hex and self.last_id_hex != K_CHANGELOG_KEYWORD_UNDEF:
            self.last_id_val = int(self.last_id_hex, 16)

        self.is_live = (self.state_part == K_CHANGELOG_KEYWORD_LIVE)
        self.end_epoch_utc_str = None
        if not self.is_live:
            self.end_epoch_utc_str = self.state_part

        self.begin_dt = parse_epoch_utc_to_datetime(self.begin_epoch_utc_str)
        self.end_dt = parse_epoch_utc_to_datetime(self.end_epoch_utc_str) if self.end_epoch_utc_str else None

        self.content_first_id = None
        self.content_last_id = None
        self.errors = []
        self.warnings = []

    def __repr__(self):
        return f"<ChangelogFile: {os.path.basename(self.filepath)}>"

    def analyze_content(self):
        if not os.path.exists(self.filepath):
            self.errors.append(f"File not found at path: {self.filepath}")
            return

        self.content_first_id, self.content_last_id = get_first_last_ids_from_file(self.filepath)

        if self.content_first_id is None:
            self.errors.append("Could not read first ID from content.")
        else:
            if self.first_id_val != self.content_first_id:
                self.errors.append(
                    f"Filename FIRST_ID ({self.first_id_hex} / {self.first_id_val}) "
                    f"mismatches content FIRST_ID ({self.content_first_id})."
                )

            content_begin_utc_str = epoch_seconds_to_utc_string(self.content_first_id)
            if self.begin_epoch_utc_str != content_begin_utc_str:
                self.errors.append(
                    f"Filename BEGIN_EPOCH_UTC ({self.begin_epoch_utc_str}) "
                    f"mismatches derived from content FIRST_ID ({content_begin_utc_str})."
                )

        if self.is_live:
            if self.last_id_hex != "UNDEF":
                self.errors.append(f"LIVE file has LAST_ID '{self.last_id_hex}', should be 'UNDEF'.")
            if self.content_last_id is not None and self.last_id_hex == "UNDEF":
                # It's okay for a .LIVE file to have content and thus a derivable last_id,
                # the filename just says UNDEF until finalization.
                pass
        else: # Finalized
            if self.last_id_hex == "UNDEF":
                self.errors.append("Finalized file has LAST_ID 'UNDEF'.")
            elif self.content_last_id is None:
                self.errors.append("Could not read last ID from content for finalized file.")
            elif self.last_id_val != self.content_last_id:
                self.errors.append(
                    f"Filename LAST_ID ({self.last_id_hex} / {self.last_id_val}) "
                    f"mismatches content LAST_ID ({self.content_last_id})."
                )

            if self.content_last_id is not None:
                content_end_utc_str = epoch_seconds_to_utc_string(self.content_last_id)
                if self.end_epoch_utc_str != content_end_utc_str:
                    self.errors.append(
                        f"Filename END_EPOCH_UTC ({self.end_epoch_utc_str}) "
                        f"mismatches derived from content LAST_ID ({content_end_utc_str})."
                    )

        # Suffix checks already handled during ChangelogFile initialization by checking base_prefix vs role_suffix_str.
        # (The errors are added to self.errors list there if mismatch)

def main():
    parser = argparse.ArgumentParser(description="Verify SaunaFS changelog files.")
    parser.add_argument("--dir", required=True, help="Directory containing changelog files.")
    args = parser.parse_args()

    if not os.path.isdir(args.dir):
        print(f"Error: Directory not found: {args.dir}")
        return

    print(f"Scanning directory: {args.dir}\n")
    parsed_files = []
    total_errors = 0
    total_warnings = 0

    for filename in sorted(os.listdir(args.dir)):
        match = FILENAME_REGEX.match(filename)
        if match:
            groups = match.groups()
            components = {
                'base_prefix': groups[0],
                'cluster_id': groups[1],
                'first_id_hex': groups[2].upper(),
                'last_id_hex': groups[3].upper(), # UNDEF or hex
                'begin_epoch_utc_str': groups[4],
                'state_part': groups[5], # LIVE or YYYY-MM-DDTHHMMZ
                'role_suffix_str': groups[6], # mds or mls
                'hostname_str': groups[8] if groups[8] else "" # group 8 is optional (.hostname), group 9 is hostname itself
            }

            filepath = os.path.join(args.dir, filename)
            cf = ChangelogFile(filepath, components)
            cf.analyze_content()
            parsed_files.append(cf)

            print(f"File: {filename}")
            if cf.warnings:
                for warn in cf.warnings:
                    print(f"  WARN: {warn}")
                    total_warnings +=1
            if cf.errors:
                for err in cf.errors:
                    print(f"  ERROR: {err}")
                    total_errors +=1
            if not cf.errors and not cf.warnings:
                 print("  OK")
            print("-" * 20)

    if not parsed_files:
        print("No changelog files found matching the expected format.")
        return

    # --- Sequence Checking ---
    print("\nSequence Check:")
    # Filter out .LIVE files for sequence checking of finalized logs
    finalized_files = sorted([pf for pf in parsed_files if not pf.is_live and pf.first_id_val is not None],
                             key=lambda x: x.first_id_val)

    if not finalized_files:
        print("No finalized changelog files to check for sequence.")
    else:
        print(f"Found {len(finalized_files)} finalized files for sequence check.")
        for i in range(len(finalized_files)):
            current_cf = finalized_files[i]
            print(f"  - {os.path.basename(current_cf.filepath)} (FirstID: {current_cf.first_id_val}, LastID: {current_cf.last_id_val}, Begin: {current_cf.begin_epoch_utc_str}, End: {current_cf.end_epoch_utc_str})")

            if current_cf.last_id_val is not None and current_cf.first_id_val > current_cf.last_id_val:
                err = (f"File {os.path.basename(current_cf.filepath)}: "
                       f"FIRST_ID ({current_cf.first_id_val}) is greater than LAST_ID ({current_cf.last_id_val}).")
                print(f"    ERROR: {err}")
                total_errors +=1

            if current_cf.begin_dt and current_cf.end_dt and current_cf.begin_dt > current_cf.end_dt:
                err = (f"File {os.path.basename(current_cf.filepath)}: "
                       f"BEGIN_EPOCH_UTC ({current_cf.begin_epoch_utc_str}) is after END_EPOCH_UTC ({current_cf.end_epoch_utc_str}).")
                print(f"    ERROR: {err}")
                total_errors += 1


            if i > 0:
                prev_cf = finalized_files[i-1]
                # Check ID sequence
                if prev_cf.last_id_val is not None and current_cf.first_id_val != prev_cf.last_id_val + 1:
                    gap_or_overlap = "Overlap" if current_cf.first_id_val <= prev_cf.last_id_val else "Gap"
                    err = (f"{gap_or_overlap} detected: Prev LAST_ID ({prev_cf.last_id_val}) "
                           f"-> Current FIRST_ID ({current_cf.first_id_val}). File: {os.path.basename(current_cf.filepath)}")
                    print(f"    ERROR: {err}")
                    total_errors +=1

                # Check time sequence (simplistic check, assumes non-zero duration logs)
                if prev_cf.end_dt and current_cf.begin_dt:
                    # Timestamps are YYYY-MM-DDTHHMMZ, so resolution is 1 minute.
                    # A direct continuation would have prev.end_dt's minute potentially same as current.begin_dt's minute
                    # if the last entry of prev and first of current are in the same minute.
                    # A more robust check: current.begin_dt should be >= prev.end_dt.
                    # If current.begin_dt < prev.end_dt, it's an overlap.
                    # If current.begin_dt is significantly after prev.end_dt (e.g. > 1 minute), it's a gap.
                    time_diff_seconds = (current_cf.begin_dt - prev_cf.end_dt).total_seconds()

                    if time_diff_seconds < 0: # Overlap
                         err = (f"Time overlap: Prev END_EPOCH ({prev_cf.end_epoch_utc_str}) "
                               f"-> Current BEGIN_EPOCH ({current_cf.begin_epoch_utc_str}). File: {os.path.basename(current_cf.filepath)}")
                         print(f"    ERROR: {err}")
                         total_errors +=1
                    elif time_diff_seconds > 60: # Gap of more than 1 minute (timestamps are per minute)
                         # This check might be too strict if rotations can happen with no new data for a while.
                         # However, the ID check is primary for gaps. This is a secondary check.
                         warn = (f"Potential time gap: Prev END_EPOCH ({prev_cf.end_epoch_utc_str}) "
                                f"-> Current BEGIN_EPOCH ({current_cf.begin_epoch_utc_str}). File: {os.path.basename(current_cf.filepath)}")
                         # print(f"    WARN: {warn}") # This can be noisy
                         # total_warnings +=1
                         pass


    print(f"\nVerification finished. Total Errors: {total_errors}, Total Warnings: {total_warnings}")

if __name__ == "__main__":
    main()
