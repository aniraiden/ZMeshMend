# ZMeshMend 项目参考手册

> **最后更新**：2026-05-30  
> **当前版本**：v1.1.0  
> **维护分支**：`smooth-hole-edges`  
> **本文档是项目的唯一权威参考。后续开发必须遵守本文档规定的算法和桥接方案，不得随意更改。**

---

## 1. 项目目标

ZMeshMend 是 ZBrush 网格孔洞自动修复插件，核心能力：

| 功能 | 说明 |
|------|------|
| **CGAL 曲率感知补洞** | 使用 `PMP::triangulate_refine_and_fair_hole()` 智能填充，输出 PolyGroup 标记的填充面 |
| **小碎片自动清理** | `PMP::connected_components()` + 面数阈值过滤 |
| **遮罩驱动清理** | 遮罩 → 锐化 → 扩大 → 删除 → CGAL 补洞 |
| **ZBrush 内置兜底** | `Close Holes` 作为 CGAL 失败时的后备方案 |
| **平滑开放边缘** | Chaikin 平滑 + 多圈 Laplacian falloff + 法线切平面投影（`smooth-hole-edges` 分支） |

提供两个版本：
- **Python 版**：ZBrush 2026+，使用 `subprocess.run()` 调用 EXE，支持 GoZ 和 OBJ
- **ZScript 版**：ZBrush 2021+，通过 ZFileUtils64.dll 的 `LaunchAppWithFile` 调用 EXE

---

## 2. 目录结构

```
ZMeshMend/
├── ZMeshMend/                         # 插件主体
│   ├── ZMeshMend.py                   # Python 版插件（ZBrush 2026+）
│   ├── ZMeshMend_ZScript.txt          # ZScript 版插件（ZBrush 2021+）
│   ├── ZMeshMend_config.txt           # 共用配置文件
│   ├── __init__.py                    # Python 包初始化
│   └── init.py                        # 插件入口
│
├── ZMeshMendData/                     # CGAL 核心 + GoZ 支持 + 汇编依赖
│   ├── zmeshmend_core.cpp             # CGAL C++ 主程序（补洞/碎片/平滑/GoZ）
│   ├── zmeshmend_core.exe             # 编译后的可执行文件
│   ├── GoZ_Mesh.h / .cpp              # GoZ 网格读写
│   ├── GoZ_Utils.h / .cpp             # GoZ 工具函数
│   ├── GoZ_Config.h / GoZ_Binary.h    # GoZ 数据格式定义
│   ├── ZMeshMend_pipeline.py          # 独立命令行管线脚本
│   ├── ZFileUtils64.dll               # ZScript 文件操作 DLL（v8.0+）
│   ├── CMakeLists.txt                 # CGAL 核心构建配置（MSVC + C++17）
│   ├── build.bat                      # 通用一键构建脚本（自动检测 vcpkg/cmake）
│   ├── run_build.ps1                  # 开发机专用构建脚本（硬编码路径）
│   └── *.dll                          # Boost 1.91+ 运行时 + gmp/mpfr/zstd/bzip/lzma
│
├── reference/                         # 外部参考资料（只读，不修改）
│   ├── ZFileUtils_2021_01A/
│   │   ├── ZFileUtils_TestZScript_2021.txt   # ZFileUtils 官方示例
│   │   ├── ZFileUtilsInc.txt                 # ZFileUtils 内置例程（FileRename/LaunchAppWithFile等）
│   │   └── MyPluginData/
│   │       ├── ZFileUtils64.dll              # 参考版 ZFileUtils DLL（不要使用）
│   │       └── readme.txt
│   └── zscripting.txt                  # ZScript 语法参考
│
├── doc/                               # 项目文档
│   ├── project-reference.md           # 本文档：项目权威参考
│   ├── architecture.md                # 架构文档
│   ├── smooth-open-edge.md            # 平滑开放边缘功能规划
│   ├── roadmap.md                     # 开发路线图
│   └── branches.md                    # 分支说明
│
├── Release/                           # 发布包
│   ├── Python_v1.1.0-smooth/          # Python 版发布包
│   └── ZScript_v1.1.0-smooth/         # ZScript 版发布包
│
├── .gitignore
└── README.md
```

---

## 3. 编译环境与依赖

### 3.1 当前开发机器

