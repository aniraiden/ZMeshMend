# GoZ Binary 管线完全指南

> 基于 Pixologic GoZ SDK + ZMeshMend 实战经验整理。
> 适用场景：ZBrush 插件开发，C++ CGAL 处理 + Python/ZScript 桥接。

---

## 1. 概述

GoZ（Go ZBrush）是 Pixologic 官方提供的 ZBrush 与其他 DCC 工具之间交换数据的协议。
GoZ **binary 格式**（文件头 magic `"GoZb"`）是最高效的方式——单文件包含
顶点、面、UV、遮罩（Mask）、PolyGroup、顶点色（MRGB）、Crease 等全部数据。

ZBrush **凭文件后缀 `.goz`（小写）自动识别** GoZ binary 格式——
无论是 `Tool:Export` 还是 `Tool:Import`，只要文件名以 `.goz` 结尾，
ZBrush 就会走 GoZ binary 路径，输出/输入含 PolyGroups + Mask 的完整数据。

### 术语

| 术语 | 含义 |
|------|------|
| GoZ binary | 文件以 `GoZb` 开头的二进制格式，每 block = 16 字节 header + data |
| GoZ SDK | Pixologic 官方 C++ 代码（GoZ_Binary.h, GoZ_Utils.cpp/h, GoZ_Mesh.cpp/h） |
| TAG | 每个 block 的类型标识（20001=FACE4, 30001=MASK16, 20005=GROUPS...） |
| iCount | block header 中的元素计数（顶点数、面数、mask 采样数等） |
| modifier | block header 中的浮点修饰符（仅 DISPLACEMENT_MAP_PATH 使用） |

---

## 2. 文件格式详解

### 2.1 全局文件头（32 字节）

```
偏移  大小   内容
0     4      "GoZb" (magic)
4     27     描述文本（null-terminated，不足 27 字节用 0 填充）
31    1      0（确保 null-terminated）
```

`openGoZFile4Write(fileName, "Exported by ZMeshMend")` 将描述写入 4-30 字节区域。

### 2.2 Block Header（16 字节）

```cpp
struct GoZ_Header {
    int tag;       // Block 类型标识（GoZ_TAG_* 枚举值）
    int size;      // 本 block 总字节数（= 16 + dataSize + padSize）
    int iCount;    // 元素计数
    float modifier; // 修饰符（仅 DISPLACEMENT_MAP_PATH 使用）
};
```

- `size` 包含 header 自身的 16 字节
- `dataSize = size - 16`（即实际数据载荷）
- 字符串类型 block 可能含 padding 到 4 字节边界

### 2.3 完整 TAG 参考表

| TAG 常量 | 枚举值 | 数据类型 | 每元素字节 | iCount 含义 |
|----------|--------|----------|-----------|-------------|
| `GoZ_TAG_END_OF_FILE` | 0 | — | 0 | 0（必须为最后 block） |
| `GoZ_TAG_MESH` | 1 | char[] | strlen+1 | 1 |
| `GoZ_TAG_MATERIAL` | 2 | char[] | strlen+1 | 1 |
| `GoZ_TAG_FLAGS` | 5001 | uint32 | 4 | 1 |
| `GoZ_TAG_POINT_LIST` | 10001 | float×3 | 12 | vertexCount |
| `GoZ_TAG_DPOINT_LIST` | 10002 | double×3 | 24 | vertexCount |
| `GoZ_TAG_FACE4_LIST_FORMAT_1` | 20001 | int×4 | 16 | faceCount |
| `GoZ_TAG_FACE4_LIST_FORMAT_2` | 20002 | int×4 | 16 | faceCount |
| `GoZ_TAG_FACE3_LIST` | 20003 | int×3 | 12 | faceCount |
| `GoZ_TAG_UV4_LIST` | 25001 | float×8 | 32 | faceCount |
| `GoZ_TAG_UV3_LIST` | 25002 | float×6 | 24 | faceCount |
| `GoZ_TAG_MASK8_LIST` | 30001 | uint8 | 1 | vertexCount |
| `GoZ_TAG_MASK16_LIST` | 30002 | uint16 | 2 | vertexCount |
| `GoZ_TAG_MRGB_LIST` | 35001 | uint8×4 | 4 | vertexCount |
| `GoZ_TAG_GROUPS_LIST` | 40001 | int16 | 2 | faceCount |
| `GoZ_TAG_CREASE_LIST` | 40002 | uint8 | 1 | faceCount |
| `GoZ_TAG_TEXTURE_MAP_PATH` | 45001 | char[] | strlen+1 | 1 |
| `GoZ_TAG_NORMAL_MAP_PATH` | 50001 | char[] | strlen+1 | 1 |
| `GoZ_TAG_DISPLACEMENT_MAP_PATH` | 55001 | char[] | strlen+1 | 1 |

