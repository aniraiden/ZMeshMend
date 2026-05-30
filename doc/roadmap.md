# 开发路线图

> 当前版本：v1.2.0 | 最后更新：2026-05-30

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
- [x] **补洞输出保留 quad 拓扑** — build_output_goz 复用原 face4
- [x] **SubDiv 检测阻断** — SDiv>1 时拒绝操作

### 核心架构

```
ZBrush ──Tool:Export(.goz)──→ zmeshmend_core.exe ──Tool:Import(.goz)──→ ZBrush
          含 MASK16_LIST                CGAL PMP 处理              含 PolyGroups
```

- Python 端：`zbc.set_next_filename` + `zbc.press("Tool:Export/Import")` + `subprocess.run`
- ZScript 端：`[FileNameSetNext]` + `[IPress,Tool:Export/Import]` + `ZFileUtils LaunchAppWithFile`
- 统一 GoZ binary 格式（magic `GoZb`），ZBrush 凭后缀 `.goz` 自动识别

### 放松算法关键设计

- 算法：Laplacian 平滑 → AABB tree snap 回原表面（参考 Maya `oaRelaxVerts`）
- 边界顶点固定 120 个，内部顶点放松（Jacobi 迭代 + OpenMP `schedule(dynamic,256)`）
- edge_neighbors 参数：从 GoZ 原始 face4 边邻居（不含 quad 对角线），CGAL halfedge 回退

## 代码审查修复（2026-05-30）

| 文件 | 问题 | 修复 |
|------|------|------|
| `ZMeshMend.py` L190 | `_call_cgal_fill` 异常返回缺元组元素 | `return False` → `return False, -1` |
| `ZMeshMend.py` L253 | `_call_cgal_relax_wireframe` 多返元素导致 bool 反转 | `return False, -1` → `return False` |
| `ZMeshMend.py` L16 | `__version__` 写 `1.1.0` 实际 `1.2.0` | 统一为 `1.2.0` |
| `ZMeshMend.py` | 14 个死函数约 300 行 | 全部删除 |
| `GoZ_Utils.cpp` | `ftell` 返回值未检查（损坏文件 → `new char[-1]` 崩溃） | 加 `if (fileSize <= 0)` 守卫 |
| `zmeshmend_core.cpp` L501 | `load_goz_to_cgal` FACE4 stride 无防御 | 加 `vertexIndices.size() < faceCount*4` 检查 |
| `zmeshmend_core.cpp` L764 | `write_fill_only_goz` stride 3/4 不匹配 | `src_fi*3` → `src_fi*4` |
| `GoZ_Mesh.cpp` | 5 处 `vector<char>(count)` 无负数防护 | `if (count)` → `if (count > 0)` |
| `zmeshmend_core.cpp` L361 | `Mesh ref_mesh` 大模型栈溢出 | `unique_ptr<Mesh>` 堆分配 |
| `ZMeshMend_ZScript.txt` L232 | `[Note]` icon=4 无效值 | 改为 2（错误图标） |
| `ZMeshMend_ZScript.txt` | `tmpPath`/`sdivMax` 从未 `[VarDef]` | 添加声明 |
| `ZMeshMend_ZScript.txt` | `SaveConfig` 三处 FileRename 返回值未检查 | 加 `[If,err!=0,[Note...]]` |
| `ZMeshMend_ZScript.txt` | `LaunchAppWithFile` err 未检查 | 加错误提示 + `[Exit]` |
| `ZMeshMend_ZScript.txt` | 错误消息写"OBJ"实际".goz" | 改为 "GoZ not created" |

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
