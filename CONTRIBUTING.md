# Contributing to the Raw Input Mouse Fix (DS3 Mod Loader adaptation)

Thanks for wanting to help! You don't need to be an expert, and you don't need to ask before you start.

This repo is slopkhayzo's [Raw Input Mouse Fix](https://github.com/slopkhayzo/DeadSpace3-RawInputMouseFix) moved
onto the DS3 Mod Loader's SDK. A problem you also have with slopkhayzo's standalone fix, without the loader, belongs
in their repo.

## Report a bug

[Open an issue](https://github.com/RHFlyer/Dead-Space-3-Mouse-Fix-Adaptation/issues) and say what you did, what you expected and what happened instead. Add the mod's
version and the DS3 Mod Loader's: select the mod's switch in the Mods panel and the line at the bottom shows its
version; in `ds3_mods.log`, the last line that contains "DS3 Mod Loader v" shows the loader's. Attach
`ds3_mods.log` from the game folder and `plugins\RawInputMouseFix\DS3RawMouse.log`, and say whether
**Use the game's sensitivity slider** is on and where the game's Mouse Sensitivity slider is. Read a log before you
post it.

A bug in the loader itself (the Mods panel, loading mods) belongs on the
[DS3 Mod Loader's issues](https://github.com/RHFlyer/Dead-Space-3-Mod-Loader/issues).

## Send a fix

1. Fork the repo and make a branch for your change.
2. Build it (see "Building from source" in [`README.md`](README.md)) and test it in the game.
3. Open a pull request and say what you changed and how you tested it.

Leave `ds3mod.h` as it is: it is pinned to a loader release, and the build check refuses a changed copy. Keep
slopkhayzo's credit and licence notice (`THIRD-PARTY-NOTICES.txt`, `RawInputMouseFix/LICENSE-RawInputMouseFix.txt`
and the top of `raw_mouse.cpp`). Using AI to help you is fine. Just say so in the pull request, and test what you
send.

## The rules

The DS3 Mod Loader's rules apply here too, and pull requests that break them can't be accepted:

- **Only the player's own mouse aim.** The mod changes how the mouse turns the camera, nothing else.
- **Never touch paid DLC, entitlements, ownership, logins or the game's copy protection (DRM).**
- **No game files** and **no personal data** in the repo.

By sending a pull request you agree that your change is shared under the project's MIT licence
([`LICENSE.txt`](LICENSE.txt)).

Thanks!
RHFlyer
