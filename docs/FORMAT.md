# GDeflate 1.0

DEFLATE (RFC 1951) with three changes: the data is cut into independent 64 KB tiles, the bits of a tile are dealt
over 32 lanes, and the length and distance alphabets are the DEFLATE64 ones. There is no specification document.
The format is whatever the reference encoder writes and the reference decoders accept; this page is that, written
down. Where the public description and the code disagree, the code wins.

## Stream

| offset | size | content |
|---|---|---|
| 0 | 1 | `id` = 4 |
| 1 | 1 | `magic` = `id ^ 0xff` = 0xfb |
| 2 | 2 | `num_tiles`, u16 |
| 4 | 4 | u32: bits 0-1 `tile_size_idx` = 1, bits 2-19 `last_tile_size`, bits 20-31 zero |
| 8 | 4 * `num_tiles` | tile table, u32 each |
| 8 + 4 * `num_tiles` | | tile data, back to back |

All little-endian.

    out_size   = (num_tiles - 1) * 65536 + (last_tile_size ? last_tile_size : 65536)
    tile i     starts at data + (i ? entry[i] : 0)
    tile i     has entry[i + 1] - entry[i] bytes; the last tile has entry[0] bytes

- Entry 0 is the compressed size of the last tile. Entry `i >= 1` is the offset of tile `i`.
- At most 65535 tiles. A tile is at least 128 bytes. Bytes after the last tile are ignored.
- Every tile decodes to exactly 65536 bytes, the last one to `last_tile_size` (or 65536).
- No checksum, per tile or per stream.
- Tiles share nothing. Codes, output window and bit buffers start fresh in every tile.

## Lanes

- A tile is a sequence of 32-bit packets. Each of 32 lanes has a bit buffer, read from bit 0 up.
- At the start lane `i` takes packet `i`.
- After a lane reads a field, if fewer than 32 bits are left it takes the next unclaimed packet.
- Lanes read in rounds, lane 0 to 31 inside a round. Packets are claimed in that order.
- A field is a group of bits read in one go (a header, a symbol with its extra bits, a stored byte), at most
  31 bits. A lane never runs dry inside a field.
- The encoder reserves a packet as soon as a lane drops below 32 bits, so a tile can end with up to one unread
  packet per lane. Do not require that every packet was consumed.
- Bound-check every packet index against the tile's size.

## Blocks

Header round: lane 0 reads `BFINAL` (1 bit), `BTYPE` (2 bits): 0 stored, 1 static, 2 dynamic, 3 invalid.

**Stored.** Lane 0 reads `LEN`, 16 bits. No `NLEN`, no byte alignment. Then rounds of up to 32 bytes: in round `j`
lane `i` reads 8 bits, byte `32 * j + i` of the block.

**Dynamic.**

1. Lane 0 reads one 14-bit field: `HLIT` (5), `HDIST` (5), `HCLEN` (4). The block has `HLIT + 257` literal/length
   symbols, `HDIST + 1` distance symbols, `HCLEN + 4` precode lengths.
2. One round: lanes 0 to `HCLEN + 3` read 3 bits each, the precode lengths in the order
   16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15.
3. Rounds of precode symbols, one per lane in lane order, until `HLIT + 257 + HDIST + 1` lengths exist. A lane
   reads only while the lengths produced before it are below that total.

| precode symbol | meaning | extra bits |
|---|---|---|
| 0-15 | this length | 0 |
| 16 | repeat the previous length 3-6 times | 2 |
| 17 | 3-10 zeros | 3 |
| 18 | 11-138 zeros | 7 |

Literal/length lengths come first, distance lengths follow, and a run may cross from one to the other.

**Static.** Literal/length code lengths: 8 for 0-143, 9 for 144-255, 7 for 256-279, 8 for 280-287. Distance: 5
for all 32.

**Codes.** Canonical, as in DEFLATE. The first bit read is the most significant bit of the code. At most 15 bits,
7 for the precode. A code must be complete (Kraft sum 1), with two exceptions: the distance code may be empty in a
block without matches, and a code may have a single symbol of length 1, which both bit values decode to.

