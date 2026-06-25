---
name: code-formatter
description: Instructions for formatting code using git clang-format with the project's specific style configuration. Use when code needs to be formatted before submission or when a user asks about code style.
---

# Code Formatter Skill

This skill provides instructions for formatting code in the Android Emulator project using `git clang-format` and the project-specific `.clang-format` file.

## Core Procedures

### Formatting Already Committed Code
**MANDATORY:** `git clang-format` should be run on code that is already committed (not just staged or unstaged) to ensure all changes are properly captured and formatted.

To format the changes in the most recent commit (relative to its parent):

```bash
git clang-format --style=file:android/.clang-format HEAD~1
```

To format the changes introduced **by** a specific commit hash:

```bash
git clang-format --style=file:android/.clang-format <commit_hash>~1
```

(Using `~1` ensures the tool compares the commit against its parent, capturing the changes made in that commit.)

**Note:** After running these commands, the formatting changes will be present in your working tree as unstaged modifications. You must stage them and amend your commit:

```bash
git add -u
git commit --amend --no-edit
```

### Formatting Changes Between Branches
To format all changes between your current branch and another branch (e.g., `main`):

```bash
git clang-format --style=file:android/.clang-format main
```

## Contextual Notes
- All formatting commands MUST be executed from the `external/qemu` directory.
- The style file used is `android/.clang-format`.
- `git clang-format` only formats the lines that have changed, not the entire file. This helps maintain a clean diff and avoids unnecessary noise in reviews.
- If you want to see the changes it would make without applying them, add the `--diff` flag.
