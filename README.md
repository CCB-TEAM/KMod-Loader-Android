# kmod loader (Android)

A Lua mod loader for the card game KARDS on Android. Mods are Lua scripts that read and call the game's own objects through Unreal's reflection system, and can load assets from the game's paks. No root is needed: the loader is added to the game's APK, which you rebuild and sign yourself.

- Lua 5.4 mods, packaged as `.kmod` (a zip) with `mod.json`, settings, localization and versioned dependencies (`kmod>=0.2.0`, `kards>=1.61`).
- Pak mods in `kmod/paks`, also shipped inside `.kmod` packages (checked by SHA-256, confirmed before they replace files).
- A Mods page inside the game's own Settings screen.
- Logging, a crash handler, a command console and a developer build with a floating console.

**Documentation:** <https://ccb-team.github.io/kmod-docs/> (source: [CCB-TEAM/kmod-docs](https://github.com/CCB-TEAM/kmod-docs)): quick start, mod format, Lua API, C++ mods, console commands, debugging and building.

## Not included

**Installer:** `installer/` is an Android app (kmod installer) that checks an installed Unreal game, injects the loader and installs the result on the phone. See the [releases](https://github.com/CCB-TEAM/KMod-Loader-Android/releases) and the documentation.

This repository contains only the loader's source. It does **not** contain the game, a patched APK, the game's C++ SDK dump, or any signing key. You need your own KARDS APK from an official source and your own SDK dump (Dumper-7 / MobileDumper-7); see the "Building the loader" page of the documentation. Do not distribute patched APKs.

## Layout

```
build.ps1        build + repack + sign (-Flavor dev|release)
loader/          libkmod.so: the shell loaded from SplashActivity
core/            libkmodcore.so: ProcessEvent hook, Lua VM, reflection, logging
core/lua/        modloader.lua, modmanager.lua (Mods page), commands.lua (console commands)
overlay/         developer-build floating console (Java)
examples/        example mods (card_art: https://github.com/CCB-TEAM/kmod-card-art) and native examples
tools/           DexTool (patches SplashActivity), repack.py
(documentation: https://github.com/CCB-TEAM/kmod-docs)
```

KARDS is a trademark of its owners; this project is not affiliated with or endorsed by them.

Licensed under the MIT License (see `LICENSE`). Lua is MIT-licensed; the build currently takes its sources from UE4SS's `deps/first/LuaRaw` (MIT).
