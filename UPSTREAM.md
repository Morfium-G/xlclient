# Upstream

This repository is a hard fork of [SatyPardus/wowdecomp](https://github.com/SatyPardus/wowdecomp),
a C++ decompilation of the World of Warcraft 3.3.5a client (build 12340).

## Fork point

```
d36a92c08630c3dd219528337e4c0ef9c0d5745b   2026-10-17
Merge pull request #21 from SatyPardus/original-335
```

Everything before that commit is upstream's work. Everything after is this fork's.

## Tracking upstream

Upstream is not merged. It is kept as a remote and read periodically, and individual commits are
cherry-picked when they are worth taking.

```sh
git remote add upstream https://github.com/SatyPardus/wowdecomp.git
git fetch upstream
git log --oneline main..upstream/main
git cherry-pick <sha>
```

Upstream is active and its ported-function share is still rising, so decompilation fixes are worth
pulling. Anything this fork adds on top -- the mod host, the renderer work -- never goes back and
never comes down.

## Upstream branches

| Branch | What it is |
|---|---|
| `main` | the decompilation, x64 primary target |
| `original-335` | reference material from the original client |
| `vulkan` | upstream's own Vulkan work |
| `mapeditor` | an editor built on the port |
