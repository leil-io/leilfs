#!/bin/bash
git log --cherry-pick --right-only "${1:-stable}"..."${2:-HEAD}" --oneline --pretty='format:%h %s'