**关键数据大小计算公式**：

```
POINTS:      12 × vertexCount  字节
FACE4:       16 × faceCount    字节
FACE3:       12 × faceCount    字节
UV4:         32 × faceCount    字节
UV3:         24 × faceCount    字节
MASK16:       2 × vertexCount  字节
MRGB:         4 × vertexCount  字节
GROUPS:       2 × faceCount    字节
CREASE:       1 × faceCount    字节
```

### 2.4 FACE4 格式细节

ZBrush 默认使用 `FACE4_LIST_FORMAT_1`：
- 每个面固定 4 个 int 索引
- 四边形：4 个有效索引
- 三角形：第 4 个索引 = `-1`（0xFFFFFFFF）

`FACE4_LIST_FORMAT_2` 是另一种表示：三角形用 `(a, b, c, c)`。

### 2.5 遮罩格式（MASK16_LIST）

- `0xFFFF` = 完全未遮罩（默认值）
- `0x0000` = 完全遮罩
- 中间值 = 半透遮罩

### 2.6 PolyGroup 格式（GROUPS_LIST）

- 每个面 2 字节 signed int16
- 值 0 和 1 在 ZBrush 中可能是默认/保留值
- ZBrush 中看到的 "PolyGroup 2" 对应数值 2
- 工具 > PolyGroups > Auto Groups 的最小起始值为 2

### 2.7 Crease 格式（CREASE_LIST）

- 每个面 1 字节
- bit 0-1: edge1 crease; bit 2-3: edge2; bit 4-5: edge3; bit 6-7: edge4
- 非零值表示对应边 creased

---

## 3. C++ API 使用

### 3.1 核心文件（来自 Pixologic SDK）

| 文件 | 用途 |
|------|------|
| `GoZ_Binary.h` | TAG 枚举、Header 结构体、Flags 枚举 |
| `GoZ_Utils.h` / `.cpp` | 文件打开/关闭、block 读写/跳过、Pref 文件读写 |
| `GoZ_Mesh.h` / `.cpp` | 高级封装：`readMesh()` / `writeMesh()` / `clear()` |

### 3.2 GoZ_Mesh 成员变量清单

```cpp
class GoZ_Mesh {
    string          m_name;           // 网格名称
    string          m_material;       // 材质名称
    unsigned int    m_flags;          // GoZ_FLAGS 组合
    int             m_vertexCount;    // 顶点数
    vector<float>   m_vertices;       // ×3: x,y,z
    int             m_faceCount;      // 面数
    int             m_faceType;       // 20001/20002/20003
    vector<int>     m_vertexIndices;  // ×4(FACE4) 或 ×3(FACE3)
    int             m_uvFaceType;     // 25001/25002/0
    vector<float>   m_uvs;            // ×8(UV4) 或 ×6(UV3)
    vector<uint16_t> m_mask;          // vertexCount 个
    vector<uint32_t> m_mrgb;          // vertexCount 个
    vector<int16_t>  m_groups;        // faceCount 个
    vector<char>    m_crease;         // faceCount 个
    string          m_diffuseMap;     // 贴图路径
    string          m_normalMap;      // 法线贴图路径
    string          m_displacementMap;// 置换贴图路径
    float           m_displacementScale;
};
```

### 3.3 读取 GoZ 文件

```cpp
#include "GoZ_Mesh.h"

GoZ_Mesh mesh;
if (mesh.readMesh("model.goz")) {
    // 读取成功
    printf("顶点: %d, 面: %d\n", mesh.m_vertexCount, mesh.m_faceCount);
    // mesh.m_vertices: [x0,y0,z0, x1,y1,z1, ...]  共 3*vertexCount 个 float
    // mesh.m_vertexIndices: [a,b,c,d, ...]         共 4*faceCount 个 int（FACE4）
    // mesh.m_groups: [g0, g1, ...]                  共 faceCount 个 short
    // mesh.m_mask: [m0, m1, ...]                    共 vertexCount 个 uint16_t
}
```

### 3.4 写入 GoZ 文件

```cpp
GoZ_Mesh mesh;
// 填充 mesh 各字段...
mesh.m_name = "MyMesh";
mesh.m_vertexCount = N;
mesh.m_vertices = { /* 3*N floats */ };
mesh.m_faceCount = M;
mesh.m_faceType = GoZ_TAG_FACE4_LIST_FORMAT_1;
mesh.m_vertexIndices = { /* 4*M ints, 三角形填 v3=-1 */ };
mesh.m_mask = { /* N shorts, 0xFFFF 默认 */ };
mesh.m_groups = { /* M shorts */ };

if (mesh.writeMesh("output.goz")) {
    printf("写入成功\n");
} else {
    printf("写入失败！\n");
}
```

