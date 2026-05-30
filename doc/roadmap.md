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
  - 多圈 Chaikin + Laplacian 混合平滑
  - 指数衰减权重（`0.5^ring`）
  - 法线切平面投影保持体积
  - 保留原始 PolyGroup 和拓扑

## 后续规划

### 高优先级

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
