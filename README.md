# skc — SKK/SKC Geometry Dash replay format

`skc` is a small C++ library and CLI for the **SKK** / **SKC** replay-macro
formats used by the **skkBot** Geode mod for Geometry Dash.

* **`.skk`** — uncompressed native macro (magic `SKKB`, version 5).
* **`.skc`** — compressed, lossless macro (magic `SKC3`, version 5, Zstd).

Both formats store per-frame player **physics** (`PhysicsFrame`), discrete
**inputs** (`Input`), level metadata, and sparse **visual/animation state
anchors** (`visualAnchors`) used to faithfully reproduce robot/spider/dash/glow
visuals during playback.

---

## Repository layout

```
include/skc/        public API (header-only library)
  codec.hpp         skc_compress_v4 / skc_decompress, stream helpers, field bits
  format.hpp        Macro / PhysicsFrame / Input structs, loadSkk / saveSkk
  varint.hpp        varint / zigzag / float-delta helpers
  zstd_wrapper.hpp  dynamic zstd loader (loads libzstd.dll at runtime)
src/
  codec.cpp         codec implementation
  zstd_wrapper.cpp  dynamic zstd loader
  skcconv.cpp       CLI tool
test/main.cpp       round-trip + real-file tests
lib/zstd/.../dll/libzstd.dll   runtime dependency (copied next to the exe)
```

This project mirrors the layout of sibling format libraries
(`libslc`, `libGDR`): a header-only `INTERFACE` library (`libskc`) plus a
converter executable (`skcconv`).

---

## Build

Requires **CMake ≥ 3.20** and a C++17 compiler (tested with MSVC 19 / VS 2022).

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The build copies `libzstd.dll` next to the produced binaries. No zstd headers
or import libraries are required — `zstd_wrapper.cpp` loads the DLL dynamically
via `LoadLibraryA`.

Outputs:

* `build/Release/skcconv.exe` — the converter CLI
* `build/Release/skc_test.exe` — runs the test suite (`ctest` also works)

---

## CLI usage (`skcconv`)

| command | description |
| --- | --- |
| `skcconv compress <in.skk> <out.skc>` | encode a native `.skk` into a compressed `.skc` |
| `skcconv decompress <in.skc> <out.skk>` | decode a `.skc` back into a native `.skk` |
| `skcconv info <file.skc\|file.skk>` | print format + macro statistics |
| `skcconv verify <in.skc>` | decode, re-encode, decode again and check lossless |

Example:

```powershell
skcconv decompress "Rolling Ball.skc" "Rolling Ball.skk"
skcconv verify    "Rolling Ball.skc"
```

---

## Library usage

The API lives in the `skc` namespace and is header-only:

```cpp
#include "skc/format.hpp"
#include "skc/codec.hpp"
#include <vector>

skc::Macro m;
if (skc::loadSkk("macro.skk", m)) {
    skc::SKCCompressResult r = skc::skc_compress_v4(m);   // -> .skc bytes
    skc::Macro back;
    skc::skc_decompress(r.data, back);                    // -> Macro
}
```

Key symbols:

* `skc::Macro` — container: `tps`, `inputs`, `physics`, `visualAnchors`, metadata.
* `skc::loadSkk(path, macro)` / `skc::saveSkk(path, macro)` — native `.skk` I/O.
* `skc::skc_compress_v4(macro)` — returns `SKCCompressResult { data, compression_ratio }`.
* `skc::skc_decompress(bytes, macro)` — decodes both v4 and v5 `.skc`.

Link your target against `libskc` and add `src/codec.cpp` + `src/zstd_wrapper.cpp`
to the sources (or compile the `.cpp` files directly — `libskc` is `INTERFACE`).

---

## Format specification

All multi-byte integers are **little-endian**. Strings are encoded as a
varint byte-length followed by UTF-8 bytes.

### `.skk` (magic `SKKB` = `0x424B4B53`, version `5`)

| field | type |
| --- | --- |
| magic | `uint32` `0x424B4B53` |
| version | `uint32` = 5 |
| tps | `float32` |
| author | string |
| description | string |
| level_name | string |
| level_id | `int32` |
| seed | `uint64` |
| input_count | `uint64` |
| inputs | `Input[input_count]` (raw struct) |
| physics_count | `uint64` |
| physics | `PhysicsFrame[physics_count]` (raw struct) |
| loop_count | `uint64` (= 0 in v5; loop compression removed) |
| anchor_count | `uint64` |
| anchors | `anchor_count` × (`uint64` length + `length` opaque bytes) |

`Input` and `PhysicsFrame` are serialized as **raw little-endian structs with
natural 4-byte alignment** (as emitted by the reference MSVC/GCC x86-64 build).
See `include/skc/format.hpp` for the exact field layout.

Each visual anchor is an opaque blob produced by serializing the full
`PlayerState` of the skkBot mod at a sparse point in time (every 30 frames and
on vehicle changes). It is decoder-agnostic: a reader that does not understand
the blob can ignore it.

### `.skc` (magic `SKC3`, version `5`)

Container (uncompressed):

| field | type |
| --- | --- |
| magic | 4 bytes `"SKC3"` |
| version | `uint32` = 5 |
| flags | `uint32` (bit 0 = Zstd, bit 1 = XOR-scrambled) |
| level_id | varint |
| seed | varint |
| total_frames | varint (physics frame count) |
| base_frame | varint (first physics frame index) |
| chunk_size | `uint32` (480) |
| chunk_count | `uint32` |
| body_size | varint (compressed body length) |
| body | `body_size` bytes of Zstd-compressed payload |

Decompressed body:

1. **strings**: `author`, `description`, `level_name` (varint len + bytes)
2. **inputs**: varint `input_count`, then per input a varint `frame_delta`
   (relative to previous input) and one packed byte:
   * bits 0–2: `button` (`1`=Jump, `2`=Left, `3`=Right)
   * bit 3: `player2`
   * bit 4: `down`
3. **physics chunks**: `chunk_count` chunks of `chunk_size` frames (last chunk
   may be shorter). Each chunk begins with a `uint32` **field mask**
   (`FieldBits` in `codec.hpp`) selecting which fields are present. Fields use
   varint frame deltas and **predictive XOR-delta** coding (value = prediction +
   `unzigzag(delta)`, prediction = `2·prev − prevprev`) for dense floats/doubles;
   sparse fields (gravity, vehicle size, speed, platformer velocity, rotation
   speed, slope rotation, land time, flags) store only changed frames. Player 2
   fields mirror player 1 when the dual-mode flag is set. Dash-vector fields
   (`p1_dash_*`, `p2_dash_*`) are stored as sparse doubles.
4. **visual anchors**: varint `anchor_count`, then per anchor a varint length +
   opaque bytes (same blobs as in `.skk`).

Because every field is delta-coded and re-decoded deterministically, `.skc` ↔
`.skk` conversion is **lossless** (verified by `skcconv verify`).

---

## Tests

```powershell
ctest --build-and-test . build --config Release
# or directly:
build\Release\skc_test.exe
```

The suite checks synthetic round-trip losslessness and decodes a real
bot-produced `.skc` (`Rolling Ball.skc`).

---

## License

MIT — see [LICENSE](LICENSE).
