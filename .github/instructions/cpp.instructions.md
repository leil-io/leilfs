---
applyTo: '**/*.{c,cc,cpp,h,hpp}'
---

# C/C++ standard

* The C code should be written using C23 standard.
* The C++ code should be written using C++23 standard.

# Header inclusions

* All files should include `common/platform.h` as the first include followed by a new line.
  This header is required for platform-specific definitions and configurations.
* Then all the system and external libraries should be included in a separate group.
* Then all the project-specific headers should be included in a separate group.
* The project-specific headers inclusions are relative to the `src` directory.
  For instance, the header in `src/common/foo.h` should be included as `#include "common/foo.h"`.
* All groups should be sorted alphabetically.

# Code style

* The code style for C and C++ is defined in the file `.clang-format` in the root of the project.
