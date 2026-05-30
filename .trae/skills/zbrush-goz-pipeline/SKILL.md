---
name: "zbrush-goz-pipeline"
description: "Guide for ZBrush GoZ binary pipeline: C++ GoZ SDK read/write, Python/ZScript bridge, CGAL integration, and common pitfalls. Invoke when building ZBrush plugins that need to exchange mesh data (vertices, faces, PolyGroups, Mask, UV) via GoZ binary format with C++ processing."
---

# ZBrush GoZ Binary Pipeline Skill

## When to Use

Invoke this skill when:
- Building a ZBrush plugin that needs C++ mesh processing
- Need to export from ZBrush, process in C++/CGAL, import back with PolyGroups/Mask/UV preserved
- Debugging GoZ write/read failures
- Need to understand GoZ binary format (TAGs, sizes, layout)
- FACE4/triangle quad representation questions

## Key Principle

ZBrush uses `.goz` file suffix to auto-detect GoZ binary format for both `Tool:Export` and `Tool:Import`. This one suffix is all you need — no OBJ, no FBX, no external converter.

## Architecture

```
ZBrush ──Tool:Export(.goz)──→ C++ EXE ──Tool:Import(.goz)──→ ZBrush
         含 MASK16_LIST          处理(PMP/平滑/...)        含 PolyGroups
         含 GROUPS_LIST                                    含 Mask
```

## GoZ Binary Format Quick Reference

- File header: 32 bytes, magic `"GoZb"` at offset 0
- Block header: 16 bytes `{tag(int), size(int), iCount(int), modifier(float)}`
- `size` includes the 16-byte header itself
- All integer types are little-endian

### Essential TAGs

| TAG | Enum | Type | Bytes/elem | iCount |
|-----|------|------|-----------|--------|
| POINT_LIST | 10001 | float×3 | 12 | vtx count |
| FACE4_LIST_FORMAT_1 | 20001 | int×4 | 16 | face count |
| FACE3_LIST | 20003 | int×3 | 12 | face count |
| UV4_LIST | 25001 | float×8 | 32 | face count |
| MASK16_LIST | 30002 | uint16 | 2 | vtx count |
| MRGB_LIST | 35001 | uint8×4 | 4 | vtx count |
| GROUPS_LIST | 40001 | int16 | 2 | face count |
| CREASE_LIST | 40002 | uint8 | 1 | face count |
| END_OF_FILE | 0 | — | 0 | 0 |

**FACE4 key detail**: Triangle = 4th index is `-1` (0xFFFFFFFF). ZBrush default format is `FACE4_LIST_FORMAT_1`.

### Data Size Formulas

```
POINTS:  12 × vertexCount
FACE4:   16 × faceCount
MASK16:   2 × vertexCount
MRGB:     4 × vertexCount
GROUPS:   2 × faceCount
CREASE:   1 × faceCount
UV4:     32 × faceCount   (4 pairs of U,V floats)
UV3:     24 × faceCount   (3 pairs of U,V floats)
```

**MASK16**: `0xFFFF` = unmasked, `0x0000` = fully masked.
**GROUPS**: signed int16 per face. ZBrush "PolyGroup 2" = value 2.

## GoZ_Mesh Class (C++ SDK)

```cpp
// Read
GoZ_Mesh mesh;
mesh.readMesh("model.goz");

// Key fields:
mesh.m_vertexCount;              // int
mesh.m_vertices;                 // vector<float>, size = 3*vertexCount
mesh.m_faceCount;                // int
mesh.m_faceType;                 // 20001/20002/20003
mesh.m_vertexIndices;            // vector<int>, size = 4*faceCount (FACE4)
mesh.m_groups;                   // vector<short>, size = faceCount
mesh.m_mask;                     // vector<uint16_t>, size = vertexCount
mesh.m_uvs;                      // vector<float>, size = 8*faceCount (UV4)
mesh.m_crease;                   // vector<char>, size = faceCount
mesh.m_mrgb;                     // vector<uint32_t>, size = vertexCount

// Write
mesh.writeMesh("output.goz");    // returns bool
```

`writeMesh` block order (DO NOT change):
```
MESH → MATERIAL → FLAGS → POINTS → FACE → UV → MASK → MRGB → GROUPS
→ TEXTURE_MAP → NORMAL_MAP → DISPLACEMENT_MAP → CREASE → END_OF_FILE
```

Empty vectors are auto-skipped by `writeMesh`.

## ZBrush Export/Import

### Python (zbrushcore, ZBrush 2026+)

