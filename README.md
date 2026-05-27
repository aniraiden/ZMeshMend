# ZMeshMend

ZBrush 网格孔洞自动修复插件。一键闭合所有开放孔洞，支持 CGAL 智能曲率感知填充、碎片移除和遮罩驱动清理。

提供 **Python** 和 **ZScript** 两种版本，互不依赖。

## 版本对比

| | Python 版 | ZScript 版 |
|---|---|---|
| 文件 | `ZMeshMend/ZMeshMend.py` | `ZMeshMend/ZMeshMend_ZScript.txt` |
| 入口 | `ZMeshMend_Launcher.py` | 直接 Load .txt |
| 依赖 | ZBrush Python API (2026+) | ZFileUtils64.dll |
| 兼容 | ZBrush 2026+ | ZBrush 2021+（含老版本） |
| CGAL 核心 | subprocess 调用 | LaunchAppWithFile 调用 |

## 功能

| 功能 | 说明 |
|------|------|
| **MendHoles + PolyGroup** | CGAL 算法智能填充，曲率感知，自动创建 PolyGroup (orig + fill) |
| **Mask-Based Cleanup** | 遮罩 → 删除 → 智能填充，全自动流程 |
| **Remove Small Fragments** | CGAL 连通性分析自动清理孤立碎片 |
| **Close All Holes** | ZBrush 内置算法快速兜底 |

---

## Python 版安装

1. 将仓库放置在电脑上任意位置（无需放入 ZBrush 插件目录）。

2. 在 ZBrush 中：菜单 `ZScript` → `Python Scripting` → `Load`
   选择 `ZMeshMend_Launcher.py`

3. 插件面板将自动出现在 ZBrush UI 中。

> Python 版要求 ZBrush 2026 及以上版本。

## ZScript 版安装

1. 将 `ZMeshMend/ZMeshMend_ZScript.txt` 和 `ZMeshMendData/` 文件夹复制到：
   ```
   C:\Program Files\Maxon\ZBrush 20XX\ZStartup\ZPlugs64\
   ```

   最终结构：
   ```
   ZStartup\ZPlugs64\
     ZMeshMend_ZScript.txt
     ZMeshMendData\
       zmeshmend_core.exe
       ZFileUtils64.dll
       ...
   ```

2. 启动 ZBrush，菜单 `ZScript` → `Load` → 选择 `ZMeshMend_ZScript.txt`
   ZBrush 会编译生成 `.zsc`，插件出现在 `ZPlugin` → `ZMeshMend` 面板。

3. 如 `.zsc` 未生成，删除已有 `.zsc` 后重新 Load。

---

## 配置

两种版本共用同一配置文件 `ZMeshMend/ZMeshMend_config.txt`：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `maskSharpenPasses` | 1 | 遮罩锐化次数 |
| `maskGrowRings` | 1 | 遮罩扩展环数 |
| `removeSmallFragments` | 1 | 是否移除小碎片 |
| `fragmentMinFraction` | 0.01 | 碎片保留的最小面数占比 |
| `fragmentMinFaces` | 50 | 碎片保留的绝对最小面数 |

ZScript 版可直接在面板 Settings 子面板中调整。

---

## 可选：编译 CGAL 核心

两种版本共用 `zmeshmend_core.exe`。如需重新编译：

**前置条件：** Visual Studio 2022 + CMake 3.16+ + [vcpkg](https://github.com/microsoft/vcpkg)

```bash
# 1. 安装 CGAL（一次性）
cd C:\path\to\vcpkg
.\vcpkg install cgal:x64-windows

# 2. 编译
cd ZMeshMendData
powershell -File run_build.ps1
```

> 编译产物 `zmeshmend_core.exe` 会自动复制到 `ZMeshMendData/`。如未检测到，插件自动回退到 ZBrush 内置算法。

---

## 项目结构

```
ZMeshMend/
├── README.md
├── ZMeshMend_Launcher.py              # Python 版入口
├── ZMeshMend/
│   ├── __init__.py
│   ├── init.py
│   ├── ZMeshMend.py                   # Python 版主逻辑
│   ├── ZMeshMend_ZScript.txt          # ZScript 版主逻辑
│   └── ZMeshMend_config.txt           # 共享配置
├── ZMeshMendData/
│   ├── CMakeLists.txt                 # C++ 构建配置
│   ├── build.bat                      # 一键编译（旧方式）
│   ├── run_build.ps1                  # 一键编译（推荐）
│   ├── zmeshmend_core.cpp             # CGAL 孔洞填充引擎
│   ├── zmeshmend_core.exe             # 编译产物
│   ├── ZFileUtils64.dll               # ZScript 文件工具 DLL
│   ├── ZMeshMend_pipeline.py          # Python 管线辅助
│   ├── GoZ_Mesh.cpp / .h              # GoZ 网格读写
│   ├── GoZ_Utils.cpp / .h             # GoZ 工具函数
│   ├── GoZ_Binary.h / GoZ_Config.h    # GoZ 格式定义
│   └── *.dll                          # CGAL 运行时依赖
└── Release/
    ├── Python_v1.0.0/                 # Python 版发布包
    └── ZScript_v1.0.0/                # ZScript 版发布包
```

## 依赖

- **Python 版:** ZBrush 2026+ Python API（`zbrush` 模块）
- **ZScript 版:** ZFileUtils64.dll（内置）
- **C++ 核心:** CGAL 5.x, Boost 1.74+, Eigen3, GMP, MPFR

## 许可

- **GoZ SDK 文件**（`GoZ_Mesh.*`, `GoZ_Utils.*`, `GoZ_Binary.h`, `GoZ_Config.h`）：版权归 Maxon/Pixologic 所有
- **其余代码**：MIT License
