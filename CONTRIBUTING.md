# Contributing

Changes to this fork should target `main` and address a focused SDK behavior,
build, dependency, test, or documentation concern. Preserve upstream authorship
when carrying an upstream change into the fork.

## Verification

Install the pinned development tools and run the repository check from Windows
PowerShell:

```powershell
python -m pip install -r scripts/requirements-dev.txt
.\scripts\verify.ps1
```

The check validates C and C++ formatting with clang-format 22, Python lint and
formatting with Ruff 0.15.21, Vulkan dependency pins, Python tests, the commit
subject, and Git whitespace.

Build or platform changes also require the affected CMake preset and tests.
State which configurations were exercised when submitting the change.

## Commit subjects

Use `type: imperative summary`: one ASCII line, 50 characters or fewer
including the type, with exactly one space after the colon. Do not use a body,
parentheses, or trailers. Preserve another contributor's credit with Git author
metadata rather than a commit-message trailer.

Choose one type from this fixed list:

- `runtime` runtime services and compatibility behavior.
- `codegen` guest-code generation and generated-source handling.
- `platform` platform-specific runtime integration.
- `build` CMake, installation, packaging, and build drivers.
- `deps` dependency and submodule revisions.
- `tools` repository tooling.
- `docs` documentation and contributor guidance.
- `ci` hosted checks and automation.
- `test` test fixtures and harnesses.
- `chore` repository housekeeping with no single code area.
- `refactor` behavior-preserving changes spanning areas.

Prefer the owning area over the kind of change. Use only the listed types;
`feat` and `fix` are not accepted aliases.

## Upstream reference

The upstream [Contributing Guide](https://github.com/rexglue/rexglue-sdk/wiki/Development/Contributing)
documents build prerequisites, code style, formatting, Git setup, and pull request mechanics.
