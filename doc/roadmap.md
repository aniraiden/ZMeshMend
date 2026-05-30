# 开发路线图

> 当前版本：v1.1.0（smooth-hole-edges 分支）
> 最后更新：2026-05-30

## 已有功能

- [x] CGAL 曲率感知孔洞填充（`triangulate_refine_and_fair_hole`）
- [x] 连通分量分析与小碎片自动清理
- [x] 填充面自动 PolyGroup 标记（与原始面区分）
- [x] 遮罩驱动模型清理（遮罩 → 删除 → 智能填充）
- [x] ZBrush 内置 Close Holes 快速兜底
- [x] Python 版插件（ZBrush 2026+）
- [x] ZScript 版插件（ZBrush 2021+）
- [x] GoZ AppLink 支持
- [x] OBJ full 输出保留 PolyGroup
- [x] 配置文件统一管理参数
- [x] **平滑开放边缘** — `smooth-hole-edges` 分支


## 后续规划

### 高优先级

- [ ] **全局布线放松（Wireframe Relax）** — `relax-wireframe` 分支 🔨
  - 对整个模型使用 `CGAL::Polygon_mesh_processing::smooth_shape()`
  - 边界顶点固定保护（通过 `vertex_is_constrained_map` 约束开放边界顶点）
  - 纯切线方向位移，保持体积和细节
  - 设置滑块 `Relax Iterations`（1-20，默认 3）控制迭代次数
  - 导出 OBJ → CGAL --relax-wireframe → 导入 OBJ
  - 不改变拓扑，顶点/面数不变，PolyGroup 完全保留

- [ ] **GoZ 输出支持四边面** — `topology` 分支依赖
  - 当前 `build_output_goz` 只写 3 顶点
  - 需支持 `GoZ_TAG_FACE4_LIST` 或混合三角/四边

### 中优先级

- [ ] **补洞区域四边形化** — `topology` 分支
  - 第一版：局部三角对贪婪合并（`CGAL::Euler::join_face`）
  - 第二版：边界四边流向引导评分
  - 第三版（远期）：方向场全局 Quad Remeshing

- [ ] **补洞质量评估**
  - 填充面平整度检测
  - 自交检测
  - 非流形检测

- [ ] **边界保形填充**
  - 保持原始边界边的几何特征
  - 避免 fairing 过度导致边界收缩

### 低优先级

- [ ] **多孔洞批量处理优化**
  - 并行补洞（多线程）
  - 进度条细化

- [ ] **增量补洞**
  - 只处理新产生的孔洞
  - 缓存已处理区域

- [ ] **UI 改进**
  - 实时预览
  - 撤销支持

## 技术债务

- [ ] GoZ 写回路径仅支持三角面（需支持四边形）
- [ ] Python fallback `_fill_hole_smart` 与 CGAL 结果一致性待验证
- [ ] ZScript 路径的 stitch_borders 可能导致原始四边形被破坏

---

## 实现计划：全局布线放松（Wireframe Relax）

> 分支：`relax-wireframe` | 状态：🔨 规划中

### 设计原则

- **最小侵入**：复用现有导出-处理-导入管线，不重构架构
- **边界保护**：开放边界顶点固定不动（`PMP::smooth_shape` 的 `vertex_is_constrained_map` 机制）
- **体积保持**：只用 `smooth_shape()` 沿切平面移动顶点，不涉及法线分量
- **纯位置操作**：不增删顶点/面，PolyGroup 完全保留

### 修改文件清单

#### 1. `ZMeshMendData/zmeshmend_core.cpp` — C++ 核心

**新增头文件**：
```cpp
#include <CGAL/Polygon_mesh_processing/smooth_shape.h>
```

**新增函数 `relax_wireframe()`**（约 40 行）：
- 收集所有开放边界顶点 → 构建 `vertex_is_constrained_map`
- 调用 `PMP::smooth_shape(mesh, params)`，传入边界约束和时间步长
- 输出统计日志

