# QLMDV

QLMDV is a command-line utility for creating, validating, inspecting, and safely
changing raw QLAY-format Sinclair QL Microdrive images. It is written in GNU11 C,
uses only the host C runtime, and builds as a single unity translation unit.

## Build

Build the bootstrap tool and the debug executable:

```sh
cc -std=gnu11 -Wall -Wextra -o build build.c
./build
./build test
```

The result is written directly to `./qlmdv`. The build helper honors `CC` as a
single compiler executable name or path.

```sh
./build debug
./build release
```

Debug builds use `-O0 -g`; release builds use `-Os -s` and link with the host C
runtime. `./build test` compiles and runs the hosted test suite. All builds use
GNU11 with warnings enabled and compile their unity translation unit directly,
without intermediate objects or project libraries.

The test runner includes the same implementation sources so it can exercise
private validation and fault-injection paths.

## Project structure

The production unity include order is:

```text
runtime -> qlay -> image -> cli
```

- `src/runtime.c` owns runtime support, the monotonic arena, time-zone
  conversion, and shared runtime helpers.
- `src/qlay.c` defines the raw 686-byte QLAY sector layout and checksum contract.
- `src/image.c` validates, decodes, rebuilds, and transactionally writes images.
- `src/cli.c` implements commands, host-file import/export, and metadata manifests.
- `build.c` provides the shared debug/release/test build helper.
- `qlmdv.c` combines the layers and defines `main`.
- `test.c` contains generated-image, CLI workflow, corruption, and transaction tests.
- `COPYRIGHT` contains the MIT license and copyright notice.

## Command line

```text
qlmdv COMMAND [OPTIONS] IMAGE [OPERANDS...]
```

Commands have readable names and tar-like aliases:

| Command | Alias | Purpose |
| --- | --- | --- |
| `create` | `c` | Create an image and optionally import host files |
| `list` | `t` | List QDOS files with type and length; `--verbose` adds header fields |
| `extract` | `x` | Extract all or selected QDOS files |
| `add` | `a` | Add files whose QDOS names do not already exist |
| `remove` | `d` | Remove selected QDOS files |
| `replace` | `u` | Replace files whose QDOS names already exist |
| `reorder` | `m` | Put named files first and rebuild their sector placement |
| `inspect` | `i` | Validate the medium and show detailed file headers |

For example:

```sh
./qlmdv create games.mdv BOOT loader_bin=loader
./qlmdv create --verbose games.mdv BOOT loader_bin=loader
./qlmdv list --verbose games.mdv
./qlmdv add games.mdv title_scr=title_scr
./qlmdv reorder games.mdv BOOT loader
./qlmdv extract --directory unpacked games.mdv BOOT loader
./qlmdv inspect --verbose games.mdv
```

An import operand is either a host path or `HOST=QDOS_NAME`. Without a mapping,
the host basename becomes the QDOS name. QDOS matching is case-insensitive and
exact, names are limited to the 36-byte directory field, and colliding imports
are rejected. Ordinary imports use QDOS type 0 and convert the host modification
time to QDOS time.

`list` prints each file's QDOS type, data length, and name. With `--verbose`, it
also prints data space, update date, version, and backup date. `inspect` includes
each file's directory ID, a description of its type, decoded header fields, and
the complete 64-byte header as field-by-field hexadecimal byte sequences.

Every command accepts `--verbose`. Mutation and extraction commands then print
an operation summary, while all commands report arena used and committed memory
on standard error. Without `--verbose`, successful mutations and extractions
remain silent. The essential output of `list` and `inspect` is unchanged.

The raw one-byte QDOS file type can have any value from 0 through 255:

| Type | Meaning |
| ---: | --- |
| `0` | Ordinary data, including text, SuperBASIC programs, and raw binary |
| `1` | Executable program or job; data space gives its extra runtime requirement |
| `2` | Relocatable object file in Sinclair SROFF format |
| `255` | Directory marker used by directory-capable QDOS-compatible drivers |
| `3`–`254` | No standard original-QDOS meaning; preserved without interpretation |

`create` defaults to 255 sectors and derives the ten-byte medium name from the
image basename after removing a final `.mdv`. New writable images may contain
200 to 255 sectors. Use `--sectors N`, `--medium-name NAME`, or `--random-id N`
to override those values. Existing output is rejected unless `--force` is used.

Extraction writes into the current directory, or the directory selected by
`--directory DIR`. Unsafe host bytes in QDOS names are converted to `%HH` leaf
names and can never escape the selected directory. Existing files are rejected.
`--force` permits overwriting regular files but still refuses symbolic links.

## QDOS metadata manifests

Use `extract --metadata FILE` to preserve complete 64-byte QDOS headers for a
later `create`, `add`, or `replace`:

```sh
mkdir unpacked
./qlmdv extract --directory unpacked --metadata headers.txt games.mdv
./qlmdv create --metadata headers.txt restored.mdv unpacked/*
```

The dependency-free manifest is plain text. Its first line is exactly
`QLMDV-METADATA 1`. Each following tab-separated record contains a
percent-escaped extracted filename and the original header as 128 hexadecimal
characters. Type-specific fields, dates, version, backup date, and other header
bytes are retained. Length, QDOS name, file ID, map entries, and allocation
fields are regenerated for the new image.

## Validation and mutation safety

`inspect` checks physical sector headers, medium identity, map entries,
directory and file consistency, block continuity, duplicate allocation,
capacity, and all three checksums. It reports every problem and exits with
status 1 for corrupt images; `--verbose` adds a line for every physical sector.
Images may store logical sectors in ascending, descending, or arbitrary
physical order.

Every mutation validates first, rebuilds in memory, writes and syncs a temporary
sibling, and only then renames it over the image while preserving permissions.
Structurally ambiguous images are never changed. `--force` permits an otherwise
unambiguous image with damaged checksums to be rebuilt; it does not override
missing-name, duplicate-name, or capacity errors. Medium identity, valid sector
headers, physical record order, and bad-sector status are preserved.

Only raw QLAY streams are supported. MDI and Qemulator Mdump files are rejected.
Usage errors exit with status 2; validation and I/O failures exit with status 1.
Use `./qlmdv --help`, `./qlmdv COMMAND --help`, and `./qlmdv --version` for the
concise command reference.
