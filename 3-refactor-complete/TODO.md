# TODO: Complete the clang-format follow-up from the audit

## Status

A best-effort manual formatting pass has been applied and pushed because the earlier execution
environment did not have the `clang-format` executable. This pass is not equivalent to running
clang-format and does not certify exact conformance to every LLVM formatting rule.

## Completed

- [x] Checked whether `clang-format` was available in the earlier execution environment.
- [x] Reviewed `3-refactor-complete/.clang-format`, including the three-space indentation rules.
- [x] Applied a best-effort manual formatting pass to all five `.cc` files and four `.h` files.
- [x] Kept the formatting scope to those nine C++ source/header files.
- [x] Compared pre-pass and post-pass C++ token sequences. All 8,694 tokens were identical.
- [x] Checked for tabs and trailing whitespace; none were found.
- [x] Updated `DOXYGEN_AUDIT_REPORT.md` with method, limitations, and verification results.
- [x] Committed and pushed the manual formatting pass to the default branch.

## Remaining: install and run clang-format on Linux

### 1. Install clang-format

On Ubuntu or Debian, open a terminal and run:

```bash
sudo apt-get update
sudo apt-get install clang-format
```

If you need a particular LLVM version, first list the versions available from your configured
package repositories:

```bash
apt-cache search '^clang-format-[0-9]+$'
```

Then install the desired versioned package, for example:

```bash
sudo apt-get install clang-format-18
```

Use the matching versioned executable (for example, `clang-format-18`) in the commands below
if the package does not provide an unversioned `clang-format` command. Check which executable
and version are installed:

```bash
clang-format --version
command -v clang-format
```

If you installed a versioned package, substitute its command name in the checks and formatting
commands below. Record the actual version in the audit report. The repository's
`.clang-format` file is the formatting configuration; it is not itself an executable.

### 2. Format the nine C++ files

Run these commands from the repository root. The `-style=file` option tells clang-format to
read `3-refactor-complete/.clang-format`. The `-i` option edits the files in place.

```bash
clang-format -i -style=file \
  3-refactor-complete/HeartbeatRunner.cc \
  3-refactor-complete/RedisClient.cc \
  3-refactor-complete/RedisHeartbeatManager.cc \
  3-refactor-complete/RedisJobStatus.cc \
  3-refactor-complete/heartbeat.cc \
  3-refactor-complete/HeartbeatRunner.h \
  3-refactor-complete/RedisClient.h \
  3-refactor-complete/RedisHeartbeatManager.h \
  3-refactor-complete/RedisJobStatus.h
```

For a versioned executable, replace `clang-format` with its name, such as `clang-format-18`.

### 3. Verify formatting and inspect the diff

Check that the files are formatted according to the configuration:

```bash
clang-format --style=file --dry-run --Werror \
  3-refactor-complete/HeartbeatRunner.cc \
  3-refactor-complete/RedisClient.cc \
  3-refactor-complete/RedisHeartbeatManager.cc \
  3-refactor-complete/RedisJobStatus.cc \
  3-refactor-complete/heartbeat.cc \
  3-refactor-complete/HeartbeatRunner.h \
  3-refactor-complete/RedisClient.h \
  3-refactor-complete/RedisHeartbeatManager.h \
  3-refactor-complete/RedisJobStatus.h
```

Then inspect the exact changes:

```bash
git diff --check
git diff -- 3-refactor-complete/
```

Confirm that only the intended formatting changes occurred. In particular, compare the C++
token sequences before and after formatting (ignoring whitespace and comments) to ensure no
program tokens changed. Do not blindly accept unexpected logic, declaration, or literal changes.

### 4. Update the audit and commit

- [ ] Record the installed clang-format version and the exact command used in
      `DOXYGEN_AUDIT_REPORT.md`.
- [ ] Record the result of the dry-run check and token comparison.
- [ ] Review the diff and confirm that no unrelated files changed.
- [ ] Commit and push the formatter-generated changes only after all formatting checks pass.

## Other remaining checks

- [ ] Build the project and run the available unit/integration tests.
- [ ] Run Doxygen, if configured, and review generated documentation.

## Constraints

- Do not claim that clang-format ran unless its execution and version are verified.
- Do not make semantic code changes as part of this formatting task.
- Do not claim compilation, Doxygen, or tests passed unless they are actually run.
- Keep `RedisJobScripts.inc`, the README, and unrelated files outside the formatting scope.