### 3.5 writeMesh 写出顺序（不可更改）

```
MESH → MATERIAL → FLAGS → POINT_LIST → FACE →
UV → MASK → MRGB → GROUPS →
TEXTURE_MAP → NORMAL_MAP → DISPLACEMENT_MAP → CREASE →
END_OF_FILE
```

空字段会自动跳过（`m_uvs.empty()` 就不写 UV block）。

### 3.6 底层 API（GoZ_Utils）

```cpp
// 打开文件（自动读/写 32 字节 GoZb 文件头）
FILE* f = GoZ_Utils::openGoZFile4Read("model.goz");
FILE* f = GoZ_Utils::openGoZFile4Write("model.goz", "My Exporter");

// 读一个 block
GoZ_Header hdr;
GoZ_Utils::readGoZBlocHeader(f, &hdr);     // 读 16 字节 header
GoZ_Utils::readGoZBlocData(f, &hdr, buf);   // 读 data 到 buf
GoZ_Utils::skipGoZBloc(f, &hdr);            // 跳过不认识的 block

// 写一个 block
GoZ_Utils::writeGoZBloc(f, tag, itemCount, dataPtr, modifier);

// 关闭
GoZ_Utils::closeGoZFile(f);
```

---

## 4. ZBrush 端桥接

### 4.1 Python (zbrushcore API, ZBrush 2026+)

**导出 GoZ**：

```python
import os
zbc.set_next_filename("C:/Temp/export.GoZ")
zbc.press("Tool:Export")
zbc.update()
```

**导入 GoZ**：

```python
zbc.set_next_filename("C:/Temp/import.GoZ")
zbc.press("Tool:Import")
zbc.update()
```

**完整 round-trip 模式**：

```python
import subprocess, tempfile, os

tmp_in  = os.path.join(tempfile.gettempdir(), "zmeshmend_in.GoZ")
tmp_out = os.path.join(tempfile.gettempdir(), "zmeshmend_out.GoZ")

# 1. 从 ZBrush 导出
zbc.set_next_filename(tmp_in)
zbc.press("Tool:Export")
zbc.update()

# 2. 调用 C++ 处理程序
subprocess.run([
    "zmeshmend_core.exe", tmp_in, tmp_out,
    "--stitch", "--fill-holes", "--select-fill"
], check=True, cwd=exe_dir)

# 3. 导回 ZBrush
zbc.set_next_filename(tmp_out)
zbc.press("Tool:Import")
zbc.update()

# 4. 清理临时文件
for f in [tmp_in, tmp_out]:
    try: os.remove(f)
    except: pass
```

### 4.2 ZScript (ZBrush 2021+)

ZScript 使用 `LaunchAppWithFile` 启动 EXE，通过配置文件传递参数。

```
[VarDef,expName,"zmeshmend_export.goz"]
[VarDef,impName,"zmeshmend_import.goz"]
[VarDef,expPath,[StrMerge,dataDir,expName]]
[VarDef,impPath,[StrMerge,dataDir,impName]]

// 删除旧的输入输出文件
[FileExecute,dllPath,"FileDelete",#expPath]
[FileExecute,dllPath,"FileDelete",#impPath]

// 导出 GoZ（.goz 后缀 → ZBrush 自动写 GoZb 格式）
[FileNameSetNext,expPath]
[IPress,Tool:Export]

// 启动 C++ EXE（零参数模式）
[If,[FileExists,exePath],
    [VarSet,errCode,[FileExecute,dllPath,"LaunchAppWithFile",#exePath]]
]

// EXE 内部自动读取 zmeshmend_export.goz，处理后写入 zmeshmend_import.goz

// 导入结果
[FileNameSetNext,impPath]
[IPress,Tool:Import]
```

ZScript 文件后缀 `.GoZ`（大写 G、大写 Z）在 `FileNameSetNext` 中等价于 `.goz`。

**zero-arg 模式**的 C++ 代码：

```cpp
const char* in_path  = "zmeshmend_export.goz";
const char* out_path = "zmeshmend_import.goz";
if (argc < 3) {
    SetCurrentDirectoryA(自身exe目录);
    // 读取 zmeshmend_config.txt 获取操作类型
    // 读取 in_path → 处理 → 写入 out_path
}
```

---