| 组件 | 路径 | 版本 |
|------|------|------|
| **源代码** | `H:\vibe_coding\ZMeshMend\ZMeshMend` | — |
| **CMake** | `C:\tools\cmake\cmake-4.3.3-windows-x86_64\bin\cmake.exe` | 4.3.3 |
| **vcpkg** | `H:\vibe_coding\vcpkg\vcpkg-master` | — |
| **vcpkg toolchain** | `H:\vibe_coding\vcpkg\vcpkg-master\scripts\buildsystems\vcpkg.cmake` | — |
| **CGAL**（通过 vcpkg） | `H:\vibe_coding\vcpkg\vcpkg-master\installed\x64-windows\include\CGAL/` | 6.1.1 |
| **Boost**（通过 vcpkg） | `H:\vibe_coding\vcpkg\vcpkg-master\installed\x64-windows\` | 1.91.0 |
| **MSVC** | `D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.34.31933` | VS2022 17.4 |
| **Windows SDK** | `10.0.22000.0` | Windows 10 |

### 3.2 构建命令

```powershell
# 当前开发机构建（直接在项目根目录运行）
$cmake = 'C:\tools\cmake\cmake-4.3.3-windows-x86_64\bin\cmake.exe'
$scriptDir = 'H:\vibe_coding\ZMeshMend\ZMeshMend\ZMeshMendData'
$buildDir = Join-Path $scriptDir 'build'
$toolchain = 'H:\vibe_coding\vcpkg\vcpkg-master\scripts\buildsystems\vcpkg.cmake'

Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
& $cmake -S $scriptDir -B $buildDir -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
& $cmake --build $buildDir --config Release
Copy-Item "$buildDir\Release\zmeshmend_core.exe" "$scriptDir\zmeshmend_core.exe" -Force
```

### 3.3 编译选项（CMakeLists.txt）

```
C++17 标准
MSVC: /W3 /O2 /utf-8
运行时库: MultiThreaded (静态链接)，避免目标机器缺少 MSVC 运行时
```

**`/utf-8` 是必需的**：源文件包含中文注释，在 MSVC GBK 代码页（936）下不添加此标志会导致行号错位和编译失败。

### 3.4 汇编产物依赖

`zmeshmend_core.exe` 静态链接 CGAL 库，但动态依赖以下 DLL（必须随 EXE 一起发布）：

```
boost_atomic-vc143-mt-x64-1_91.dll
boost_chrono-vc143-mt-x64-1_91.dll
boost_container-vc143-mt-x64-1_91.dll
boost_date_time-vc143-mt-x64-1_91.dll
boost_filesystem-vc143-mt-x64-1_91.dll
boost_graph-vc143-mt-x64-1_91.dll
boost_iostreams-vc143-mt-x64-1_91.dll
boost_program_options-vc143-mt-x64-1_91.dll
boost_random-vc143-mt-x64-1_91.dll
boost_serialization-vc143-mt-x64-1_91.dll
boost_thread-vc143-mt-x64-1_91.dll
boost_wserialization-vc143-mt-x64-1_91.dll
gmp-10.dll, gmpxx-4.dll, mpfr-6.dll
z.dll, zstd.dll, bz2.dll, liblzma.dll
ZFileUtils64.dll
```

---

## 4. 参考文件

| 文件 | 路径 | 用途 |
|------|------|------|
| ZFileUtils 官方示例 | `reference/ZFileUtils_2021_01A/ZFileUtils_TestZScript_2021.txt` | ZScript 调用 ZFileUtils 的标准写法 |
| ZFileUtils 内置例程 | `reference/ZFileUtils_2021_01A/ZFileUtilsInc.txt` | `LaunchAppWithFile`、`FileRename`、`FileDelete` 等 API |
| ZScript 语法参考 | `reference/zscripting.txt` | ZScript 命令和语法规范 |

---

## 5. 当前算法清单

### 5.1 CGAL C++ 核心（zmeshmend_core.cpp）

#### 5.1.1 孔洞填充

```
算法：PMP::triangulate_refine_and_fair_hole()
流程：
  1. extract_boundary_cycles() 提取所有边界环
  2. 对每个环去重，得到 hole_seeds
  3. 逐个调用 triangulate_refine_and_fair_hole()
     - 三角化孔洞
     - 细分（refine）
     - 光顺（fair）使填充面与周围曲率一致
  4. 汇总 faces_added 统计信息
