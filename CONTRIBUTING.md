# Contributing

## Commit Messages

This project uses [Conventional Commits](https://www.conventionalcommits.org/). All commits must follow this format:

```
type(scope): description
```

The scope is optional. PR commits are validated by CI and will fail if they don't match.

### Types

| Type | When to use |
|------|-------------|
| `feat` | New feature or capability |
| `fix` | Bug fix |
| `docs` | Documentation only |
| `style` | Formatting, whitespace, no code change |
| `refactor` | Code change that neither fixes a bug nor adds a feature |
| `perf` | Performance improvement |
| `test` | Adding or updating tests |
| `build` | Build system or dependency changes |
| `ci` | CI/CD configuration |
| `chore` | Maintenance tasks |
| `revert` | Reverting a previous commit |

### Breaking Changes

Append `!` after the type to flag a breaking change:

```
feat!: remove legacy FDB loading support
```

## Release Notes

Release notes are auto-generated from commit messages when a version tag (`v*`) is pushed. Commits are categorized by type:

- `feat` -> **Features**
- `fix` -> **Bug Fixes**
- `perf` -> **Performance**
- `docs` -> **Documentation**
- `ci`, `build` -> **Build & CI**
- Breaking changes (`!`) -> **Breaking Changes**

Write clear commit messages -- they become the changelog.
