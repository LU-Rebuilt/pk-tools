# pk-tools

LEGO Universe PK archive tools. CLI extractor and Qt6 GUI viewer.

> **Note:** This project was developed with significant AI assistance (Claude by Anthropic). All code has been reviewed and validated by the project maintainer, but AI-generated code may contain subtle issues. Contributions and reviews are welcome.

Part of the [LU-Rebuilt](https://github.com/LU-Rebuilt) project.

## Tools

### pk_extractor

Extract all files from a PK archive to disk.

```
pk_extractor <file.pk> [output_dir]
```

Extracts each entry as `entry_N.bin`. Automatically decompresses SD0-compressed entries. Prints entry metadata (CRC, sizes, compression flag) to stdout.

### pk_viewer

Qt6 GUI for browsing and extracting PK archives. Requires Qt6.

```
pk_viewer [file.pk]
```

**Features:**
- Open a single .pk file or set a client root to auto-load all .pk archives with multithreaded parallel loading
- CRC-to-filename resolution using `versions/trunk.txt` manifest and `primary.pki` pack index
- File type detection from resolved filename extensions and magic bytes
- Search/filter box for finding files by name or type across all loaded packs
- Extract individual files, multiple selected files, or all files preserving directory structure
- Double-click to open NIF, HKX, LXFML, LXF, and FDB files directly in their respective viewers
- Multi-pack tree view with expandable pack nodes

**Keyboard shortcuts:**
- `Ctrl+O` — Open single archive
- `Ctrl+R` — Set client root (loads all packs + manifest)
- `Ctrl+E` — Extract selected entries
- `Ctrl+Shift+E` — Extract all entries

**Setup:** Set the client root to the game's install directory (the folder containing `versions/` and `client/`). The viewer loads `versions/trunk.txt` for filename resolution and `versions/primary.pki` for CRC-to-pack mapping.

## Building

```bash
cmake -B build
cmake --build build -j$(nproc)
```

pk_viewer is skipped automatically if Qt6 is not found.

For local development:

```bash
cmake -B build -DFETCHCONTENT_SOURCE_DIR_LU_ASSETS=/path/to/local/lu-assets \
               -DFETCHCONTENT_SOURCE_DIR_TOOL_COMMON=/path/to/local/tool-common
```

## Acknowledgments

Format parsers built from:
- **[lcdr/lu_formats](https://github.com/lcdr/lu_formats)** — Kaitai Struct PK/SD0 format definitions
- **[lcdr/utils](https://github.com/lcdr/utils)** — Python PK/SD0 reference implementations
- **[DarkflameServer](https://github.com/DarkflameServer/DarkflameServer)** — PackRecord structure reference
- **Ghidra reverse engineering** of the original LEGO Universe client binary

## License

[GNU Affero General Public License v3.0](https://www.gnu.org/licenses/agpl-3.0.html) (AGPLv3)