## Symbol rounds

    every lane starts in LIT
    each round, lanes 0 to 31 in order:
        lane in DIST:  read a distance symbol and its extra bits, D; the lane's match is complete; go to LIT
        lane in LIT:   read a literal/length symbol s
                       s < 256   one output byte
                       s == 256  end of block: higher lanes in LIT read nothing this round
                       s > 256   read its extra bits, L; reserve L output bytes here; go to DIST
    after the end-of-block round one more round runs: lanes in DIST read their distance, nobody else reads

- Output positions are a prefix sum over the lanes of a round, in lane order.
- A match takes its `L` bytes where its length was read. Its distance arrives one round later.
- Copy: `out[p + k] = out[p + k - D]` for `k = 0 .. L-1`, which repeats the last `D` bytes when `D < L`.
- `D` must not exceed `p`, the bytes this tile has produced. `p + L` must not exceed the tile's size.
- The tile ends after the block with `BFINAL = 1`. Leftover bits and packets are ignored.

## Alphabets

Literal/length: 0-255 literals, 256 end of block, 257-285 lengths. 286 and 287 are invalid.

| symbols | extra bits | lengths |
|---|---|---|
| 257-264 | 0 | 3-10 |
| 265-268 | 1 | 11-18 |
| 269-272 | 2 | 19-34 |
| 273-276 | 3 | 35-66 |
| 277-280 | 4 | 67-130 |
| 281-284 | 5 | 131-258 |
| 285 | 16 | 3-65538 |

Distance, 32 symbols:

| symbols | extra bits | distances |   | symbols | extra bits | distances |
|---|---|---|---|---|---|---|
| 0-3 | 0 | 1-4 |   | 16-17 | 7 | 257-512 |
| 4-5 | 1 | 5-8 |   | 18-19 | 8 | 513-1024 |
| 6-7 | 2 | 9-16 |   | 20-21 | 9 | 1025-2048 |
| 8-9 | 3 | 17-32 |   | 22-23 | 10 | 2049-4096 |
| 10-11 | 4 | 33-64 |   | 24-25 | 11 | 4097-8192 |
| 12-13 | 5 | 65-128 |   | 26-27 | 12 | 8193-16384 |
| 14-15 | 6 | 129-256 |   | 28-29 | 13 | 16385-32768 |
|   |   |   |   | 30-31 | 14 | 32769-65536 |

For a distance symbol `s >= 4`: `extra = s / 2 - 1`, `base = 1 + ((2 + (s & 1)) << extra)`. Extra bits follow the
symbol's code in the same field and are added to the base.

## What a decoder checks

1. Header: at least 8 bytes, `id`, `magic`, `tile_size_idx`, reserved bits, `last_tile_size <= 65536`.
2. Tile table: fits in the input, offsets strictly increasing, every tile at least 128 bytes, last tile inside
   the input.
3. Every packet index is inside the tile.
4. `BTYPE != 3`.
5. Stored: `LEN` fits in the output left.
6. Dynamic: the precode is complete; no symbol 16 before any length; no run past the total; symbols 286 and 287
   have length 0; the literal/length code is complete and has symbol 256; the distance code is complete, single
   or empty.
7. Symbols: none above 285; no distance when the distance code is empty; `D <= p`; the output never passes the
   tile's size.
8. After the last block the tile has produced exactly its size.

## Where the references disagree

| topic | public description or reference decoders | vkdeflate |
|---|---|---|
| stored block header | described as 4 bytes, byte-aligned | 16 bits at lane 0's position, as the encoder writes it |
| code-length run past the total | ignored | error |
| symbols 286, 287 | CPU decoder treats them as 285 | error |
| packet index past the tile | not checked | error |
| a bad tile on the GPU | garbage output | status word per tile |

## Vulkan regions

`VK_EXT_memory_decompression` takes regions, not streams. For GDeflate a region is one tile: its compressed bytes
without the stream header, `decompressedSize <= 65536`, both addresses 4-byte aligned, ranges not overlapping.
vkdeflate's `tile_desc_t` holds the same four facts. This has not been tested against a driver that has the
extension.

Sources and their licenses are listed in the README.
