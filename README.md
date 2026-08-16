# repkg

`repkg` is a standalone C++ toolchain for unpacking, inspecting, editing, and rebuilding the
version-38 Windows Tiger packages used by Sunrise.

The project provides two command-line programs:

- **`depkg.exe`** converts a `.pkg` file into a self-contained editable project.
- **`repkg.exe`** compiles an editable project or authored manifest back into a `.pkg` file.

It supports complete custom packages, stock-family patch generations, editable DDS textures,
native Wwise assets, package metadata preservation, and authenticated output verification.

> [!IMPORTANT]
> `repkg` is intended for the offline, open-source Sunrise environment. Keep a known-good backup of
> every package you replace. A structurally valid package can still contain an asset that the game
> does not know how to use.

## Features

- Unpacks full packages and patch generations into `package.json` plus an `assets` directory.
- Flattens inherited patch blocks so extracted projects rebuild without the original package chain.
- Builds complete packages directly from manifest data instead of copying stock package bytes.
- Builds the next patch generation from a stock-family predecessor.
- Replaces complete entries or localized strings in patch packages.
- Converts verified Tiger texture pairs to editable DDS files and rebuilds both package entries.
- Preserves entry metadata, named tags, wide hashes, tag pairs, package identity, header profile,
  and primary/alternate block-key mode.
- Encrypts, compresses, authenticates, and verifies generated package blocks.
- Runs as native Windows x64 executables and is also compatible with Wine.

## Requirements

- 64-bit Windows, or Wine on Linux.
- `repkg.exe` and `depkg.exe` from the same build.
- The matching `oo2core_3_win64.dll` from your own installation, placed beside both executables.
- An offline Sunrise client with custom-package trust support when testing modified packages.

The Oodle DLL is proprietary and is not distributed by this repository. Do not commit or
redistribute it here.

Your working directory should look like this:

```text
repkg-toolkit/
  repkg.exe
  depkg.exe
  oo2core_3_win64.dll
```

## Quick start: unpack, edit, and rebuild

### 1. Unpack a package

Close the game, open a terminal beside the tools, and run:

```text
depkg.exe "C:\Games\Sunrise\packages\w64_ui_bootflow_unp1_0.pkg"
```

`depkg` creates a project named after the input package in the current directory:

```text
w64_ui_bootflow_unp1_0/
  package.json
  assets/
    0000.bin
    0001.wem
    texture_80A14580.dds
    ...
```

Choose a different project directory by passing a second path:

```text
depkg.exe "C:\path\to\package.pkg" "C:\mods\my-package-project"
```

If the destination already exists, `depkg` refuses to overwrite it. Add `--force` only when you
intend to delete and recreate that extracted project:

```text
depkg.exe "C:\path\to\package.pkg" "C:\mods\my-package-project" --force
```

### 2. Edit assets

Edit files inside the project's `assets` directory without renaming them. Start with one small
change at a time.

| Extension | Resource | Editing notes |
| --- | --- | --- |
| `.dds` | Texture | Use a DDS-aware editor. Keeping the original dimensions, DXGI format, and mip count is safest. |
| `.wem` | Wwise audio | Convert externally for editing, then convert back to WEM before rebuilding. |
| `.bnk` | Wwise bank | Use Wwise-aware tools; arbitrary binary edits are unsafe. |
| `.otf` | OpenType font | Replace only with a valid font compatible with the consuming UI. |
| `.dxbc` | DirectX shader | Requires shader-specific tools and compatible compiled bytecode. |
| `.bin` | Unidentified/raw resource | Preserved losslessly, but no editable codec has been verified yet. |

Do not casually change package IDs, entry order, tag hashes, class references, `block_key`, or
header-profile values in `package.json`. Those fields describe relationships expected by the game.

### 3. Rebuild the project

Pass the generated manifest to `repkg`:

```text
repkg.exe "w64_ui_bootflow_unp1_0\package.json"
```

The rebuilt package is written beside `package.json` using the manifest's exact `name`:

```text
w64_ui_bootflow_unp1_0/
  package.json
  w64_ui_bootflow_unp1_0.pkg
  assets/
```

If that output already exists, use `--force` to replace it:

```text
repkg.exe "w64_ui_bootflow_unp1_0\package.json" --force
```

`repkg` authenticates and decodes its output before reporting success. Do not use `--no-verify`
for normal builds; it exists only for format diagnostics.

### 4. Install and test

1. Close the game.
2. Back up the original package outside the active `packages` directory.
3. Copy the rebuilt package into the game directory using the exact expected filename.
4. Start Sunrise and test the specific screen, activity, sound, or resource you changed.
5. Restore the original immediately if the game reports a content error, freezes, or renders the
   affected content incorrectly.

## Patch-generation extraction

A patch package may inherit blocks from earlier files in the same family. When unpacking a package
such as `w64_ui_01a3_6.pkg`, keep every required predecessor (`_0` through `_5`) beside it.

