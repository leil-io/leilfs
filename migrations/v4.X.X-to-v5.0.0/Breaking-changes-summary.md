## Breaking Changes Summary from v5.0.0 to v4.X.X

### Changelog format:
- added a third boolean parameter (0|1) to the LENGTH entries. The LENGTH entries are logged when a file increases its size or a truncate operation finishes. The new parameter is used to decide whether to remove or the chunks further than this new size.

Please review the detailed migration guide and test thoroughly before downgrading.
