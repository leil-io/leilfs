#!/bin/bash
sed -i "s|^set(DEFAULT_MIN_VERSION \".*\")|set(DEFAULT_MIN_VERSION \"$(./ci/new-version.sh)\"\)|" CMakeLists.txt
