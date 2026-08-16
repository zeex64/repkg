# repkg

`repkg.exe` is the standalone Sunrise package compiler for the version-38 Windows Tiger format.
It replaces the Python `sunrise-pkg` frontend and the separate `sunrise_pkg_extract.exe` helper.

`depkg.exe` is the matching unpacker. It turns a package into a complete editable project that can
be passed straight back to `repkg.exe`.

Copy the matching `oo2core_3_win64.dll` beside `repkg.exe`. That is the only adjacent runtime file:
the verified package keys and Sunrise header profile are compiled into the executable.

This repository is the standalone extraction of the package tooling originally developed in
[Sunrise](https://github.com/TotalTaxAmount/Sunrise). It contains no game packages, extracted game
assets, game executable, or proprietary Oodle binary. Copy the matching Oodle DLL from your own
installation for local use; do not submit it to this repository.

> [!IMPORTANT]
> This project targets the offline, open-source Sunrise environment. Back up packages before
> replacing them, and do not use the tool against services or content you are not authorized to
> modify.

## Complete custom package

```json
{
  "patch": false,
  "name": "w64_sunrise_0aa0_0",
  "entries": [
    {
      "reference": "0x80809C36",
      "type_info": "0x0000100A",
      "path": "custom-entry.bin"
    }
  ],
  "named_tags": [
    {
      "entry_index": 0,
      "class_id": "0x80809C36",
      "name": "sunrise:example"
    }
  ]
}
```

```text
repkg.exe package.json
```

`name` supplies the package id and generation. The command writes `w64_sunrise_0aa0_0.pkg` beside
`package.json`. The package is built from the manifest payloads; it does not read or clone a stock
package. An optional `profile` package may override the built-in header constants for another
compatible client build, but it is not needed for Sunrise's supported build. An optional output
argument may write the package elsewhere, but its filename must still match `name`.

`block_key` may be `primary` (the default) or `alternate`. `depkg` records the verified mode from
the source package. The early unpatchable UI packages use the alternate key and must retain that
mode when rebuilt.

A few stock package names do not contain their header package ID. Projects produced by `depkg`
include an explicit `package_id` for those names; `repkg` accepts it and verifies it against `name`
whenever the name already contains an ID.

## Stock-family patch

```json
{
  "patch": true,
  "game_dir": "C:/Games/Sunrise",
  "name": "w64_ui_01a3_7",
  "replacements": [
    {
      "type": "localized_text",
      "container_tag": "0x80B46A5A",
      "string_hash": "0xBD97E2B5",
      "text": "LONG TEXT WORKS - WOOOOHOOOOO"
    }
  ]
}
```

```text
repkg.exe patch.json
```

The target `name` determines both filenames. For `w64_ui_01a3_7`, repkg requires
`<game_dir>/packages/w64_ui_01a3_6.pkg` and writes
`<game_dir>/packages/w64_ui_01a3_7.pkg`. If the predecessor does not exist, the command stops with
its exact expected path. An optional output argument may stage the result elsewhere, but its
filename must still be `w64_ui_01a3_7.pkg`.

A stock override reads the predecessor because every patch republishes the complete current
entry/block directory while storing only its new physical blocks.

Both build commands authenticate and decode their output before succeeding. Use `--no-verify` only
for diagnostics. Existing output files are refused unless `--force` is supplied.

## Unpack and rebuild

```text
depkg.exe C:\Games\Sunrise\packages\w64_ui_01a3_7.pkg
repkg.exe w64_ui_01a3_7\package.json
```

By default, `depkg` creates a directory named after the package in the current directory. A second
argument selects another output directory, and `--force` replaces an existing output directory:

```text
depkg.exe PACKAGE OUTPUT_DIRECTORY --force
```

The generated project contains:

```text
w64_ui_01a3_7/
  package.json
  assets/
    0000.bin
    0001.wem
    texture_80A14580.dds
    ...
```

`package.json` preserves the package ID and generation, every entry's class/reference and type
metadata, named tags, wide hashes, tag-pair metadata, and the source header profile. Tag pairs are
emitted as raw `first_tag`/`second_tag` TagHash values because either side may refer to another
package. Recognized resources are written in their verified native or editable formats:

- Wwise audio streams remain native compressed `.wem` files. Convert them externally when editing,
  then convert the result back to WEM before running `repkg`.
- Verified paired Tiger texture header/data entries become editable `.dds` files. `repkg` rebuilds
  both entries from the DDS dimensions, DXGI format, and pixel payload.
- Wwise banks remain `.bnk`, DirectX shader containers remain `.dxbc`, and already-standard file
  formats retain their normal extension.
- Unknown resource classes remain `.bin` until a verified bidirectional codec is implemented.

When the input is a patch generation, `depkg` resolves inherited blocks from `_0` through the
selected generation using sibling package files. It then emits a self-contained full-package
manifest (`"patch": false`) with every current entry materialized. Rebuilding therefore does not
need the original package chain and does not copy any stock package bytes.

The unpacker currently emits rebuild projects for the verified `d2_prebl` version-38 package
variant used by Sunrise. It rejects the older beta metadata layout instead of silently rebuilding
it as a different format. Like `repkg`, `depkg` needs only the matching `oo2core_3_win64.dll`
beside the executable; it does not need a game executable, helper program, or key-cache file.

## Other commands

```text
repkg.exe inspect PACKAGE [--verify-blocks] [--list-entries] [--list-lookups]
repkg.exe extract PACKAGES TAG OUTPUT
repkg.exe replace-localized-string CONTAINER ENGLISH HASH TEXT OUTPUT
repkg.exe verify-directory PACKAGES [--stock-only] [--verify-blocks]
```

## Building

On Windows with Visual Studio 2022 and its C++ workload:

```text
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executables are written to `build\Release`.

On Linux, install CMake, Ninja, `clang-cl`, the LLVM tools, and
[xwin](https://github.com/Jake-Shadle/xwin). Prepare a local Windows SDK/CRT and build with the
included cross-toolchain:

```text
xwin --accept-license splat --output .xwin-cache
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchain-windows.cmake"
cmake --build build
```

Then copy your matching `oo2core_3_win64.dll` beside `repkg.exe` and `depkg.exe` before running
either program.

## License

`repkg` is distributed under the GNU General Public License v3.0. The Oodle runtime is not part of
this project and is not distributed under that license.
