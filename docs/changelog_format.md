# SaunaFS Changelog Filename Format

This document describes the robust changelog filename format used by SaunaFS components, including Master MDS, Shadow MDS, and Metaloggers. The filenames are designed to be stable, sortable, unique, and derived from the actual changelog content.

**This documentation refers to changes that modify filenames ONLY. File contents are NOT modified by this naming convention change.**

## Filename Format Definition

The standard format for changelog files is as follows:

`<base_prefix>.<cluster_id>.<first_id_hex>.<last_id_hex>.<begin_epoch_utc>Z.<state_specific_part>.<role_suffix>[.<hostname_suffix>]`

Where:

*   **`<base_prefix>`**:
    *   `changelog.sfs`: For Master MDS and Shadow MDS.
    *   `changelog_ml.sfs`: For Metaloggers.
*   **`<cluster_id>`**: A unique identifier for the SaunaFS cluster (e.g., "alpha01", "eu1-prod"). This is sourced from the service's configuration file.
*   **`<first_id_hex>`**: The first 64-bit incremental ID (typically an epoch timestamp) from the first entry in the changelog file, represented as a 16-character uppercase hexadecimal string, zero-padded (e.g., `00000000683DB60E`).
*   **`<last_id_hex>`**: The last 64-bit incremental ID from the last entry in the changelog file, represented similarly to `<first_id_hex>`. For `.LIVE` files, this field will be the literal string `UNDEF`.
*   **`<begin_epoch_utc>Z`**: The UTC timestamp corresponding to `<first_id_hex>`, formatted as `YYYY-MM-DDTHHMMZ` (e.g., `2025-06-05T1423Z`). Seconds are truncated.
*   **`<state_specific_part>`**:
    *   For finalized (closed) files: The UTC timestamp corresponding to `<last_id_hex>`, formatted as `YYYY-MM-DDTHHMMZ` (e.g., `2025-06-22T1555Z`). This is the `<end_epoch_utc>Z`.
    *   For live (actively being written) files: The literal string `LIVE`.
*   **`<role_suffix>`**: A mandatory suffix indicating the role of the node that generated the log:
    *   `.mds`: For Master MDS and Shadow MDS.
    *   `.mls`: For Metaloggers.
*   **`[.<hostname_suffix>]`**: An optional suffix providing the hostname of the node that generated the file (e.g., `.mds01`, `.metalogger-alpha-3`). This is for clarity and uniqueness, especially in environments where multiple instances of a service might (even if temporarily) write to the same shared storage.

## Field Derivation and Rules

*   **IDs from Content**: `FIRST_ID` and `LAST_ID` (and thus their hex representations) *must* be derived from the actual first and last log entry lines within the file. They are not arbitrary.
*   **Timestamps from IDs**: `BEGIN_EPOCH_UTC` and `END_EPOCH_UTC` *must* be derived by converting the corresponding `FIRST_ID` and `LAST_ID` (which are epoch timestamps) to the specified UTC string format. System clock is NOT used for these filename timestamps.
*   **`UNDEF`**: The literal string `UNDEF` is used for `<last_id_hex>` in filenames that are currently being written (i.e., `.LIVE` files).
*   **`LIVE`**: The literal string `LIVE` is used as the `<state_specific_part>` for files actively being written. Upon finalization, this part is replaced by the `<end_epoch_utc>Z` string, and the `<last_id_hex>` is updated from `UNDEF` to the actual last ID hex.
*   **Role Suffixes**: The `.mds` or `.mls` suffix is mandatory and clearly distinguishes the origin of the changelog.
*   **Hostname Suffix**: While optional, including the hostname is strongly recommended for operational clarity.

## Example Filenames

*   **Live file from Metalogger `ml-node-01` in cluster `prodcluster`**:
    `changelog_ml.sfs.prodcluster.000000007B1A2C3D.UNDEF.2024-07-15T1030Z.LIVE.mls.ml-node-01`

*   **Finalized file from Metalogger `ml-node-01`**:
    `changelog_ml.sfs.prodcluster.000000007B1A2C3D.000000007B1F9E8A.2024-07-15T1030Z.2024-07-15T1100Z.mls.ml-node-01`

*   **Finalized file from Master MDS `mds-main` in cluster `prodcluster` (no hostname suffix for brevity in example)**:
    `changelog.sfs.prodcluster.000000007B2A1B0C.000000007B2D4F5E.2024-07-16T0800Z.2024-07-16T0900Z.mds`

## ASCII Art Flow (Filename Lifecycle)

1.  **File Creation (Live State):**
    A new changelog is opened. The first entry's ID determines `FIRST_ID` and `BEGIN_EPOCH_UTC`. `LAST_ID` is `UNDEF`, and state is `LIVE`.

    ```
    +-------------------------------------------------------------------------------------------------+
    | changelog.sfs.<cluster>.<first_id>.<UNDEF>.<begin_utc>Z.LIVE.<role_suffix>[.<hostname>]         |
    | (Example: changelog.sfs.alpha01.001.UNDEF.2023-01-01T1000Z.LIVE.mds.mds01)                     |
    +-------------------------------------------------------------------------------------------------+
    ```

2.  **File Finalization (Rotation):**
    When the changelog is rotated (closed), the file is renamed.
    *   The actual last entry's ID is read to determine `LAST_ID` and `END_EPOCH_UTC`.
    *   `UNDEF` is replaced with `<last_id_hex>`.
    *   `LIVE` is replaced with `<end_epoch_utc>Z`.

    ```
                    |
                    v  (Rotation Triggered)
    +-------------------------------------------------------------------------------------------------------+
    | changelog.sfs.<cluster>.<first_id>.<last_id>.<begin_utc>Z.<end_utc>Z.<role_suffix>[.<hostname>]       |
    | (Example: changelog.sfs.alpha01.001.005.2023-01-01T1000Z.2023-01-01T1059Z.mds.mds01)                  |
    +-------------------------------------------------------------------------------------------------------+
    ```
    A new `.LIVE` file is then started for subsequent entries.

## Rationale

*   **Sortability**: Filenames are naturally sortable by time and sequence due to the inclusion of IDs and timestamps.
*   **Uniqueness**: The combination of cluster ID, IDs, timestamps, role, and hostname ensures high probability of uniqueness across a distributed system.
*   **Durability/Self-Description**: Key metadata (IDs, time range, origin) is embedded in the filename, aiding diagnostics and manual inspection.
*   **Content-Derived**: Critical components (`FIRST_ID`, `LAST_ID`, and their timestamps) are derived from the file's actual content, not just system clock at time of creation/rotation, making them robust against clock Skew for sequencing.
*   **Clear Role Identification**: The `.mds` and `.mls` suffixes clearly distinguish between logs from metadata servers and metaloggers.

## Configuration for Metaloggers

Metaloggers (`sfsmetalogger`) also require the `CLUSTER_ID` to be set in their configuration file (`sfsmetalogger.cfg`) to correctly name their changelog files. Example:

```
# In sfsmetalogger.cfg
CLUSTER_ID = mycluster01
```

This ensures metalogger changelogs are associated with the correct cluster.

## Symlink (Optional)

For operational convenience, an optional symlink (e.g., `changelog.sfs.latest` or `changelog_ml.sfs.latest.<hostname>`) may point to the most recently finalized changelog file for a given node type/hostname. This is not part of the core filename format itself but can be a management utility.