## 5. 高级集成模式

### 5.1 将 GoZ_Mesh 转换为 CGAL Surface_mesh

```cpp
#include <CGAL/Surface_mesh.h>
typedef CGAL::Exact_predicates_inexact_constructions_kernel K;
typedef CGAL::Surface_mesh<K::Point_3> Mesh;

Mesh to_cgal(const GoZ_Mesh& goz) {
    Mesh mesh;
    for (int vi = 0; vi < goz.m_vertexCount; ++vi) {
        float* v = &goz.m_vertices[vi * 3];
        mesh.add_vertex(K::Point_3(v[0], v[1], v[2]));
    }
    for (int fi = 0; fi < goz.m_faceCount; ++fi) {
        int* f = &goz.m_vertexIndices[fi * 4];  // FACE4
        int v3 = f[3];
        if (v3 < 0 || v3 == f[2])
            mesh.add_face(Mesh::Vertex_index(f[0]),
                          Mesh::Vertex_index(f[1]),
                          Mesh::Vertex_index(f[2]));
        else
            mesh.add_face(Mesh::Vertex_index(f[0]), Mesh::Vertex_index(f[1]),
                          Mesh::Vertex_index(f[2]), Mesh::Vertex_index(f[3]));
    }
    return mesh;
}
```

### 5.2 build_output_goz：保留 quad 拓扑 + 追加填充面

当 CGAL 做补洞时，新面是三角的，但原始面可能含 quad。策略：

```
1. out_goz.m_faceCount = in_goz.m_faceCount         ← 从原始面数开始
2. out_goz.m_vertexIndices = in_goz.m_vertexIndices  ← 整段拷贝原始 face4
3. 构建 orig_faces 集合（CGAL mesh 中对应原始面的 face index）
4. 遍历 mesh.faces()：
   - 如果 f ∈ orig_faces → 跳过（已在步骤2中写入）
   - 如果 f ∉ orig_faces → 作为新三角面追加到 out_goz：
     v0, v1, v2, -1    （FACE4 格式，-1 表示三角面）
5. out_goz.m_faceCount += 填充面数
```

### 5.3 PolyGroup 分配

```cpp
// 检测是否有输入 group 数据
bool has_input_groups = !in_goz.m_groups.empty()
                     && (int)in_goz.m_groups.size() == in_goz.m_faceCount;

// 计算新 group ID（max_group + 1）
short new_group = 2;
if (has_input_groups) {
    short max_group = 0;
    for (short g : in_goz.m_groups)
        if (g > max_group) max_group = g;
    new_group = max_group > 0 ? (short)(max_group + 1) : 2;
}

// 拷贝原始 groups，追加新 group 给填充面
if (has_input_groups)
    out_goz.m_groups = in_goz.m_groups;
for (每个填充面)
    if (has_input_groups)
        out_goz.m_groups.push_back(new_group);
```

---

## 6. 踩坑经验

### 6.1 🔴 致命：per-face 数组未对齐导致 writeMesh 失败

**症状**：`writeMesh` 返回 `false`，无任何 block 级别报错。

**根因**：追加填充面后 `m_faceCount` 增加（如 127160 → 139292），但 `m_uvs`、`m_crease` 仍保持原有长度（基于原 faceCount）。
`writeGoZBloc(pFile, tag, m_faceCount, m_uvs.data())` 按**新的** m_faceCount 计算 dataSize（=32×新faceCount），但 buffer 实际只有 32×旧faceCount 字节，
`fwrite` 越过缓冲区末尾 → 短写/越界 → 返回 false。

**修复**：在 face/vertex 全部追加完毕后，对齐所有 per-face per-vertex 数组：

```cpp
// UV：按 m_uvFaceType 计算每面 float 数
if (!out_goz.m_uvs.empty()) {
    int per_face = (out_goz.m_uvFaceType == GoZ_TAG_UV3_LIST) ? 6 : 8;
    out_goz.m_uvs.resize(out_goz.m_faceCount * per_face, 0.0f);
}
// Crease：每面 1 byte
if (!out_goz.m_crease.empty() && (int)out_goz.m_crease.size() != out_goz.m_faceCount)
    out_goz.m_crease.resize(out_goz.m_faceCount, 0);
// Mask：每顶点 2 bytes
if (!out_goz.m_mask.empty() && (int)out_goz.m_mask.size() != out_goz.m_vertexCount)
    out_goz.m_mask.resize(out_goz.m_vertexCount, 0xFFFF);
// Groups：每面 2 bytes
if (!out_goz.m_groups.empty() && (int)out_goz.m_groups.size() != out_goz.m_faceCount)
    out_goz.m_groups.resize(out_goz.m_faceCount, new_group);
```

