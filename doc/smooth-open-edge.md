# Smooth Open Edge — 功能规划

## 目标

一键平滑所有开放边界环（Open Boundary Edge Loop），使用 Chaikin 曲线细分算法对边界顶点做平滑，再投影回原表面附近，使洞口边缘更流畅自然。

## 约束

- **仅作用于开放边界**：不触碰封闭网格内部顶点
- **不破坏 PolyGroup**：不修改任何面的 PolyGroup 归属
- **不增删顶点/面**：只移动边界顶点位置，拓扑不变
- **轻量级**：在现有插件基础上添加一个按钮，不改动核心补洞流程

## 实现步骤

### Step 1 — 找到 Open Boundary Edge Loop

使用 CGAL 现成的边界检测 API：

```cpp
std::vector<Mesh::Halfedge_index> border_halfedges;
PMP::extract_boundary_cycles(mesh, std::back_inserter(border_halfedges));
```

- 去重，每个边界环只保留一个 seed halfedge
- 沿 `mesh.next(h)` 遍历整个环，收集该环的所有顶点

相关已有代码：[extract_boundary_cycles 调用](file:///h:/vibe_coding/ZMeshMend/ZMeshMend/ZMeshMendData/zmeshmend_core.cpp#L682-L707)

### Step 2 — 提取 Boundary Vertices

对每个边界环：

```
for each border halfedge h in loop:
    v = target(h, mesh)   // 或 source(h, mesh)
    收集顶点位置 Point(x, y, z)
```

得到每个边界环的有序顶点位置序列 `std::vector<Point>`。

### Step 3 — Chaikin 曲线平滑

Chaikin 算法是二次 B-spline 的 corner-cutting 细分，对闭合多边形环迭代平滑：

```
对于闭合环 {P0, P1, ..., Pn-1}，一次迭代：
  对每个 i = 0..n-1：
    Q_2i   = 0.75 * Pi + 0.25 * P_{i+1}
    Q_2i+1 = 0.25 * Pi + 0.75 * P_{i+1}
  输出 {Q0, Q1, Q2, Q3, ..., Q_{2n-1}}
```

**本方案采用变体**：不细分出新顶点，而是把 Chaikin 的中间位置作为原顶点的平滑目标。

```
对闭合环的每个顶点 Vi：
  取相邻两个顶点 V_{i-1}, V_{i+1}
  平滑后位置 = 0.25 * V_{i-1} + 0.50 * Vi + 0.25 * V_{i+1}
```

- 迭代次数：默认 2~3 次（可配置）
- 对开放边界环（非闭合）首尾顶点保持不动

### Step 4 — 向内扩展多圈 + 位移逐圈衰减

单圈平滑在高面数边界上几乎看不出变化。将平滑扩展到边界向内 N 圈顶点，位移权重逐圈衰减，形成自然的 smooth falloff 效果。

**收集顶点环：**

```
Ring 0 = 边界环上的所有顶点
Ring 1 = 与 Ring 0 相邻但不属于 Ring 0 的顶点
Ring 2 = 与 Ring 1 相邻但不属于 Ring 0/1 的顶点
...
Ring N = 与 Ring N-1 相邻但不属于前面所有环的顶点
```

- 默认 3 圈（可配置 `--smooth-rings N`）
- 每圈顶点的平滑方式不同：
  - Ring 0：Chaikin 平滑（沿边界环方向）
  - Ring 1~N：Laplacian 平滑（邻域平均，跟随边界位移自然过渡）

**位移权重衰减：**

```
对第 i 圈的每个顶点：
  weight = pow(0.5, i)    // Ring 0=1.0, Ring 1=0.5, Ring 2=0.25, Ring 3=0.125
  displacement = (smoothed_pos - original_pos) * weight
  clamped_pos = original_pos + displacement
```

效果类比：类似 smooth 笔刷从边界向内逐渐减轻力度。

### Step 5 — 法线切平面投影

平滑后的顶点可能偏离原表面（向内收缩或向外漂移），导致洞口体积变化。投影保证顶点始终贴在原表面上：

```
对每个顶点（所有环）：
  original_pos = 原始位置
  clamped_pos  = 加权位移后的位置
  normal       = 原始位置处邻接三角面的平均法线

  // 将 clamped_pos 沿法线投影回 original_pos 所在的切平面
  offset = clamped_pos - original_pos
  projected_pos = clamped_pos - dot(offset, normal) * normal
```

- 只允许在切平面方向移动（沿表面滑动），不允许法线方向偏移
- 保持洞口体积不变

**投影不会导致重叠面**：
- 投影方向是法线方向（垂直于表面），不是切线方向 → 不会把顶点推过邻接面
- 投影幅度极小：平滑产生的位移主要在切平面内，法线分量本身就很小
- 多圈 falloff 让位移过渡平滑，避免局部突变导致翻面

## CGAL 核心修改

### 新增入口参数

在 `zmeshmend_core.cpp` 的 `main()` 参数解析中增加：

```
--smooth-border         启用边界平滑
--smooth-iterations N   平滑迭代次数（默认 2）
--smooth-rings N        向内扩展圈数（默认 3）
```

### 新增函数

```cpp
// 提取所有边界环的顶点序列
std::vector<std::vector<Mesh::Vertex_index>> extract_border_loops(const Mesh& mesh);

// 从边界环向内扩展 N 圈顶点
// 返回 vector<ring>，ring[0] = 边界顶点，ring[1] = 第一圈内部顶点...
std::vector<std::vector<Mesh::Vertex_index>> expand_rings(
    const Mesh& mesh,
    const std::vector<Mesh::Vertex_index>& border_loop,
    int num_rings
);

// Chaikin 平滑边界环（Ring 0 专用）
std::vector<Point> chaikin_smooth_loop(
    const std::vector<Point>& points,
    int iterations,
    bool closed
);

// Laplacian 平滑内部环（Ring 1~N 用）
std::vector<Point> laplacian_smooth_ring(
    const Mesh& mesh,
    const std::vector<Mesh::Vertex_index>& ring_vertices,
    int iterations
);

// 沿切平面投影回原表面
Point project_to_tangent_plane(
    const Point& displaced,
    const Point& original,
    const Vector& normal
);

// 主入口：平滑网格所有开放边界及其邻域
void smooth_open_borders(Mesh& mesh, int iterations, int num_rings);
```

### 处理流程

```
1. extract_boundary_cycles → border loops
2. 对每个 loop：
   a. expand_rings → Ring 0 (边界), Ring 1, Ring 2, Ring 3
   b. Chaikin 平滑 Ring 0，Laplacian 平滑 Ring 1~N
   c. 计算每个顶点的邻接面平均法线
   d. 逐圈加权：weight = 0.5^ring_index
      displaced = original + (smoothed - original) * weight
   e. 法线切平面投影（消除法线方向偏移，保持体积）
   f. 更新 mesh 中的顶点位置
3. 写回（GoZ / OBJ），PolyGroup 原样保留
```

## 插件层修改

### Python 版 (`ZMeshMend.py`)

新增按钮和回调：

```python
def do_smooth_open_edges(self):
    """Smooth Open Edge — Chaikin 平滑边界环"""
    # 1. Export OBJ
    # 2. 调用 zmeshmend_core.exe --smooth-border --smooth-iterations 2
    #    输入: zmeshmend_export.obj → 输出: zmeshmend_import.obj
    # 3. Import OBJ 回 ZBrush
```

### ZScript 版 (`ZMeshMend_ZScript.txt`)

新增按钮：

```zscript
[ISubPalette, "Smooth Open Edge"]
[Button, "Smooth Open Edge",
    "对开放边界环做 Chaikin 平滑，使洞口边缘更流畅",
    // Tool:Export → 写配置文件 → LaunchAppWithFile → Tool:Import
]
```

### 配置文件 (`ZMeshMend_config.txt`)

新增配置项：

```
smoothBorder=1
smoothIterations=2
smoothRings=3
```

## 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 边界环平滑 | Chaikin corner-cutting | 简单、稳定、不引入振荡 |
| 内部环平滑 | Laplacian | 邻域平均，自然跟随边界位移 |
| 扩展圈数 | 默认 3，可配置 1~5 | 3 圈 + falloff 消除可见边界，更多圈代价增大 |
| 权重衰减 | w = 0.5^ring | 指数衰减，过渡自然 |
| 投影方式 | 法线切平面投影 | 保持体积，不破坏几何 |
| 拓扑 | 只移动顶点，不增删 | 保证 PolyGroup 和面索引完全不变 |
| 范围 | 所有开放边界环 + N 圈邻域 | 一键处理所有洞口边缘 |

## 文件变更清单

| 文件 | 变更类型 | 内容 |
|------|----------|------|
| `ZMeshMendData/zmeshmend_core.cpp` | 修改 | 新增 `--smooth-border` 参数和 `smooth_open_borders()` 函数 |
| `ZMeshMendData/CMakeLists.txt` | 不变 | 不需要新的 CGAL 头文件 |
| `ZMeshMend/ZMeshMend.py` | 修改 | 新增 `do_smooth_open_edges()` 按钮回调 |
| `ZMeshMend/ZMeshMend_ZScript.txt` | 修改 | 新增 Smooth Open Edge 按钮 |
| `ZMeshMend/ZMeshMend_config.txt` | 修改 | 新增 `smoothBorder`、`smoothIterations` 配置 |
| `ZMeshMendData/ZMeshMend_pipeline.py` | 不变 | 不需要改变 |
