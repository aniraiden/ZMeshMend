# ZMeshMend

ZBrush 网格孔洞自动修复插件。一键闭合所有开放孔洞，支持智能曲率感知填充、碎片移除和遮罩驱动清理。非常适合AI生成的三维模型的清理工作。

## 功能

| 功能 | 说明 |
|------|------|
| **关闭所有孔洞** | 使用 ZBrush 内置算法快速闭合所有开放边界 |
| **MendHoles + PolyGroup** | CGAL 算法智能填充，曲率感知细化，自动创建 PolyGroup |
| **遮罩清理流程** | 遮罩 → 删除 → 智能填充 → 分组，全自动流程 |
| **移除小碎片** | 基于连通性分析自动清理孤立网格碎片 |
| **导出/导入 OBJ** | 支持导出网格到外部处理，处理完成后导回 |

## 安装

1. 将整个 `ZMeshMend` 文件夹复制到 ZBrush 插件目录：
   ```
   C:\Program Files\Pixologic\ZBrush 20XX\ZStartup\ZPlugs64\
   ```

2. 在 ZBrush 中加载：
   - 菜单：`ZScript` → `Python Scripting` → `Load`
   - 选择 `ZMeshMend_Launcher.py`

3. 插件面板将自动出现在 ZBrush UI 中。

### 可选：启用 CGAL 高级填充

如需使用曲率感知智能填充（推荐），需编译 CGAL 核心引擎：

**前置条件：**
- Visual Studio 2019/2022（含"Desktop C++"工作负载）
- CMake 3.16+
- [vcpkg](https://github.com/microsoft/vcpkg) + CGAL 库

**编译步骤：**
```bash
# 1. 安装 CGAL（一次性）
cd C:\path\to\vcpkg
.\vcpkg install cgal:x64-windows

# 2. 编译核心引擎
cd ZMeshMendData
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release

# 3. 复制产物
copy build\Release\zmeshmend_core.exe ..
```

> 如果未检测到 `zmeshmend_core.exe`，插件会自动回退到 ZBrush 内置算法。

## 配置

编辑 `ZMeshMend\ZMeshMend_config.txt` 或在插件面板中调整：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `removeSmallFragments` | 1 | 是否自动移除小碎片 |
| `fragmentMinFraction` | 0.01 | 碎片保留的最小面数占比 |
| `fragmentMinFaces` | 50 | 碎片保留的绝对最小面数 |
| `maskGrowRings` | 1 | 遮罩扩展环数 |
| `maskSharpenPasses` | 1 | 遮罩锐化次数 |

## 项目结构

```
ZMeshMend/
├── README.md
├── ZMeshMend_Launcher.py          # 入口：ZBrush 中加载此文件
├── ZMeshMend/
│   ├── __init__.py
│   ├── init.py                    # 自动加载初始化
│   ├── ZMeshMend.py               # 主插件逻辑
│   └── ZMeshMend_config.txt       # 配置文件
└── ZMeshMendData/
    ├── CMakeLists.txt             # C++ 构建配置
    ├── build.bat                  # 一键编译脚本
    ├── zmeshmend_core.cpp         # CGAL 孔洞填充引擎
    ├── GoZ_Mesh.cpp / .h          # GoZ 网格读写
    ├── GoZ_Utils.cpp / .h         # GoZ 工具函数
    ├── GoZ_Binary.h               # GoZ 二进制格式定义
    ├── GoZ_Config.h               # 平台配置
    └── zmeshmend_core.exe         # 编译产物（CGAL 引擎）
```

## 使用说明

### 关闭所有孔洞
点击 `Close All Holes`，一键闭合当前网格的所有开放孔洞。

### 智能填充 + PolyGroup
点击 `MendHoles + PolyGroup`。CGAL 引擎通过球体拟合感知曲率，填充面自动分配到 `ZMeshMend_Fill` PolyGroup。完成后可用 `Ctrl+Shift+Click` 按 PolyGroup 遮罩填充区域。

### 遮罩清理流程
1. 在网格上绘制遮罩标记需删除区域
2. 点击`Mask-Based Cleanup`
3. 插件自动执行：锐化遮罩 → 扩展遮罩 → 删除遮罩面 → 智能填充孔洞 → 创建 PolyGroup

### 移除小碎片
点击`Remove Small Fragments`，基于连通性分析移除孤立碎片。

## 依赖

- **Python:** ZBrush Python API（`zbrush` 模块）
- **C++（可选）:** CGAL 5.x, Boost, Eigen3

## 许可

- **GoZ SDK 文件**（`GoZ_Mesh.*`, `GoZ_Utils.*`, `GoZ_Binary.h`, `GoZ_Config.h`）：版权归 Pixologic Inc. 所有，仅限 ZBrush 数据交换用途。
- **其余代码**：MIT License
