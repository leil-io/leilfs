#!/bin/bash
git log "${1:-stable}".."${2:-HEAD}" --oneline --pretty='format:%h %s'