**核心原则**：`writeGoZBloc(tag, count, buffer)` 中 `count` 和 `buffer.size()` 必须严格匹配 tag 对应的数据量。

### 6.2 🟡 补洞顶点被涂白色 PolyPaint

**原因**：透传 `in_goz.m_mrgb`（顶点色数组），新顶点默认 `0xFFFFFFFF`（白色）。
**修复**：`out_goz.m_mrgb.clear()`——让 ZBrush 不读取 MRGB block，保持原 PolyPaint。

### 6.3 🟡 VERTEX_FLIP_Y 导致模型上下颠倒

GoZ 格式中 `m_flags` 可以设置 `GoZ_VERTEX_FLIP_Y`（值为 2），某些 DCC 工具交换数据时会自动 Y 轴翻转。
如果 ZBrush 导入出现模型颠倒，检查 `m_flags` 字段。

### 6.4 🟡 Python _import_goz 假阴性

**症状**：smooth/relax 后弹窗"导入失败"，但模型实际已正确导入。

**原因**：用 "导入前后**面数是否变化**" 判断成功——smooth/relax 不改变拓扑，面数不变。
**修复**：直接信任 `Tool:Import` 调用，不抛异常即成功。

### 6.5 🟢 零参数模式的 CWD

ZScript `LaunchAppWithFile` 启动 EXE 时，**CWD 不一定**是 EXE 所在目录。
必须在 EXE 启动后自行切换：

```cpp
if (argc < 3) {
    char exeDir[MAX_PATH];
    GetModuleFileNameA(NULL, exeDir, MAX_PATH);
    char* slash = strrchr(exeDir, '\\');
    if (slash) *slash = '\0';
    SetCurrentDirectoryA(exeDir);
}
```

### 6.6 🟢 文件冲突

ZScript 启动 EXE **之前**，必须先 `FileDelete` 旧的输出文件：

```
[FileExecute,dllPath,"FileDelete",#impPath]       // 删除旧输出
[FileExecute,dllPath,"LaunchAppWithFile",#exePath] // 启动 C++ EXE
```

否则旧文件残留可能导致 ZBrush 导入过期数据。

---

## 7. 跨平台注意事项

- **Windows**：共享文件夹 `C:\Users\Public\Pixologic\`
- **macOS**：共享文件夹 `/Users/Shared/Pixologic/`
- GoZ binary 本身没有字节序问题（Little Endian，Windows/Mac Intel 统一）
- 文件路径分隔符：`\\` (Win) vs `/` (Mac)

---

## 8. 完整最小示例

```cpp
// 最小 GoZ 读写示例
#include "GoZ_Mesh.h"
#include <cstdio>

int main() {
    // 读取
    GoZ_Mesh mesh;
    if (!mesh.readMesh("input.goz")) {
        printf("读取失败\n");
        return 1;
    }
    printf("输入: %d 顶点, %d 面\n", mesh.m_vertexCount, mesh.m_faceCount);

    // 处理（示例：所有顶点沿 Y 轴位移 5 单位）
    for (int i = 0; i < mesh.m_vertexCount; ++i)
        mesh.m_vertices[i * 3 + 1] += 5.0f;

    // 写出
    if (mesh.writeMesh("output.goz"))
        printf("输出成功\n");
    else
        printf("输出失败\n");

    return 0;
}
```

---

## 9. 参考资源

- Pixologic GoZ SDK（GoZ_Binary.h, GoZ_Utils.cpp/h, GoZ_Mesh.cpp/h）
- 本指南配套代码：ZMeshMend 仓库 `ZMeshMendData/` 目录
- ZBrush → Tool:Export（.goz 后缀）= GoZ binary 导出
- ZBrush → Tool:Import（.goz 后缀）= GoZ binary 导入
- [GoZ_Binary.h](file:///h:/vibe_coding/ZMeshMend/ZMeshMend/ZMeshMendData/GoZ_Binary.h): TAG 枚举 + Header 结构体
- [GoZ_Mesh.h](file:///h:/vibe_coding/ZMeshMend/ZMeshMend/ZMeshMendData/GoZ_Mesh.h): 高级读写封装
- [GoZ_Utils.cpp](file:///h:/vibe_coding/ZMeshMend/ZMeshMend/ZMeshMendData/GoZ_Utils.cpp): 底层 block I/O
- [build_output_goz](file:///h:/vibe_coding/ZMeshMend/ZMeshMend/ZMeshMendData/zmeshmend_core.cpp): quad 保留 + 对齐修复
