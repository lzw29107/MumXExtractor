# MumXExtractor

Reads Windows servicing containers (`.mumx`): header and section table, blob listing,
blob extraction, and index lookups.

## Engines

| Engine | Behaviour |
|---|---|
| `dll` | Calls `TurboContainer.dll` from the servicing stack. |
| `native` | Built-in container reader; needs no external DLL. |
| `auto` (default) | DLL when it loads and opens the container, otherwise native. |

## Usage

```
MumXExtractor info     <container>                        show header and section table
MumXExtractor dump     <container> <index|all> <outPath>  decompress a section payload
MumXExtractor list     <container>                        list blobs
MumXExtractor index    <container> [key]                  list indexes and look up a key
MumXExtractor extract  <container> <name|*> <outDir>      extract one or all blobs
MumXExtractor mum      <container> <outDir>               extract update.mum only
MumXExtractor selftest [container]                        run engine self-tests

  --engine auto|dll|native   auto (default) prefers the DLL and falls back to native
  --dll <path>               explicit TurboContainer.dll path
  --exact                    list: report rehydrated sizes (slow, byte exact)
  -v, --verbose              timings, header fields, section table
```

Command names and `--engine` values are case-insensitive. Every run starts with the
product banner, followed by the operation and the container path:

```text
MumXExtractor v1.0.0
operation : info
container : collection.mumx
```

`mum` mirrors the servicing behaviour: it extracts the single blob named `update.mum`.

## Format notes

### Byte order

Every fixed-width integer is big-endian; variable-width integers use LEB128.

### File header

| Offset | Type | Meaning |
|-------:|------|---------|
| 0x00 | char[4] | `"MUMX"` |
| 0x04 | u32 | version, 1 or 2 |
| 0x08 | u32 | section count |
| 0x0C | u32 | (v2) hash algorithm, `0x800C` = `CALG_SHA_256` |
| 0x10 | u32 | (v2) hash length n |
| 0x14 | u8[n] | (v2) content hash |
| … | u32 | (v2) default compression algorithm |
| … | u32 | (v2) default block size |

A v1 header is 12 bytes, a v2 header is 28 + n bytes.

### Sections

Section headers are packed back to back and each payload follows its own header:

```
payloadOffset(i) = headerOffset(i) + headerSize(i)
headerOffset(i+1) = payloadOffset(i) + payloadSize(i)
```

A header is 28 bytes (v1) or 52 bytes (v2) and holds the version, section type,
`payloadSize`, `poolOffset` (bookkeeping, not a file offset) and the v2 compression
parameters (`compressionAlg`, `blockSize`, `uncompressedSize`, `blockIndexSize`).
`payloadSize` is the only field used to walk the chain.

| Type | Content |
|-----:|---------|
| 0 | Primary data pool: string pool plus raw blob buffers |
| 1, 2 | Entry table: id → offset map and the entry tree |
| 3, 4 | Name table |
| 5 | Index table |

Types 1–4 start with a second, nested pool header.

### Payload content

- **String pool**: `[varint length][UTF-8]` records. Every string reference in the
  container is a byte offset into this pool, never an index.
- **Entry table**: two nested pools — the entry tree, and an `id → offset` table stored
  as consecutive LEB128 pairs. An entry carries a shared-subtree reference, a flags
  word, an optional hash/size block, a tag id, attributes and children.
- **Name table**: `[varint length][UTF-8][0x00]` records; the entry id is the byte
  offset of the name record.
- **Index table**: an index header (name, key comparer, value type), a compressed
  string pool, then an uncompressed table of `[key id][value id]` LEB128 pairs. The
  entry count sits in the section header's `poolOffset`.
- **Compression**: the block index is stored at the end of each compressed stream as
  `(uncompressedOffset, compressedOffset)` pairs. A block whose compressed size reaches
  its uncompressed size is stored verbatim and copied instead of decompressed.

### Hydration flags

| Flag | Effect |
|---|---|
| `0x20` | Emit a UTF-8 BOM before the XML declaration |
| `0x10` | Close empty elements as `></tag>` instead of ` />` |
| `0x08` | Append a trailing CRLF |

Text escapes `&`, `<`, `>`; attribute values also escape `"` and encode `\r`, `\n`,
`\t` as `&#xD;`, `&#xA;`, `&#x9;`. Elements containing text keep their subtree inline.
