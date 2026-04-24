<!-- omit in toc -->
# Contributing to LeilFS

First off, thanks for taking the time to contribute!

All types of contributions are encouraged and valued. See the [Table of
Contents](#table-of-contents) for different ways to help and details about how
this project handles them. Please make sure to read the relevant section before
making your contribution. It will make it a lot easier for us maintainers and
smooth out the experience for all involved. The community looks forward to your
contributions.

> And if you like the project, but just don't have time to contribute, that's
> fine. There are other easy ways to support the project and show your
> appreciation, which we would also be very happy about:
> - Star the project
> - Refer this project in your project's readme
> - Mention the project at local meetups and tell your friends/colleagues

<!-- omit in toc -->
## Table of Contents

- [I Have a Question](#i-have-a-question)
- [I Want to Contribute](#i-want-to-contribute)
  - [Reporting Bugs](#reporting-bugs)
  - [Suggesting Enhancements](#suggesting-enhancements) <!-- - [Improving The
    Documentation](#improving-the-documentation) -->
  - [Grammar/Typos/Nitpicks](#grammar)
- [Styleguides](#styleguides)
  - [Commit Messages](#commit-messages)
  - [Conventional Commits Specification](#conventional-commits-specification)
  - [Code Style Guides](#code-style-guides)
  - [Feature Level Atomicity to Keep the History
    Clean](#feature-level-atomicity-to-keep-the-history-clean)
  - [Break the Build, Then Fix It!](#break-the-build-then-fix-it)
  - [Definition of Done](#definition-of-done)
- [Attribution](#attribution)

## I Have a Question

> If you want to ask a question, we assume that you have read the available
> [Documentation](https://docs.leil.io) and the [man files].

Before you ask a question, it is best to search for existing
[Issues](https://github.com/leil-io/leilfs/issues) that might help you. In case
you have found a suitable issue and still need clarification, you can write your
question in this issue. It is also advisable to search the internet for answers
first.

If you then still feel the need to ask a question and need clarification, we
recommend the following:

- Open an [Issue](https://github.com/leil-io/leilfs/issues/new).
- Provide as much context as you can about what you're running into.
- Include all the technical details that seem relevant e.g. project version (s),
  platform (if needed).

We will then take care of the issue as soon as possible.

You might also consider joining the [Matrix chat](https://matrix.to/#/#leil:matrix.org)
to ask questions and discuss the project.

## I Want to Contribute

> ### Legal Notice
> When contributing to this project, you must agree that you have authored 100%
> of the content and that you have the necessary rights to the content.

### Reporting Bugs

<!-- omit in toc -->
#### Before Submitting a Bug Report

A good bug report shouldn't leave others needing to chase you up for more
information. Therefore, we ask you to investigate carefully, collect information
and describe the issue in detail in your report. Please complete the following
steps in advance to help us fix any potential bug as fast as possible.

- Make sure that you are using the latest version.
- Determine if your bug is really a bug and not an error on your side e.g. using
  incompatible environment components/versions (Make sure that you have read the
  [documentation](https://docs.leil.io) and the [man
  files](https://github.com/leil-io/leilfs/tree/main/doc). If you are looking
  for support, you might want to check [this section](#i-have-a-question)).
- To see if other users have experienced (and potentially already solved) the
  same issue you are having, check if there is not already a bug report existing
  for your bug or error in the [issue
  tracker](https://github.com/leil-io/leilfs/issues?q=is%3Aissue+label%3Abug).
- Also make sure to search the internet (including Stack Overflow) to see if
  users outside the GitHub community have discussed the issue.
- Collect information about the bug:
  - Stack trace (Traceback)
  - OS, Platform and Version
  - Version of the interpreter, compiler, SDK, runtime environment, package
    manager, depending on what seems relevant.
  - Possibly your input and the output
  - Can you reliably reproduce the issue? And can you also reproduce it with older
  versions?

<!-- omit in toc -->
#### How Do I Submit a Good Bug Report?

We use GitHub issues to track bugs and errors. If you run into an issue with the
project:

- Open an [Issue](https://github.com/leil-io/leilfs/issues/new). (Since we
  can't be sure at this point whether it is a bug or not, we ask you not to talk
  about a bug yet and not to label the issue.)
- Explain the behavior you would expect and the actual behavior.
- Please provide as much context as possible and describe the *reproduction
  steps* that someone else can follow to recreate the issue on their own. This
  usually includes your code. For good bug reports you should isolate the
  problem and create a reduced test case.
- Provide the information you collected in the previous section.

Once it's filed:

- The project team will label the issue accordingly.
- A team member will try to reproduce the issue with your provided steps. If
  there are no reproduction steps or no obvious way to reproduce the issue, the
  team will ask you for those steps and mark the issue as `needs-repro`. Bugs
  with the `needs-repro` tag will not be addressed until they are reproduced.
  After some time without reproduction, we will probably close the issue.
- If the team is able to reproduce the issue, it will be marked as a `bug`, as
  well as possibly other tags (such as `critical`).
- The team will then work on fixing the issue and will keep you updated on the
  progress.
- Alternatively, you can also contribute a fix for the issue. In this case, you
  should follow the guidelines for [contributing code](#contributing-code).

<!-- You might want to create an issue template for bugs and errors that can be
used as a guide and that defines the structure of the information to be
included. If you do so, reference it here in the description. -->


### Suggesting Enhancements

This section guides you through submitting an enhancement suggestion for
LeilFS, **including completely new features and minor improvements to existing
functionality**. Following these guidelines will help maintainers and the
community to understand your suggestion and find related suggestions.

<!-- omit in toc -->
#### Before Submitting an Enhancement

- Make sure that you are using the latest version.
- Read the [documentation](https://docs.leil.io) carefully and find out if the functionality is
  already covered, maybe by an individual configuration.
- Perform a [search](https://github.com/leil-io/leilfs/issues) to see if the
  enhancement has already been suggested. If it has, add a comment to the
  existing issue instead of opening a new one.

<!-- omit in toc -->
#### How Do I Submit a Good Enhancement Suggestion?

Enhancement suggestions are tracked as [GitHub issues](https://github.com/leil-io/leilfs/issues).

- Use a **clear and descriptive title** for the issue to identify the
  suggestion.
- Provide a **step-by-step description of the suggested enhancement** in as many
  details as possible.
- **Describe the current behavior** and **explain which behavior you expected to
  see instead** and why. At this point, tell which alternatives do not work for
  you. You might want to use images and/or animations to enhance the description
  of your ideas.
- **Explain why this enhancement would be useful** to most LeilFS users. Point
  to other projects that have solved similar issue better and would serve as
  inspiration.

<!-- ### Improving The Documentation TODO Updating, improving and correcting the
documentation -->

### Grammar/Typos/Nitpicks

If you notice a typo, grammatical error, or potentially a small improvement to
make in the code, we encourage you to post it in the mega issue for
[grammar/typos/nitpicks](https://github.com/leil-io/leilfs/issues/6) instead
of creating a pull request or another issue.

This is to to keep both the issue tracker and the pull requests clean and
focused. Each release we will address the issues in this mega issue and include
the fixes in one commit. We will credit all contributors in the commit message
as co-authors.

### Contributing code/PR's for a feature/bug fix

The best way to contribute is by creating a PR. However, before you start
developing, ask us first if it's something we would accept (so to not waste your
time or the team's).

Currently, the preferred ways to indicate you want to contribute code are (in
order):
* Commenting on a specific Github issue
* [Matrix](https://matrix.to/#/#leil:matrix.org),
* [Email](mailto:hello@leil.io?subject=RFI),

If you get an OK from us, please read below for more details.

## Style guides

### Commit Messages

- **Title:** Give a descriptive summary, use [conventional commits
 specification](#conventional-commits-specification).
- Imperative, start uppercase, no period at the end, no more than 50
 chars.
- Remember blank line between title and body.
- **Body:** Explain why and what (not how), include task ID, wrap at 72
 chars.
- **At the end:** Include Co-authored-by for all contributors.
- Remember at least one blank line before it.
- Format: Co-authored-by: name <user@users.noreply.github.com>

### Conventional Commits Specification

The Conventional Commits specification is a lightweight convention on top of
commit messages. It provides an easy set of rules for creating an explicit
commit history, making it easier to write automated tools on top of. This
convention integrates with SemVer, by describing the features, fixes, and
breaking changes made in commit messages.

The commit message should be structured as follows:

> &lt;type&gt; [(optional scope)]: &lt;descriptive summary&gt;
>
> &lt;body&gt;
>
> [optional footer(s)]

The commit contains the following structural elements, to communicate intent to
the consumers of your library:

* **type:** is based on the [Angular
  Convention](https://github.com/angular/angular/blob/22b96b9/CONTRIBUTING.md#-commit-message-guidelines)
  and has following main options: **fix:, feat:, build:, chore:, ci:, docs:,
  style:, refactor:, perf:, test:,** and others. A commit of the type **fix:**
  patches a bug in your codebase (this correlates with [PATCH in Semantic
  Versioning](http://semver.org/#summary)), as a commit of the type **feat:**
  introduces a new feature to the codebase (this correlates with [MINOR in
  Semantic Versioning](http://semver.org/#summary)).
* **scope** is one or more component name(s), referring to domains changed
  (mount, chunkserver, nfs-ganesha, tests, master, ...)
* **footer(s)** - **BREAKING CHANGE: &lt;description&gt;** introduces a breaking
  change (correlating with [MAJOR in Semantic
  Versioning](http://semver.org/#summary)). A Footers other than **BREAKING
  CHANGE:**  may be provided, following a convention similar to [git trailer
  format](https://git-scm.com/docs/git-interpret-trailers).

### Code Style Guides

You should format your code using clang-format with the included
`.clang-format` file, which specifies the settings we use (We mostly use the
Google style with a few modifications). See the
[clang-format](https://clang.llvm.org/docs/ClangFormat.html) documentation for
more details.

### Feature Level Atomicity to Keep the History Clean

When introducing a new feature, aim to propose the pull request to protected
branches (e.g., dev, main) in a clean and organized manner. This often involves
squashing WIP (Work In Progress) commits to include only those with relevant new
code. In such cases, avoid retaining commits that simply amend earlier ones. For
example, if you have a pull request with 10 commits, and commit 2 contains an
error that was fixed in commit 8, it is unnecessary to keep both commits in
protected branches. Only the code that correctly adds value should be preserved.

The level of commit atomicity differs for existing code and is usually based on
bug fixes, feature improvements, and similar factors. It is common to have
commits that amend older ones already present in protected branches.

### Break the Build, Then Fix It

If the changes you introduce break the pipeline, it is your responsibility, as
the author of those changes, to fix it. While you can seek assistance from the
team, and seeking assistance is welcome, the primary responsibility for fixing
the issue rests with you. Restoring the functionality of the pipeline is a top
team priority, and help is available as soon as possible when needed.

Code reviews for changes within a broken pipeline are counterproductive; no code
within a broken pipeline will be reviewed or approved for merging. If a merge
has already occurred, it must be promptly fixed or reverted.

Despite what has been mentioned, don't hesitate to intentionally break the
pipeline – in fact, we encourage it! Please, go ahead and break it! Then,
proceed to promptly fix it.

### Definition of Done

The team should reach a consensus on what "done" means to ensure consistent
quality and a shared understanding among all members. A developer submits a
change for peer review once they deem the task complete and ready for production
delivery. To facilitate this, we provide a checklist for a development task that
must be satisfied before it is considered for peer review:

- The new code should have some test coverage, either unit or integration tests.
- All automated tests must pass.
- The build job must be in a healthy state. <!-- omit in toc -->

### Developer Certificate of Origin

Before we accept your PR, we want you to sign-off your commit(s) with a valid
GPG key. See the git manual for more info. This indicates that you accept the
text below.

```
Developer Certificate of Origin
Version 1.1

Copyright (C) 2004, 2006 The Linux Foundation and its contributors.
1 Letterman Drive
Suite D4700
San Francisco, CA, 94129

Everyone is permitted to copy and distribute verbatim copies of this
license document, but changing it is not allowed.


Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the same open source license (unless I am
    permitted to submit under a different license), as indicated
    in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it, including my sign-off) is
    maintained indefinitely and may be redistributed consistent with
    this project or the open source license(s) involved.
```

## Attribution

- [Chris Beams - How to Write a Git Commit
  Message](https://chris.beams.io/posts/git-commit/)
- [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/)
- [Semantic Versioning](https://semver.org/#summary)
- [Angular
  Convention](https://github.com/angular/angular/blob/22b96b9/CONTRIBUTING.md#-commit-message-guidelines)
- [Git Trailer Format](https://git-scm.com/docs/git-interpret-trailers)