**新增 CLI 选项**：
- `--relax-wireframe`：触发全模型布线放松
- `--relax-iterations N`：迭代次数（默认 3）

**新增配置解析**（zero-arg 模式）：
- `relaxWireframe=1` → 触发放松
- `relaxIterations=N` → 迭代次数

**新增处理分支**（`main()` 中）：
- 类似 `opt_smooth_only` 分支：读入网格 → `relax_wireframe()` → 写回 OBJ

#### 2. `ZMeshMend/ZMeshMend.py` — Python 插件

**新增配置项**（`CONFIG` dict）：
```python
"relaxWireframe": False,
"relaxIterations": 3,
```

**新增配置注释**（`_config_comment_map`）：
```python
"relaxWireframe": "全局布线放松模式（1=放松, 0=正常补洞）",
"relaxIterations": "全局布线放松迭代次数（1-20）",
```

**新增 UI 按钮**（`build_ui()` 中，放在 "Smooth Open Edges" 后面）：
```python
zbc.add_button(
    _ui_path("Close Holes:Relax Wireframe"),
    "使用 CGAL smooth_shape 对全模型布线进行切线方向放松，"
    "保持体积和细节，边界顶点不动。",
    _on_relax_wireframe_click,
    width=1.0,
)
```

**新增 UI 滑块**（Settings 面板，放在 "Smooth Rings" 后面）：
```python
zbc.add_slider(
    _ui_path("Settings:Relax Iterations"),
    float(CONFIG["relaxIterations"]),
    1,
    1.0,
    20.0,
    "全局布线放松迭代次数（1-20）。",
    _on_config_change,
    width=1.0,
)
```

**新增回调函数**：
- `_on_relax_wireframe_click(sender="")` → `_freeze_op(lambda: do_relax_wireframe(sender))`
- `do_relax_wireframe(sender="")` — 核心流程：导出 OBJ → 调用 CGAL → 导入 OBJ
- `_call_cgal_relax_wireframe(input_obj, output_obj)` — 调用 EXE 的命令行包装

**更新 `_on_config_change()`**：
- 添加 `elif name == "Relax Iterations":` 分支

#### 3. `ZMeshMend/ZMeshMend_ZScript.txt` — ZScript 版插件

**新增全局变量**：
```zscript
[VarDef, gRelaxIter]
```

**新增 UI 按钮**（`InitUI` 中）：
```zscript
IButton, , ZMeshMend:Close Holes:Relax Wireframe, ...
```

**新增 UI 滑块**（`InitUI` 中）：
```zscript
ISlider, ZMeshMend:Settings:Relax Iterations, ...
```

**新增配置保存/加载**：
- `LoadConfig` 中添加 `gRelaxIter` 读取
- `SaveConfig` 中添加 `relaxIterations` 写入

**新增回调**：
- 调用 `zmeshmend_core.exe`（zero-arg 模式），从配置读取 `relaxWireframe=1`

#### 4. `ZMeshMend/ZMeshMend_config.txt` — 配置文件

**新增配置项**：
```
# 全局布线放松迭代次数（1-20）
relaxIterations=3
```

### 关键决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| CGAL 函数 | `PMP::smooth_shape()` | 专为保持体积的切线方向平滑设计 |
| 边界保护方式 | `vertex_is_constrained_map` | CGAL 内置约束机制，比手动投影更简洁可靠 |
| 是否影响拓扑 | 否 | 纯顶点位置操作，不翻转边、不增删面 |
| 是否需要新滑块 | 是，`Relax Iterations` | 与现有 `Smooth Iterations` 语义不同（全局 vs 边界），独立控制 |
| 是否需要 `relaxWireframe` 配置标志 | 仅在 ZScript zero-arg 模式需要 | Python 模式通过 CLI `--relax-wireframe` 直接触达 |
