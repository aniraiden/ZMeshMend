# ZMeshMend 架构文档

## 项目概述

ZBrush 网格孔洞自动修复插件。支持 CGAL 智能曲率感知填充、碎片移除和遮罩驱动的模型清理。

提供 **Python** 和 **ZScript** 两种版本，互不依赖。

## 目录结构

```
ZMeshMend/
├── ZMeshMend/                    # 插件主体
│   ├── ZMeshMend.py              # Python 版插件（ZBrush 2026+）
│   ├── ZMeshMend_ZScript.txt     # ZScript 版插件（ZBrush 2021+）
│   ├── ZMeshMend_config.txt      # 共用配置文件
│   ├── __init__.py               # Python 包初始化
│   └── init.py                   # 插件入口
├── ZMeshMendData/                # CGAL 核心和 GoZ 支持
│   ├── zmeshmend_core.cpp        # CGAL C++ 核心（补洞/碎片移除）
│   ├── zmeshmend_core.exe        # 编译后的 CGAL 可执行文件
│   ├── ZMeshMend_pipeline.py     # Python 管线辅助脚本
│   ├── GoZ_Mesh.h / .cpp         # GoZ 网格读写
│   ├── GoZ_Utils.h / .cpp        # GoZ 工具函数
│   ├── GoZ_Config.h / Binary.h   # GoZ 数据格式定义
│   ├── CMakeLists.txt            # CGAL 核心构建配置
│   ├── build.bat / run_build.ps1 # 构建脚本
│   └── *.dll                     # 运行时依赖（Boost/CGAL/压缩库）
├── pic/                          # 图片资源
│   └── cover.png
├── doc/                          # 项目文档
├── ZMeshMend_Launcher.py         # Python 版启动入口
├── .gitignore
└── README.md
```

## 核心模块

### 1. CGAL C++ 核心 (`zmeshmend_core.cpp`)

- 使用 `CGAL::Surface_mesh<EPICK::Point_3>` 作为网格数据结构
- 输入：OBJ 或 GoZ 格式
- 核心功能：
  - 边界检测：`PMP::extract_boundary_cycles()`
  - 孔洞填充：`PMP::triangulate_refine_and_fair_hole()` — 三角化 + 细分 + 光顺
  - 碎片移除：`PMP::connected_components()` + 面数阈值过滤
  - 边界缝合：`PMP::stitch_borders()`
- 输出：GoZ 或 OBJ（支持 full OBJ 保留 PolyGroup）
- 依赖：CGAL、Eigen3、Boost（通过 vcpkg）

### 2. Python 插件 (`ZMeshMend.py`)

- ZBrush 2026+ Python API
- 通过 `subprocess.run()` 调用 `zmeshmend_core.exe`
- 解析 `SUMMARY: faces_added=N` 获取填充面数
- 功能入口：
  - `do_close_all_holes()` — ZBrush 内置 Close Holes
  - `do_close_with_polygroup_mask()` — CGAL 补洞 + PolyGroup 标记
  - `do_mask_based_cleanup()` — 遮罩删除 + CGAL 补洞
  - `do_remove_small_fragments()` — 碎片移除
- 备用逻辑：
  - `_fill_hole_smart()` — Python 版曲率感知补洞（球拟合/质心 fan）
  - `_find_boundary_edges()` / `_build_boundary_loops()` — 边界检测

### 3. ZScript 插件 (`ZMeshMend_ZScript.txt`)

- ZBrush 2021+ 兼容
- 通过 `LaunchAppWithFile` 调用 `zmeshmend_core.exe`
- 失败时回退到 ZBrush 内置 `Close Holes`
- 功能入口：
  - `RunCGAL` — 调用 CGAL 补洞
  - `RemoveFragments` — 碎片移除
  - 对应 UI 按钮：Close All Holes、MendHoles + PolyGroup、Mask-Based Cleanup、Remove Small Fragments

### 4. Python 管线脚本 (`ZMeshMend_pipeline.py`)

- 独立命令行工具
- 调用 `zmeshmend_core.exe` 后将 patch OBJ 合并回原始 OBJ
- 核心函数：
  - `call_cgal_fill()` — 调用 CGAL 可执行文件
  - `merge_obj_with_patch()` — 合并 patch 到原始网格

## 数据流

### Python 版流程

```
ZBrush → Export OBJ → zmeshmend_core.exe → Import OBJ (patch) → Merge → ZBrush
                                                    ↓
                                          PolyGroup 标记填充面
```

### ZScript 版流程

```
ZBrush → Export OBJ → zmeshmend_core.exe → Import OBJ (patch) → ZBrush
                     (0-arg mode, reads zmeshmend_config.txt)
```

### GoZ 路径（备选）

```
ZBrush GoZ AppLink → zmeshmend_core.exe → GoZ 写回 ZBrush
```

## CGAL 核心补洞流程

```
1. 读取输入 (OBJ/GoZ) → Surface_mesh
2. stitch_borders（仅 ZScript 路径）
3. triangulate_faces（非三角面 → 三角面）
4. 检查 is_closed，若封闭则跳过
5. extract_boundary_cycles → 去重 → hole_seeds
6. 逐个 triangulate_refine_and_fair_hole
7. 可选：connected_components → 移除小碎片
8. 输出：GoZ 或 OBJ（区分 original_faces / fill_faces）
```

## 构建

```
cd ZMeshMendData
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg_path>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

要求：CMake 3.16+、C++17、CGAL（通过 vcpkg）、Eigen3
