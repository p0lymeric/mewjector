# Mewjector

A DLL mod loader for [Mewgenics](https://store.steampowered.com/app/686060/Mewgenics/). Drop it into your Mewgenics install directory and it automatically loads mod DLLs at startup.

Designed to work alongside [Mewtator](https://www.nexusmods.com/mewgenics/mods/1), the Mewgenics mod manager. When both are installed, they handshake automatically through `chainloader.ini`.

## How It Works

Mewjector acts as a DLL proxy for `version.dll`. When the game loads it, Mewjector:

1. Forwards all `version.dll` exports to the real system DLL (the game never notices)
2. Reads `chainloader.ini` for configuration
3. Scans configured directories for `.dll` mod files and loads them
4. Optionally reads a [Mewtator](https://www.nexusmods.com/mewgenics/mods/1) manifest for managed mod loading
5. Exports an opt-in [mod API](API.md) for hook chaining, type ID allocation, and namespace collision detection
6. Logs everything to `mod_logs/chainloader.log`

## Mod API

Mewjector v3.0 exports functions that mods can resolve at runtime. The API is opt-in; mods that don't use it work exactly as before.

| Export | Purpose |
|--------|---------|
| `MJ_InstallHook` | Chainable entry-point hook |
| `MJ_QueryHook` | Hook count at an RVA |
| `MJ_AllocTypeIdPair` | Unique type index pair |
| `MJ_RegisterName` | Collision-detected name registration |
| `MJ_LookupName` | Name ownership query |
| `MJ_GetGameBase` | Game image base |
| `MJ_Log` | Shared log output |
| `MJ_VerifyHooks` | Hook integrity scan |
| `MJ_GetVersion` | API version (3) |

Include [`mewjector.h`](mewjector.h) for convenience wrappers, or resolve directly via `GetProcAddress`. See [API.md](API.md) for full documentation.

## Quick Start

1. Download the [latest release](../../releases/latest) (`Mewjector.zip`)
2. Extract `version.dll` and `chainloader.ini` into your Mewgenics directory (next to the `.exe`)

**Using Mewtator?** You're done! Just enable DLL mods in Mewtator's settings and it handles the rest.

**Manual setup (without Mewtator):**

3. Create a `mods/` folder in the same directory
4. Drop your mod DLLs into `mods/`
5. Launch the game — check `mod_logs/chainloader.log` to confirm everything loaded

## Configuration

Edit `chainloader.ini` to customize behavior:

```ini
[Chainloader]

; Directory to scan for mod DLLs. Relative to the game exe, or absolute.
ScanPath=mods

; Also scan the game exe's own directory for loose DLLs.
ScanGameDir=0

; Path to a Mewtator manifest file listing additional DLLs to load.
; Mewtator sets this automatically when both tools are installed.
; Leave empty if not using Mewtator.
MewtatorManifest=

; Set to 0 to disable mod loading without removing Mewjector.
Enabled=1

; Write a log to mod_logs/chainloader.log.
Logging=1
```

## Building from Source

Requires MSVC (Visual Studio Build Tools or full Visual Studio).

```
git submodule update --init --recursive
cl /c /O2 /GS- /I third_party\Detours\src third_party\Detours\src\disasm.cpp third_party\Detours\src\modules.cpp third_party\Detours\src\detours.cpp
lib /out:Detours.lib disasm.obj modules.obj detours.obj
cl /LD /O2 /GS- /I third_party\Detours\src version.c Detours.lib /Fe:version.dll /link /DEF:version.def
```

This produces `version.dll` along with intermediate build artifacts (`.obj`, `.lib`, `.exp`) which can be discarded.

## Technical Details

**DLL exclusion list:** Mewjector automatically skips known system/engine DLLs (Steam, DirectX, XInput, etc.) to avoid conflicts. See `g_excludeList` in `version.c` for the full list.

**Load order:** DLLs are loaded in filesystem enumeration order within each phase:
1. Configured `ScanPath` directory
2. Game executable directory (if `ScanGameDir=1`)
3. Mewtator manifest entries (if configured)

DLLs already loaded in an earlier phase are skipped in later ones.

**Thread safety:** Logging is mutex-protected. DLL loading happens synchronously during `DLL_PROCESS_ATTACH` before the game's main thread continues.

## Compatibility

- Windows 10/11
- Works alongside Steam

## Mewtator Integration

Mewjector and [Mewtator](https://www.nexusmods.com/mewgenics/mods/1) are separate downloads that recognize each other automatically. When Mewtator detects `chainloader.ini` in the game directory, it writes its manifest path into the config so Mewjector knows which DLLs to load on its behalf, no manual intervention required.

DLL mods installed through Mewtator are loaded alongside any loose DLLs you've placed in the `mods/` folder.

## License

[MIT](LICENSE)

## Attribution

Mewjector uses [Detours](https://github.com/microsoft/Detours) to implement part of its function hook API.
> Copyright (c) Microsoft Corporation. All rights reserved. Licensed under the [MIT](https://github.com/microsoft/Detours/blob/main/LICENSE.md) License.
