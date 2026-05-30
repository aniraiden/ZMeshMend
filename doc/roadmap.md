# 开发路线图

> 当前版本：v1.2.0 | 最后更新：2026-05-31

## 已完成功能

- [x] CGAL 曲率感知孔洞填充（`triangulate_refine_and_fair_hole`）
- [x] 连通分量分析与小碎片自动清理
- [x] 填充面自动 PolyGroup 标记
- [x] 遮罩驱动模型清理（遮罩 → 删除 → 智能填充）
- [x] ZBrush 内置 Close Holes 快速兜底
- [x] Python 版插件（ZBrush 2026+）
- [x] ZScript 版插件（ZBrush 2021+）
- [x] 平滑开放边缘（smooth_open_borders）
- [x] **全局布线放松（Wireframe Relax）** — oaRelaxVerts 算法
- [x] **GoZ binary 中转方案**（双向：Export/Import 含 mask/uv/groups/mrgb）
  - [x] ZScript 端：`[FileNameSetNext, .goz]` + `Tool:Export/Import` + `LaunchAppWithFile`
  - [x] Python 端：`zbc.set_next_filename(.goz)` + `subprocess.run`（v1.2.0 全面切换）
- [x] **补洞输出保留 quad 拓扑** — build_output_goz 复用原 face4
- [x] **SubDiv 检测阻断** — SDiv>1 时拒绝操作
- [x] **GoZ writeMesh buffer 对齐修复** — m_uvs/m_crease/m_mask/m_groups 自动 resize 到新 face/vertex 数
- [x] **GoZ 管线完全指南** → [doc/goz-pipeline-guide.md](goz-pipeline-guide.md) + `.trae/skills/zbrush-goz-pipeline/`
- [x] **Python SubDiv 检测** — `_ensure_edit_mode()` 加 SDiv>1 阻断，与 ZScript 一致

### 核心架构

```
ZBrush ──Tool:Export(.goz)──→ zmeshmend_core.exe ──Tool:Import(.goz)──→ ZBrush
          含 MASK16_LIST                CGAL PMP 处理              含 PolyGroups
          含 GROUPS_LIST                                          含 Mask
```

- Python 端：`zbc.set_next_filename(.goz)` + `zbc.press("Tool:Export/Import")` + `subprocess.run`
- ZScript 端：`[FileNameSetNext, .goz]` + `[IPress,Tool:Export/Import]` + `ZFileUtils LaunchAppWithFile`
- 统一 GoZ binary 格式（magic `GoZb`），ZBrush 凭后缀 `.goz` 自动识别

### 放松算法关键设计

- 算法：Laplacian 平滑 → AABB tree snap 回原表面（参考 Maya `oaRelaxVerts`）
- 边界顶点固定 120 个，内部顶点放松（Jacobi 迭代 + OpenMP `schedule(dynamic,256)`）
- edge_neighbors 参数：从 GoZ 原始 face4 边邻居（不含 quad 对角线），CGAL halfedge 回退

## v1.2.0 关键修复

| 文件 | 问题 | 修复 |
|------|------|------|
| `ZBrush` 行为 | SDiv>1 时 GoZ 导入重新细分导致 Y 轴偏移 | Python/ZScript 双端加 SDiv>1 阻断 |
| `zmeshmend_core.cpp` | build_output_goz 追加面后 m_uvs/m_crease 未对齐 → writeMesh 失败 | 自动 resize 到新 faceCount |
| `ZMeshMend.py` | `_import_goz` 面数不变时假阴性弹窗（smooth/relax） | 去掉面数变化判断 |
| `GoZ_Mesh.cpp` | 5 处 `vector<char>(count)` 无负数防护 | `if (count)` → `if (count > 0)` |
| `ZMeshMend_ZScript.txt` | HDivider 不存在、Note icon 无效等 | SDiv、图标值修正 |

## 后续规划

### 高优先级

- [ ] **补洞区域四边形化** — `topology` 分支
  - 局部三角对贪婪合并（`CGAL::Euler::join_face`）

### 中优先级

- [ ] **补洞质量评估** — 平整度/自交/非流形检测
- [ ] **边界保形填充** — 避免 fairing 过度导致边界收缩

### 低优先级

- [ ] **多孔洞批量并行处理**
- [ ] **ZScript 配置跨会话持久化** — LoadConfig 当前不解析 key=value
