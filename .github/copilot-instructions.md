---
applyTo: '**/*'
---

# Language-specific instructions

Use specific instructions for C and C++ files from
[`instructions/cpp.instructions.md`](instructions/cpp.instructions.md).

# Building and testing during development

If the file `copilot-user.instructions.md` exists, see it for environment-specific build and test commands.

# Commit messages

* Use conventional commits style for commit messages with the following limits:
  * Title: max 50 characters.
  * Body: max 72 characters per line.
* Use the type of commit and the scope (if any) in the commit title. For example:
  * feat(master): add support for X
  * fix(filesystem): correct Y issue
  * chore: improve comments in file Z
* Use the imperative mood in the subject line.
* Separate the subject from the body with a blank line.
* Reference issues and pull requests in the body when applicable.