```python
# Export
zbc.set_next_filename("path/to/file.GoZ")
zbc.press("Tool:Export")
zbc.update()

# Import
zbc.set_next_filename("path/to/file.GoZ")
zbc.press("Tool:Import")
zbc.update()
```

Complete round-trip:
```python
import subprocess, tempfile, os

tmp_in  = os.path.join(tempfile.gettempdir(), "in.GoZ")
tmp_out = os.path.join(tempfile.gettempdir(), "out.GoZ")

zbc.set_next_filename(tmp_in)
zbc.press("Tool:Export")
zbc.update()

subprocess.run([exe, tmp_in, tmp_out], check=True, cwd=exe_dir)

zbc.set_next_filename(tmp_out)
zbc.press("Tool:Import")
zbc.update()
```

### ZScript (ZBrush 2021+)

```
[VarDef,expName,"zmeshmend_export.goz"]
[VarDef,impName,"zmeshmend_import.goz"]

[FileExecute,dllPath,"FileDelete",#impPath]    // delete old output first

[FileNameSetNext,expPath]
[IPress,Tool:Export]

[FileExecute,dllPath,"LaunchAppWithFile",#exePath] // launch C++ EXE

[FileNameSetNext,impPath]
[IPress,Tool:Import]
```

**Critical**: In zero-arg mode (ZScript launch), the C++ EXE must call `SetCurrentDirectoryA` to its own directory — ZScript does NOT set CWD.

## Quad-Preserving Fill Pattern (build_output_goz)

When CGAL hole-filling adds new triangles to a quad-dominant mesh:

1. Copy `in_goz.m_vertexIndices` wholesale → `out_goz.m_vertexIndices` (preserves original quads)
2. Track which CGAL faces are "original" (skip them during append)
3. Append only new fill faces: `v0, v1, v2, -1` (FACE4 triangle representation)
4. Copy `in_goz.m_groups` → `out_goz.m_groups`, append `new_group` for fill faces
5. Compute `new_group = max(existing_group_ids) + 1`; fallback to 2

## ⚠️ CRITICAL PITFALL: Per-Face Array Size Mismatch

**After appending fill faces, ALL per-face and per-vertex arrays MUST be resized to match the new `m_faceCount` / `m_vertexCount`.**

If any array's `size()` ≠ expected count, `writeGoZBloc(tag, count, buffer)` will compute `dataSize` from the new count but read from the old-sized buffer → buffer overrun → `fwrite` fails → `writeMesh` returns false.

**Always do this after face/vertex appending completes:**

```cpp
// UV: per_face = 8 for UV4, 6 for UV3
if (!out_goz.m_uvs.empty())
    out_goz.m_uvs.resize(out_goz.m_faceCount * per_face_floats, 0.0f);

// Crease: 1 byte per face
if (!out_goz.m_crease.empty())
    out_goz.m_crease.resize(out_goz.m_faceCount, 0);

// Mask: 2 bytes per vertex
if (!out_goz.m_mask.empty())
    out_goz.m_mask.resize(out_goz.m_vertexCount, 0xFFFF);

// Groups: 2 bytes per face
if (!out_goz.m_groups.empty())
    out_goz.m_groups.resize(out_goz.m_faceCount, new_group);
```

**Golden rule**: `writeGoZBloc(tag, count, buffer)` requires `buffer.size()` == expected bytes for that `(tag, count)`.

## Common Pitfalls

1. **Vertex paint on fill vertices**: `m_mrgb` carries per-vertex color. New vertices default to `0xFFFFFFFF` (white). Solution: `out_goz.m_mrgb.clear()` to omit MRGB block.
2. **Python import false negative**: For smooth/relax (no topology change), don't check face count to detect success — face count stays the same. Just trust the import if no exception.
3. **File conflict**: Always `FileDelete` old output file before launching the EXE in ZScript.
4. **Y-axis flip**: Check `m_flags` for `GoZ_VERTEX_FLIP_Y` (value 2) if model appears upside-down.
5. **Zero-arg CWD**: ZScript's `LaunchAppWithFile` does not guarantee CWD = EXE directory. Call `SetCurrentDirectoryA` manually.

## Reference Files

Full documentation: `doc/goz-pipeline-guide.md`

Source files to copy for any new ZBrush→C++ project:
- `GoZ_Binary.h` — TAG definitions, header struct, flags
- `GoZ_Utils.h` / `GoZ_Utils.cpp` — low-level file I/O, block read/write/skip
- `GoZ_Mesh.h` / `GoZ_Mesh.cpp` — high-level readMesh/writeMesh/clear
- `GoZ_Config.h` — cross-platform setup (WIN/MAC)