返回值：auto [ok, fo, vo] = ...;（C++17 结构化绑定解包 tuple）
        ok = fair_success（布尔值），fo/vo = 迭代器（忽略）
```

**注意**：此 API 在 CGAL 6.1 中返回 `std::tuple<bool, FaceOutIt, VertexOutIt>`，必须用结构化绑定解包，不能使用 `std::get<0>()`（MSVC 上有模板推导问题）。

#### 5.1.2 小碎片移除

```
算法：PMP::connected_components() + 面数阈值
流程：
  1. 计算连通分量（connected_components）
  2. 统计每个分量面数
  3. 计算阈值：max(fraction * total_faces, min_faces)
  4. 删除低于阈值的小分量（remove_connected_components）
  5. 清理孤立顶点（remove_isolated_vertices）
  6. 垃圾回收（collect_garbage）
  7. 从 original_faces 追踪器移除被删除的面
触发条件：opt_min_frac > 0 或 opt_min_faces > 0
```

#### 5.1.3 平滑开放边缘（smooth-hole-edges 分支）

```
算法：多圈 Chaikin + Laplacian + 切平面投影
流程：
  1. 提取边界环种子（extract_border_seeds）
  2. 对每个边界环：
     a. collect_border_loop() — 收集边界顶点序列
     b. expand_one_ring() 重复 N 次 — 获取 Ring 0 ~ Ring N
     c. Ring 0：chaikin_smooth_loop()
        - 加权平均：Vi' = 0.25*V_{i-1} + 0.5*Vi + 0.25*V_{i+1}
        - 迭代 smoothIterations 次
     d. Ring 1~N：laplacian_smooth_ring()
        - 邻域顶点平均
        - 迭代 smoothIterations 次
     e. 计算权重：weight = 0.5^ring（指数衰减）
        displacement = (smoothed_pos - original_pos) * weight
     f. project_to_tangent_plane()
        - 计算顶点原始位置的邻接面平均法线
        - 将位移向量投影到切平面：消除法线分量
        - 保持洞口体积不变
     g. 更新 mesh 顶点位置
触发条件：opt_smooth_border = true
纯平滑模式：opt_smooth_only = true（跳过补洞，直接输出平滑后的网格）
```

边界处理辅助函数（详见 [smooth-open-edge.md](smooth-open-edge.md)）：
- `extract_border_seeds(mesh)` → 去重后的边界环种子
- `collect_border_loop(mesh, seed)` → 有序顶点序列
- `expand_one_ring(mesh, src, visited)` → BFS 扩展一圈
- `vertex_normal(mesh, v)` → 顶点平均法线
- `chaikin_smooth_loop(points, iterations)` → Chaikin 平滑
- `laplacian_smooth_ring(mesh, ring_vs, iterations)` → Laplacian 平滑
- `project_to_tangent_plane(displaced, original, normal)` → 法线投影

#### 5.1.4 边界缝合

```
ZScript 路径（zero-arg 模式）：
  read_OBJ() + PMP::stitch_borders() — ZBrush Tool:Export 导出的 OBJ 是未缝合的
Python/CLI 路径：
  read_OBJ() 直接读取 — Python 写入的是已缝合的 OBJ
```

#### 5.1.5 GoZ 输入输出

```
输入：GoZ_Mesh::readMesh() — 读取 GoZ 二进制格式
  - 支持顶点/面/PolyGroup/Mask/UV
  - 自动转换为 CGAL Surface_mesh（仅三角面）

输出（补洞模式）：
  - GoZ 输出：build_output_goz() — 带 PolyGroup 标记
  - OBJ 输出（--full-obj）：group_1 = 原始面，group_2 = 填充面

输出（纯平滑模式）：
  - OBJ 输出：保留原始 PolyGroup（从原 OBJ 的 'g' 行重新映射）
```

### 5.2 Python 插件（ZMeshMend.py）

#### 5.2.1 CGAL 桥接

```
方案：subprocess.run() 调用 zmeshmend_core.exe
参数传递：通过命令行参数（CLI mode）
  Python 调用示例：
    zmeshmend_core.exe input.obj output.goz fill.obj
    zmeshmend_core.exe input.obj output.obj --smooth-border --smooth-only

CGAL 输出解析：从 stdout 解析 "SUMMARY: faces_added=N"
错误处理：检查 returncode、文件存在性、超时（300秒）
```

#### 5.2.2 Python 碎片移除

```
方案：纯 ZBrush Python — 不用 CGAL EXE
算法：
  1. 创建临时 PolyGroup
  2. 按 PolyGroup 分离
  3. 删除低于阈值的碎片
  4. 合并回原工具
