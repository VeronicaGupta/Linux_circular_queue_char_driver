# Chromebook / FydeOS Notes

The active project environment is FydeOS with a Crostini Debian container.

The maintained instructions are in:

[`README_FYDEOS.md`](README_FYDEOS.md)

The repository root Makefile is FydeOS-safe: `make` builds user-space components and skips kernel-module compilation when a matching kernel build tree is unavailable.
