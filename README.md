# WarcraftXL / client

A moddable World of Warcraft 3.3.5a client, built from source.

This is a hard fork of [wowdecomp](https://github.com/SatyPardus/wowdecomp) -- a C++ decompilation
of the original `Wow.exe` (build 12340) that compiles into a complete, playable executable. The
fork adds a mod host: named extension points in the client, a service registry, and a loader for
mods built against [WarcraftXL/sdk](https://github.com/WarcraftXL/sdk).

Upstream's own README is kept as [README.upstream.md](README.upstream.md). Provenance and licensing
are in [NOTICE.md](NOTICE.md); the fork point and how upstream is tracked are in
[UPSTREAM.md](UPSTREAM.md).

## Why a source fork

On the stock `Wow.exe` a mod has no choice but to inject a DLL, scan for signatures and detour
addresses. With the source, modding stops being an intrusion: the extension points are declared in
the client, the data they carry is chosen rather than reverse-engineered, and a mod is compiled
against a contract instead of a memory layout.

That contract is deliberately kept out of this repository. It lives in `WarcraftXL/sdk` so that the
same mod source also builds against `WarcraftXL/core`, which serves the stock client by detour.

## Status

Early. The fork point is fresh and the mod host is not written yet.

## Building

See [README.upstream.md](README.upstream.md). Building requires an original 3.3.5a client
installation; none of its files are distributed here.