`depkg` resolves those inherited blocks and writes a self-contained project with `"patch": false`.
Rebuilding that extracted project therefore does not require the original package files and does
not copy bytes from them.

It is normal for an extracted project to contain fewer asset files than package entries. One
editable DDS file represents a paired Tiger texture-header entry and texture-data entry; `repkg`
reconstructs both during the build.

## Authoring a complete custom package

A manifest with `"patch": false` creates a complete package from the listed assets and metadata.
It does not need `game_dir` or a stock base package.

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

Build it with:

```text
repkg.exe package.json
```

The output filename is derived from `name` and must match it. An optional second command-line
argument can select another output path, but the filename itself must remain unchanged:

```text
repkg.exe package.json "C:\mods\w64_sunrise_0aa0_0.pkg"
```

Names normally encode the package ID and generation as
`w64_<family>_<4-hex-package-id>_<generation>`. Package IDs must be in `0x0100` through `0x19FF`,
and generations must be in `0` through `255`. Choose an unused identity for a genuinely new custom
package.

`block_key` may be `primary` (the default) or `alternate`. Projects created by `depkg` record the
verified mode automatically. Early unpatchable UI packages use the alternate key and must retain
it when rebuilt.

See [`examples/package.json`](examples/package.json) for a complete minimal project.

## Authoring a stock-family patch

A manifest with `"patch": true` creates the next generation of an existing package family. It
needs the immediately preceding package because the new generation republishes the complete
current entry/block directory while storing only newly replaced physical blocks.

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
      "text": "CUSTOM TEXT"
    }
  ]
}
```

Build it with:

```text
repkg.exe patch.json
```

For this example, `repkg` requires:

```text
C:\Games\Sunrise\packages\w64_ui_01a3_6.pkg
```

and writes `w64_ui_01a3_7.pkg`. If the predecessor does not exist or its internal identity does
not match the requested family, the build stops with an error.

Pass an explicit output path if you want to stage the package outside the game directory first.
The output filename must still match `name`:

```text
repkg.exe patch.json "C:\mods\w64_ui_01a3_7.pkg"
```

Patch manifests support two replacement types:

- `localized_text` replaces one string selected by its localized container TagHash and string hash.
- `entry` replaces an entire package entry selected by `tag` or `entry_index` with bytes from
  `path`.

See [`examples/patch.json`](examples/patch.json) for a localized-text example.

## Command reference

### depkg

```text
depkg.exe PACKAGE [OUTPUT_DIRECTORY] [--force]
```

### repkg build

The manifest's `patch` field chooses complete-package or patch-generation behavior:

```text
repkg.exe MANIFEST [OUTPUT] [--force] [--no-verify]
```

### Inspection and verification

```text
repkg.exe inspect PACKAGE [--verify-blocks] [--list-entries] [--list-lookups]
repkg.exe verify-directory PACKAGES [--stock-only] [--verify-blocks]
```

`--verify-blocks` reads, authenticates, decrypts, and decompresses every physical package block.

### Focused extraction and localized-text utilities

```text
repkg.exe extract PACKAGES TAG OUTPUT
repkg.exe replace-localized-string CONTAINER ENGLISH HASH TEXT OUTPUT
```

Run either executable with `--help` for its built-in usage summary.

## Building from source

### Windows

Install Visual Studio 2022 with the Desktop development with C++ workload, then run:

```text
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executables are written to `build\Release`.

### Linux cross-build

Install CMake, Ninja, `clang-cl`, the LLVM tools, and
[`xwin`](https://github.com/Jake-Shadle/xwin). Prepare the Windows SDK/CRT and build:

```text
xwin --accept-license splat --output .xwin-cache
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchain-windows.cmake"
cmake --build build
```

Copy your matching `oo2core_3_win64.dll` beside the resulting executables before running them.

## Troubleshooting

### `oo2core_3_win64.dll must be beside the executable`

Copy the matching DLL from your own installation into the same directory as `repkg.exe` and
`depkg.exe`.

### `output already exists`

The tools avoid accidental overwrites. Add `--force` only after confirming the existing output can
be replaced.

### An earlier patch generation cannot be found

Place the required sibling generations together in the source package directory. Patch extraction
must be able to follow inherited block ownership back through the family.

### The rebuilt package has a different size or checksum

That is expected. Recompression and block layout can produce different bytes while preserving the
same decoded resources. Use `inspect --verify-blocks` to check container integrity.

### The package verifies but the game freezes or displays broken content

Container verification proves the package tables, encryption, compression, and hashes are
internally readable. It cannot prove that an edited texture, sound, shader, font, or binary object
matches every semantic requirement of the game. Restore the backup and retry with a smaller edit
that preserves the original asset format.

### The game reports a content-integrity error

Confirm that you are using the offline Sunrise client with custom-package trust support and that
the package name, internal ID, generation, header profile, and block-key mode were preserved.

## License

`repkg` is distributed under the [GNU General Public License v3.0](LICENSE). The Oodle runtime is
not part of this project and is not distributed under that license.