```

#### 5.2.3 Python 补洞后备

```
ZBrush 内置 Close Holes 作为 CGAL 失败时的回退方案
```

### 5.3 ZScript 插件（ZMeshMend_ZScript.txt）

#### 5.3.1 CGAL 桥接方案（重要！不可更改）

```
方案：LaunchAppWithFile 调用 zmeshmend_core.exe（zero-arg 模式）
参数传递：通过 zmeshmend_config.txt 配置文件

流程：
  1. ZScript 构建配置文本，写入 temp 文件（MemSaveToFile → zmeshmend_tmp.txt）
  2. 校验 temp 文件存在（FileExists）
  3. 删除旧的 zmeshmend_config.txt（FileDelete）
  4. 原子重命名 temp → config（FileRename）
  5. LaunchAppWithFile 启动 EXE（零参数模式）
  6. EXE 在 zero-arg 模式下：
     - 切换到自身所在目录（SetCurrentDirectoryA）
     - 读取 zmeshmend_config.txt
     - 读取 zmeshmend_export.obj → stitch_borders → 处理 → 输出 zmeshmend_import.obj

桥接方式：配置文件（不是 bat 文件、不是 CLI 参数）
EXE 启动方式：ZFileUtils64.dll LaunchAppWithFile → #exePath（直接启动 EXE，不通过 bat）
配置文件写入：MemSaveToFile → temp file → FileRename（原子操作）
```

**禁止事项**：
- ❌ 不要使用 .bat 文件传递参数
- ❌ 不要在 `LaunchAppWithFile` 中使用 `#batPath`
- ❌ 不要修改 C++ EXE 来支持新的 ZScript 专用 CLI 参数（这会导致 zero-arg 模式和 CLI 模式偏离）
- ✅ 如需新参数，通过配置文件（zmeshmend_config.txt）传递

#### 5.3.2 ZScript 配置写入例程

```
SaveConfig：
  - 写入所有参数（smoothBorder=0 显式：确保正常补洞模式）
  - 使用 temp + FileRename 原子写入

SaveSmoothConfig：
  - 写入 smoothBorder=1 + iterations/rings
  - 同样使用 temp + FileRename 原子写入
  - RunSmooth 末尾调用 SaveConfig 恢复正常配置
```

#### 5.3.3 ZScript 按钮功能映射

| 按钮 | 例程 | EXE 行为 | 配置关键参数 |
|------|------|----------|-------------|
| 按钮 1：Close All Holes | `IPress Tool:Close Holes` | ZBrush 内置 | — |
| 按钮 2：MendHoles + PolyGroup | `RunCGAL` | 补洞 + PolyGroup 标记 | `smoothBorder=0`, `removeSmallFragments`（滑块控制） |
| 按钮 3：Mask-Based Cleanup | 遮罩处理 + `RunCGAL` | 删除遮罩区域 → 补洞 | `smoothBorder=0` |
| 按钮 4：Remove Small Fragments | `RemoveFragments` → `RunCGAL` | 补洞 + 碎片移除 | `smoothBorder=0`, `removeSmallFragments=1` |
| 按钮 5：Smooth Open Edge | `RunSmooth` | 仅平滑边界 | `smoothBorder=1`（由 SaveSmoothConfig 写入） |

### 5.4 Python 管线脚本（ZMeshMend_pipeline.py）

```
命令行工具：
  call_cgal_fill() → merge_obj_with_patch()
  将 CGAL 输出的 patch OBJ 合并回原始 OBJ（顶点焊接 + 去重）
```

---

## 6. 桥接机制总结

### 6.1 C++ EXE 运行模式

| 模式 | 触发条件 | 输入 | 参数来源 | 使用方 |
|------|----------|------|----------|--------|
| **Zero-arg** | `argc < 3` | `zmeshmend_export.obj` / `zmeshmend_import.obj` | `zmeshmend_config.txt` | ZScript |
| **CLI** | `argc >= 3` | `argv[1]` / `argv[2]` | 命令行参数 | Python |

### 6.2 配置变量对照表

| 配置键 | C++ 变量 | 用途 |
|--------|----------|------|
| `removeSmallFragments` | `opt_min_frac=0.01 / opt_min_faces=50`（当=1） | 是否启用碎片移除 |
| `fragmentMinFraction` | `opt_min_frac` | 碎片面数占比阈值 |
| `fragmentMinFaces` | `opt_min_faces` | 碎片最小面数阈值 |
| `smoothBorder` | `opt_smooth_border + opt_smooth_only` | 平滑边缘模式 |
| `smoothIterations` | `opt_smooth_iterations` | 平滑迭代次数 |
| `smoothRings` | `opt_smooth_rings` | 向内扩展圈数 |
| `maskSharpenPasses` | （不使用，ZScript 直接调用 ZBrush API） | 遮罩锐化次数 |
| `maskGrowRings` | （不使用，ZScript 直接调用 ZBrush API） | 遮罩扩大圈数 |

---

## 7. 发布规范

### 7.1 Release 目录结构

```
Release/
├── Python_v1.1.0-smooth/           # Python 版发布包
│   ├── ZMeshMend/                  # 插件文件夹（放入 ZBrush ZPlugs64）
│   │   ├── ZMeshMend.py
│   │   ├── ZMeshMend_ZScript.txt
│   │   ├── ZMeshMend_config.txt
│   │   ├── __init__.py
│   │   └── init.py
│   ├── ZMeshMendData/              # 数据文件夹（放入 ZBrush ZStartup）
│   │   ├── zmeshmend_core.exe
│   │   ├── ZMeshMend_pipeline.py
│   │   ├── ZFileUtils64.dll
│   │   └── *.dll（Boost + gmp 等运行时）
│   ├── ZMeshMend_Launcher.py       # 可选启动器
│   └── _README.md
│
└── ZScript_v1.1.0-smooth/          # ZScript 版发布包
    ├── ZMeshMend_ZScript.txt       # 放入 ZBrush ZPlugs64
    ├── ZMeshMend_config.txt        # 放入 ZBrush ZPlugs64
    ├── ZMeshMendData/              # 放入 ZBrush ZPlugs64
    │   ├── zmeshmend_core.exe
    │   ├── ZMeshMend_pipeline.py
    │   ├── ZFileUtils64.dll
    │   └── *.dll
    └── _README.md
```

### 7.2 同步 Release

```
每次修改以下文件后必须同步 Release：
  - zmeshmend_core.exe  →  Python + ZScript 两个包的 ZMeshMendData/
  - ZMeshMend_ZScript.txt → ZScript 包根目录 + Python 包的 ZMeshMend/
  - ZMeshMend_config.txt  → ZScript 包根目录 + Python 包的 ZMeshMend/
  - ZMeshMend.py          → Python 包的 ZMeshMend/
```

---

## 8. 编码规则

### 8.1 C++ (zmeshmend_core.cpp)

- C++17 标准
- 编译选项：`/W3 /O2 /utf-8`（MSVC）
- CGAL API：使用 `PMP` 命名空间（`namespace PMP = CGAL::Polygon_mesh_processing;`）
- 网格类型：`typedef CGAL::Surface_mesh<Point> Mesh;`（全局 `typedef`，所有辅助函数以此为准）
- 结构化绑定：`triangulate_refine_and_fair_hole` 的返回值用 `auto [ok, fo, vo] = ...;`
- 不使用 `std::get<>()` 解包 CGAL 函数返回值

### 8.2 ZScript (ZMeshMend_ZScript.txt)

- 参考 `reference/ZFileUtils_2021_01A/ZFileUtils_TestZScript_2021.txt` 的标准写法
- EXE 调用：仅使用 `LaunchAppWithFile #exePath`
- 配置文件写入：temp file + `FileRename` 原子操作
- 不使用 bat 文件

### 8.3 Python (ZMeshMend.py)

- 使用 `subprocess.run()` 调用 EXE（CLI 模式）
- 不修改 ZScript 的零参数执行路径

---

## 9. 相关文档索引

| 文档 | 路径 | 内容 |
|------|------|------|
| 架构文档 | [architecture.md](architecture.md) | 模块划分和数据流 |
| 平滑功能规划 | [smooth-open-edge.md](smooth-open-edge.md) | Smooth Open Edge 算法设计 |
| 开发路线图 | [roadmap.md](roadmap.md) | 已有功能 + 后续规划 |
| 分支说明 | [branches.md](branches.md) | Git 分支命名和用途 |
