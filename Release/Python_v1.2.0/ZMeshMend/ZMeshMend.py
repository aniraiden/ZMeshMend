"""ZMeshMend - ZBrush Python 插件
=============================

自动闭合当前网格上的所有开放孔洞。
基于原 ZMeshMend 插件用 ZBrush Python API 重写。

核心功能：
  1. 自动闭合所有孔洞
  2. 移除小型分离碎片 / 网格碎屑
  3. 为填充区域创建新 PolyGroup
  4. 自动遮罩新闭合的区域
  5. 支持基于 ZBrush 遮罩的网格清理
"""

__author__ = "ZMeshMend Rebuild"
__version__ = "1.2.0"

import os
import sys
import tempfile
import struct
import math
import subprocess
from zbrush import commands as zbc

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

_CGAL_DATA_DIR = os.path.join(os.path.dirname(_SCRIPT_DIR), "ZMeshMendData")
_CGAL_EXE_PATH = os.path.join(_CGAL_DATA_DIR, "zmeshmend_core.exe")
_CGAL_EXE_REL = os.path.normpath(os.path.join("..", "ZMeshMendData", "zmeshmend_core.exe"))

PALETTE_NAME = "ZMeshMend"
SUBPALETTE_MAIN = f"{PALETTE_NAME}:Close Holes"
SUBPALETTE_CONF = f"{PALETTE_NAME}:Settings"
SUBPALETTE_INFO = f"{PALETTE_NAME}:Info"

CONFIG_PATH = os.path.join(_SCRIPT_DIR, "ZMeshMend_config.txt")

_cgal_available_cache = None

CONFIG = {
    "removeSmallFragments": True,
    "fragmentMinFraction": 0.01,
    "fragmentMinFaces": 50,
    "maskGrowRings": 1,
    "maskSharpenPasses": 1,
    "smoothBorder": False,
    "smoothIterations": 2,
    "smoothRings": 3,
    "relaxIterations": 3,
    "relaxFactor": 1.0,
}

_config_comment_map = {
    "removeSmallFragments": "移除小型分离碎片（1=是, 0=否）",
    "fragmentMinFraction": "保留碎片所需的最小面数占比",
    "fragmentMinFaces": "保留碎片所需的最小绝对面数",
    "maskGrowRings": "删除前扩展遮罩的环数",
    "maskSharpenPasses": "扩展前锐化遮罩的遍数",
    "smoothBorder": "平滑边界模式（1=仅平滑, 0=正常补洞）",
    "smoothIterations": "边界平滑迭代次数（1-20）",
    "smoothRings": "边界平滑向内扩展圈数（1-20）",
    "relaxIterations": "全局布线放松迭代次数（1-20）",
    "relaxFactor": "全局布线放松阻尼因子（0.1-1.0，越小越保守）",
}


def load_config():
    """从 ZMeshMend_config.txt 加载配置"""
    global CONFIG
    if not os.path.exists(CONFIG_PATH):
        save_config()
        return
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    key, val = line.split("=", 1)
                    key = key.strip()
                    val = val.strip()
                    if key in CONFIG:
                        if isinstance(CONFIG[key], bool):
                            CONFIG[key] = val == "1"
                        elif isinstance(CONFIG[key], int):
                            CONFIG[key] = int(val)
                        elif isinstance(CONFIG[key], float):
                            CONFIG[key] = float(val)
    except Exception as e:
        _log(f"警告：load_config 失败：{e}")


def save_config():
    """保存当前配置到 ZMeshMend_config.txt"""
    try:
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            f.write("# ZMeshMend 配置文件\n")
            f.write("# 按需修改参数。\n\n")
            for key, default in CONFIG.items():
                comment = _config_comment_map.get(key, "")
                if comment:
                    f.write(f"# {comment}\n")
                if isinstance(default, bool):
                    f.write(f"{key}={1 if CONFIG[key] else 0}\n")
                elif isinstance(default, int):
                    f.write(f"{key}={CONFIG[key]}\n")
                elif isinstance(default, float):
                    f.write(f"{key}={CONFIG[key]}\n")
                f.write("\n")
    except Exception as e:
        _log(f"警告：save_config 失败：{e}")


def _cgal_available():
    """检查 CGAL 核心可执行文件是否存在"""
    global _cgal_available_cache
    if _cgal_available_cache is None:
        _cgal_available_cache = os.path.exists(_CGAL_EXE_PATH)
    return _cgal_available_cache


def _call_cgal_fill(input_obj, output_goz, fill_goz=None, debug_obj=None):
    """调用 CGAL 核心 EXE 填充孔洞。输出为带 PolyGroup 的 GoZ 格式。

    输入可为 OBJ 或 GoZ。输出始终为带 PolyGroup 的 GoZ。
    如提供 debug_obj，将同时写入一个验证用 OBJ。

    返回 (成功, faces_added)。faces_added 为 -1 表示无法解析。
    """
    args = [_CGAL_EXE_PATH, input_obj, output_goz]
    if fill_goz:
        args.append(fill_goz)
    else:
        args.append("")
    if debug_obj:
        args.append(debug_obj)
    faces_added = -1

    try:
        p = subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=300,
            cwd=_CGAL_DATA_DIR,
        )

        stdout = p.stdout
        stderr = p.stderr

        if stderr:
            for line in stderr.strip().split("\n"):
                if line.strip():
                    _log(f"  [CGAL] {line.strip()}")

        for line in stdout.strip().split("\n"):
            line = line.strip()
            if not line:
                continue
            _log(f"  [CGAL] {line}")
            if line.startswith("SUMMARY:"):
                parts = line.split()
                for part in parts:
                    if part.startswith("faces_added="):
                        try:
                            faces_added = int(part.split("=")[1])
                        except ValueError:
                            pass

        if p.returncode != 0:
            _log(f"  [CGAL] EXE 返回错误码 {p.returncode}")
            return False, faces_added

        if not os.path.exists(output_goz):
            _log(f"  [CGAL] 输出文件未创建")
            return False, faces_added

        return True, faces_added

    except FileNotFoundError:
        _log(f"  [CGAL] 未找到 EXE：{_CGAL_EXE_PATH}")
        return False, -1
    except subprocess.TimeoutExpired:
        _log(f"  [CGAL] EXE 超时（300 秒）")
        return False, -1
    except Exception as e:
        _log(f"  [CGAL] 错误：{e}")
        return False, -1


def _call_cgal_relax_wireframe(input_obj, output_obj):
    """调用 CGAL 核心 EXE 做布线放松。

    输入 GoZ binary（含 MASK16_LIST）时，core 自动从 mask 构造 vertex_allow，
    仅放松遮罩区顶点。输入 OBJ 文本时，core 退化为全模型放松。

    返回 True 表示成功。
    """
    iterations = CONFIG["relaxIterations"]
    factor = CONFIG["relaxFactor"]
    args = [
        _CGAL_EXE_PATH,
        input_obj,
        output_obj,
        "--relax-wireframe",
        "--relax-iterations", str(iterations),
        "--relax-factor", str(factor),
    ]

    try:
        p = subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=300,
            cwd=_CGAL_DATA_DIR,
        )

        stdout = p.stdout
        stderr = p.stderr

        if stderr:
            for line in stderr.strip().split("\n"):
                if line.strip():
                    _log(f"  [CGAL] {line.strip()}")

        for line in stdout.strip().split("\n"):
            line = line.strip()
            if not line:
                continue
            _log(f"  [CGAL] {line}")

        if p.returncode != 0:
            _log(f"  [CGAL] EXE 返回错误码 {p.returncode}")
            return False

        if not os.path.exists(output_obj):
            _log(f"  [CGAL] 输出文件未创建")
            return False

        return True

    except FileNotFoundError:
        _log(f"  [CGAL] 未找到 EXE：{_CGAL_EXE_PATH}")
        return False
    except subprocess.TimeoutExpired:
        _log(f"  [CGAL] EXE 超时（300 秒）")
        return False
    except Exception as e:
        _log(f"  [CGAL] 错误：{e}")
        return False


def _call_cgal_smooth_border(input_obj, output_obj):
    """调用 CGAL 核心 EXE 平滑开放边界环。

    返回 True 表示成功。
    """
    iterations = CONFIG["smoothIterations"]
    rings = CONFIG["smoothRings"]
    args = [
        _CGAL_EXE_PATH,
        input_obj,
        output_obj,
        "--smooth-border",
        "--smooth-iterations", str(iterations),
        "--smooth-rings", str(rings),
        "--smooth-only",
    ]

    try:
        p = subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=300,
            cwd=_CGAL_DATA_DIR,
        )

        stdout = p.stdout
        stderr = p.stderr

        if stderr:
            for line in stderr.strip().split("\n"):
                if line.strip():
                    _log(f"  [CGAL] {line.strip()}")

        for line in stdout.strip().split("\n"):
            line = line.strip()
            if not line:
                continue
            _log(f"  [CGAL] {line}")

        if p.returncode != 0:
            _log(f"  [CGAL] EXE 返回错误码 {p.returncode}")
            return False

        if not os.path.exists(output_obj):
            _log(f"  [CGAL] 输出文件未创建")
            return False

        return True

    except FileNotFoundError:
        _log(f"  [CGAL] 未找到 EXE：{_CGAL_EXE_PATH}")
        return False
    except subprocess.TimeoutExpired:
        _log(f"  [CGAL] EXE 超时（300 秒）")
        return False
    except Exception as e:
        _log(f"  [CGAL] 错误：{e}")
        return False


def _ui_path(relative):
    """根据插件面板构建完整 UI 路径"""
    return f"{PALETTE_NAME}:{relative}"


def _log(msg):
    """打印消息到 ZBrush 控制台"""
    print(f"[ZMeshMend] {msg}")


def _progress(text, value=0.0):
    """更新记事条进度显示"""
    zbc.set_notebar_text(f"ZMeshMend: {text}", value)


def _clear_progress():
    """清除记事条"""
    zbc.set_notebar_text("", 0)


def _ensure_edit_mode():
    """确保处于编辑模式且有活动的 polymesh 工具"""
    try:
        if zbc.is_polymesh3d_solid():
            return True
    except Exception as e:
        _log(f"警告：is_polymesh3d_solid 检查失败：{e}")

    try:
        pt = zbc.query_mesh3d(0)
        if pt and pt[0] > 0:
            return True
    except Exception as e:
        _log(f"警告：query_mesh3d 检查失败：{e}")

    zbc.message_ok(
        "未找到活动的 PolyMesh3D 工具。\n\n"
        "请先选择一个 3D 工具并进入编辑模式。",
        "ZMeshMend - 错误"
    )
    return False


def _get_vertex_count():
    """获取当前网格顶点数"""
    try:
        result = zbc.query_mesh3d(0)
        if result:
            return int(result[0])
    except Exception as e:
        _log(f"警告：_get_vertex_count 失败：{e}")
    return 0


def _get_face_count():
    """获取当前网格面数"""
    try:
        result = zbc.query_mesh3d(1)
        if result:
            return int(result[0])
    except Exception as e:
        _log(f"警告：_get_face_count 失败：{e}")
    return 0


def _sharpen_mask(count=1):
    """锐化当前遮罩 N 次"""
    for i in range(count):
        try:
            zbc.press("Tool:Masking:SharpenMask")
        except Exception:
            try:
                zbc.press("Tool:Masking:Sharpen")
            except Exception:
                _log("警告：无法锐化遮罩（此 ZBrush 版本的按钮路径可能不同）")
                break


def _grow_mask(rings=1):
    """扩展当前遮罩 N 环"""
    try:
        for i in range(rings):
            zbc.press("Tool:Masking:GrowMask")
    except Exception:
        _log("警告：无法扩展遮罩（按钮路径可能不同）")


def _hide_masked():
    """根据当前遮罩状态隐藏点。

    注意：ZBrush 的 HidePt 按钮隐藏未遮罩的点（遮罩 = 保护）。
    调用者如需隐藏遮罩区域，须先反转遮罩。
    """
    candidates = [
        "Tool:Visibility:HidePt",
        "Tool:Visibility:Hide Pt",
    ]
    for path in candidates:
        try:
            if hasattr(zbc, "exists") and not zbc.exists(path):
                continue
            zbc.press(path)
            _log(f"  [vis] 通过 '{path}' 执行 HidePt")
            return True
        except Exception:
            continue
    _log("  警告：无法隐藏点")
    return False


def _delete_hidden():
    """删除隐藏的几何体"""
    try:
        zbc.press("Tool:Geometry:Modify Topology:Del Hidden")
    except Exception:
        try:
            zbc.press("Tool:Geometry:Del Hidden")
        except Exception:
            _log("警告：无法删除隐藏面")


def _close_holes():
    """闭合所有开放孔洞"""
    try:
        zbc.press("Tool:Geometry:Modify Topology:Close Holes")
    except Exception:
        try:
            zbc.press("Tool:Geometry:Close Holes")
        except Exception:
            _log("错误：无法闭合孔洞")


def _auto_groups():
    """根据网格连通性自动创建 PolyGroup"""
    try:
        zbc.press("Tool:PolyGroup:Auto Groups")
    except Exception:
        _log("警告：无法自动分组")


def _invert_mask():
    """反转当前遮罩。尝试多个已知按钮路径。"""
    candidates = [
        "Tool:Masking:Inverse",
        "Tool:Masking:Invert",
        "Tool:Masking:Mask Invert",
    ]
    for path in candidates:
        try:
            if hasattr(zbc, "exists") and not zbc.exists(path):
                continue
            zbc.press(path)
            _log(f"  [mask] 通过 '{path}' 反转遮罩")
            return True
        except Exception:
            continue
    _log("  警告：无法反转遮罩（无已知按钮路径）")
    return False


def _export_obj(filepath):
    """导出当前工具为 OBJ 文件"""
    try:
        zbc.set_next_filename(filepath)
        zbc.press("Tool:Export")
        zbc.update()
        return os.path.exists(filepath)
    except Exception as e:
        _log(f"警告：_export_obj 失败：{e}")
        return False


def _import_obj(filepath):
    """导入 OBJ 文件作为当前工具"""
    try:
        zbc.set_next_filename(filepath)
        zbc.press("Tool:Import")
        zbc.update()
        return True
    except Exception as e:
        _log(f"警告：_import_obj 失败：{e}")
        return False


def _read_obj_full(filepath):
    """读取 OBJ，保持顶点/面/组结构不变（保留四边形）。"""
    vertices = []
    faces = []
    groups = []
    current_group = "default"
    with open(filepath, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.strip()
            if not s:
                continue
            if s.startswith("v "):
                parts = s.split()
                vertices.append((float(parts[1]), float(parts[2]), float(parts[3])))
            elif s.startswith("f "):
                idx = []
                for tok in s[2:].split():
                    raw = tok.split("/")[0]
                    n = int(raw)
                    if n < 0:
                        n = len(vertices) + n + 1
                    idx.append(n)
                faces.append(idx)
                groups.append(current_group)
            elif s.startswith("g "):
                current_group = s[2:].strip() or "default"
    return vertices, faces, groups


def _merge_obj_with_patch(orig_obj, patch_obj, out_obj, weld_eps=None):
    """合并原始 OBJ 与填充补丁 OBJ。

    - 精确保留原始四边形/N 边形。
    - 焊接与原始顶点重合的补丁边界顶点，
      使用基于模型包围盒的相对容差，
      确保大尺度网格（ASCII OBJ 浮点精度掉尾数时）仍能正确焊接。
    - 将填充面标记为组名 'ZMeshMend_Fill'。

    返回 (成功, 填充面数)。
    """
    try:
        ov, of, og = _read_obj_full(orig_obj)
        pv, pf, _ = _read_obj_full(patch_obj)
    except Exception as e:
        _log(f"  OBJ 合并读取失败：{e}")
        return False, 0

    if not pf:
        return False, 0

    if weld_eps is None or weld_eps <= 0:
        if ov:
            xs = [v[0] for v in ov]; ys = [v[1] for v in ov]; zs = [v[2] for v in ov]
            diag = (
                (max(xs) - min(xs)) ** 2
                + (max(ys) - min(ys)) ** 2
                + (max(zs) - min(zs)) ** 2
            ) ** 0.5
            weld_eps = max(diag * 1e-5, 1e-5)
        else:
            weld_eps = 1e-5
    _log(f"  焊接容差：{weld_eps:.6g}")

    grid = {}
    cell = max(weld_eps * 2.0, 1e-12)

    def _key(p):
        return (
            int(p[0] // cell),
            int(p[1] // cell),
            int(p[2] // cell),
        )

    for i, v in enumerate(ov):
        grid.setdefault(_key(v), []).append(i + 1)

    eps2 = weld_eps * weld_eps
    patch_remap = {}
    next_idx = len(ov)
    new_verts = list(ov)
    welded_count = 0

    for pi, pv_ in enumerate(pv):
        kx, ky, kz = _key(pv_)
        best = -1
        best_d2 = eps2
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    bucket = grid.get((kx + dx, ky + dy, kz + dz))
                    if not bucket:
                        continue
                    for cand in bucket:
                        ov_ = new_verts[cand - 1]
                        d2 = ((ov_[0] - pv_[0]) ** 2
                              + (ov_[1] - pv_[1]) ** 2
                              + (ov_[2] - pv_[2]) ** 2)
                        if d2 < best_d2:
                            best_d2 = d2
                            best = cand
        if best > 0:
            patch_remap[pi + 1] = best
            welded_count += 1
        else:
            next_idx += 1
            new_verts.append(pv_)
            patch_remap[pi + 1] = next_idx
            grid.setdefault((kx, ky, kz), []).append(next_idx)

    new_faces = list(of)
    new_groups = list(og)

    fill_count = 0
    for face in pf:
        mapped = [patch_remap[v] for v in face]
        new_faces.append(mapped)
        new_groups.append("ZMeshMend_Fill")
        fill_count += 1

    try:
        with open(out_obj, "w", encoding="utf-8") as f:
            f.write("# ZMeshMend 合并网格\n")
            f.write(f"# 顶点数：{len(new_verts)} 面数：{len(new_faces)}\n")
            for v in new_verts:
                f.write(f"v {v[0]:.9f} {v[1]:.9f} {v[2]:.9f}\n")
            cur = None
            for i, face in enumerate(new_faces):
                g = new_groups[i] if i < len(new_groups) else "default"
                if g != cur:
                    cur = g
                    f.write(f"g {g}\n")
                f.write("f " + " ".join(str(x) for x in face) + "\n")
    except Exception as e:
        _log(f"  OBJ 合并写入失败：{e}")
        return False, 0

    _log(f"  合并 OBJ：原始={len(ov)}v/{len(of)}f，补丁={len(pv)}v/{len(pf)}f，"
         f"已焊接={welded_count}/{len(pv)}，"
         f"最终={len(new_verts)}v/{len(new_faces)}f")
    return True, fill_count


def _merge_patch_and_weld(patch_obj_path, orig_obj_path=None):
    """策略：在 Python 中构建合并 OBJ（原始 + 焊接顶点的补丁），
    然后 Tool:Import 导回结果。避开 SubTool API 的坑，
    并保持原始四边形完整。

    如提供 orig_obj_path 且存在，则复用之（跳过一次冗余的
    Tool:Export）。否则将当前网格导出到临时文件。
    """
    try:
        face_before = _get_face_count()

        cleanup_orig = False
        if orig_obj_path and os.path.exists(orig_obj_path):
            orig_obj = orig_obj_path
        else:
            orig_obj = os.path.join(tempfile.gettempdir(), "zmeshmend_orig.obj")
            cleanup_orig = True
            if not _export_obj(orig_obj):
                _log("  无法导出原始网格以进行补丁合并")
                return False

        merged_obj = os.path.join(tempfile.gettempdir(), "zmeshmend_merged.obj")

        ok, fill_count = _merge_obj_with_patch(orig_obj, patch_obj_path, merged_obj)
        if not ok or fill_count == 0:
            _log("  补丁为空或合并失败")
            try: os.remove(merged_obj)
            except Exception: pass
            if cleanup_orig:
                try: os.remove(orig_obj)
                except Exception: pass
            return False

        if not _import_obj(merged_obj):
            _log("  合并 OBJ 的 Tool:Import 失败，正在恢复原始……")
            _import_obj(orig_obj)
            try: os.remove(merged_obj)
            except Exception: pass
            if cleanup_orig:
                try: os.remove(orig_obj)
                except Exception: pass
            return False

        zbc.update(redraw_ui=True)

        face_after = _get_face_count()
        _log(f"  补丁已合并：{face_before} -> {face_after} 面 "
             f"(+{face_after - face_before}，填充={fill_count})")

        try: os.remove(merged_obj)
        except Exception: pass
        if cleanup_orig:
            try: os.remove(orig_obj)
            except Exception: pass

        return True
    except Exception as e:
        _log(f"  补丁合并失败：{e}")
        return False


def _count_faces_in_obj(filepath):
    """统计 OBJ 文件中的面行（'f '）数量"""
    count = 0
    try:
        with open(filepath, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("f "):
                    count += 1
    except Exception:
        pass
    return count


def _freeze_op(fn, desc="处理中"):
    """冻结 UI 并显示进度来执行操作"""
    def wrapped():
        _progress(desc, 0.0)
        try:
            fn()
        finally:
            _clear_progress()
    zbc.freeze(wrapped)


def _write_obj(filepath, vertices, faces, groups=None):
    """从顶点和面写入 OBJ 文件"""
    with open(filepath, "w", encoding="utf-8") as f:
        f.write("# ZMeshMend - 清理后的网格\n")
        f.write(f"# 顶点数：{len(vertices)}，面数：{len(faces)}\n")

        for v in vertices:
            f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")

        if groups:
            current_group = None
            for i, face in enumerate(faces):
                g = groups[i] if i < len(groups) else "default"
                if g != current_group:
                    current_group = g
                    f.write(f"g {g}\n")
                _write_face_line(f, face)
        else:
            for face in faces:
                _write_face_line(f, face)


def _write_face_line(f, face):
    """写入单行面数据到 OBJ"""
    f.write("f " + " ".join(str(i) for i in face) + "\n")


def _find_connected_components(faces):
    """通过面邻接关系查找连通分量"""
    face_to_vert = {i: set(f) for i, f in enumerate(faces)}
    vert_to_faces = {}
    for fi, face in enumerate(faces):
        for vi in face:
            vert_to_faces.setdefault(vi, set()).add(fi)

    visited = set()
    components = []

    for start_fi in range(len(faces)):
        if start_fi in visited:
            continue
        stack = [start_fi]
        component = set()
        while stack:
            fi = stack.pop()
            if fi in visited:
                continue
            visited.add(fi)
            component.add(fi)
            for vi in faces[fi]:
                for nfi in vert_to_faces.get(vi, set()):
                    if nfi not in visited:
                        stack.append(nfi)
        components.append(component)

    return components


def _remove_small_fragments(filepath, total_faces, min_frac, min_abs):
    """从 OBJ 文件中移除小型分离碎片"""
    vertices, faces, groups = _read_obj_full(filepath)

    if len(faces) < 2:
        return False

    components = _find_connected_components(faces)
    if len(components) <= 1:
        _log(f"  网格为单一连通分量（无碎片可移除）")
        return False

    threshold = max(min_abs, int(total_faces * min_frac))
    keep = set()
    large_count = 0
    removed_count = 0

    for comp in components:
        if len(comp) >= threshold:
            keep.update(comp)
            large_count += 1
        else:
            removed_count += 1

    if removed_count == 0:
        _log(f"  所有 {len(components)} 个分量均超过阈值 {threshold} 面")
        return False

    new_faces = [faces[i] for i in sorted(keep)]
    if groups:
        new_groups = [groups[i] for i in sorted(keep)]
    else:
        new_groups = None

    _write_obj(filepath, vertices, new_faces, new_groups)
    _log(f"  保留 {large_count} 个大分量，移除 {removed_count} 个小型碎片")
    return True


def _v3_sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _v3_len(v):
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def _v3_normalize(v):
    l = _v3_len(v)
    if l < 1e-12:
        return (0.0, 0.0, 0.0)
    return (v[0] / l, v[1] / l, v[2] / l)


def _find_boundary_edges(faces):
    """查找边界边（仅出现在一个面中的边）"""
    edge_count = {}
    for face in faces:
        n = len(face)
        for i in range(n):
            a, b = face[i], face[(i + 1) % n]
            key = (min(a, b), max(a, b))
            edge_count[key] = edge_count.get(key, 0) + 1

    return [k for k, v in edge_count.items() if v == 1]


def _build_boundary_loops(boundary_edges):
    """将边界边链成环"""
    adj = {}
    for a, b in boundary_edges:
        adj.setdefault(a, []).append(b)
        adj.setdefault(b, []).append(a)

    loops = []
    visited = set()

    for start in adj:
        if start in visited:
            continue
        loop = []
        stack = [start]
        while stack:
            v = stack.pop()
            if v in visited:
                continue
            visited.add(v)
            loop.append(v)
            for nb in adj.get(v, []):
                if nb not in visited:
                    stack.append(nb)
        if loop:
            loops.append(loop)

    return loops


def _compute_loop_centroid(loop, vertices):
    """计算边界环的质心"""
    if not loop:
        return (0, 0, 0)
    sx = sy = sz = 0.0
    for vi in loop:
        v = vertices[vi - 1]
        sx += v[0]
        sy += v[1]
        sz += v[2]
    n = len(loop)
    return (sx / n, sy / n, sz / n)


def _compute_loop_normal(loop, vertices):
    """使用 Newell 方法计算环的近似法线"""
    if len(loop) < 3:
        return (0, 1, 0)
    nx = ny = nz = 0.0
    n = len(loop)
    for i in range(n):
        j = (i + 1) % n
        vi = vertices[loop[i] - 1]
        vj = vertices[loop[j] - 1]
        nx += (vi[1] - vj[1]) * (vi[2] + vj[2])
        ny += (vi[2] - vj[2]) * (vi[0] + vj[0])
        nz += (vi[0] - vj[0]) * (vi[1] + vj[1])
    return _v3_normalize((nx, ny, nz))


def _collect_nearby_vertices(loop, vertices, faces, rings=2):
    """收集边界环附近的顶点"""
    loop_set = set(loop)

    adj = {}
    for face in faces:
        n = len(face)
        for i in range(n):
            a, b = face[i], face[(i + 1) % n]
            adj.setdefault(a, set()).add(b)
            adj.setdefault(b, set()).add(a)

    nearby = set(loop)
    frontier = set(loop)

    for _ in range(rings):
        new_frontier = set()
        for v in frontier:
            for nb in adj.get(v, set()):
                if nb not in nearby:
                    nearby.add(nb)
                    new_frontier.add(nb)
        frontier = new_frontier

    return [vertices[vi - 1] for vi in nearby]


def _fit_sphere(points):
    """用最小二乘法拟合球体到 3D 点集"""
    if len(points) < 4:
        return None

    n = len(points)
    sum_x = sum(p[0] for p in points)
    sum_y = sum(p[1] for p in points)
    sum_z = sum(p[2] for p in points)
    sum_x2 = sum(p[0] * p[0] for p in points)
    sum_y2 = sum(p[1] * p[1] for p in points)
    sum_z2 = sum(p[2] * p[2] for p in points)
    sum_x3 = sum(p[0] * p[0] * p[0] for p in points)
    sum_y3 = sum(p[1] * p[1] * p[1] for p in points)
    sum_z3 = sum(p[2] * p[2] * p[2] for p in points)
    sum_xy = sum(p[0] * p[1] for p in points)
    sum_xz = sum(p[0] * p[2] for p in points)
    sum_yz = sum(p[1] * p[2] for p in points)
    sum_xy2 = sum(p[0] * p[1] * p[1] for p in points)
    sum_xz2 = sum(p[0] * p[2] * p[2] for p in points)
    sum_yx2 = sum(p[1] * p[0] * p[0] for p in points)
    sum_yz2 = sum(p[1] * p[2] * p[2] for p in points)
    sum_zx2 = sum(p[2] * p[0] * p[0] for p in points)
    sum_zy2 = sum(p[2] * p[1] * p[1] for p in points)

    A = [
        [2 * sum_x2, 2 * sum_xy, 2 * sum_xz, 2 * sum_x],
        [2 * sum_xy, 2 * sum_y2, 2 * sum_yz, 2 * sum_y],
        [2 * sum_xz, 2 * sum_yz, 2 * sum_z2, 2 * sum_z],
        [2 * sum_x, 2 * sum_y, 2 * sum_z, 2 * n],
    ]
    b = [
        sum_x3 + sum_xy2 + sum_xz2,
        sum_yx2 + sum_y3 + sum_yz2,
        sum_zx2 + sum_zy2 + sum_z3,
        sum_x2 + sum_y2 + sum_z2,
    ]

    try:
        det = (
            A[0][0] * (A[1][1] * (A[2][2] * A[3][3] - A[2][3] * A[3][2])
                       - A[1][2] * (A[2][1] * A[3][3] - A[2][3] * A[3][1])
                       + A[1][3] * (A[2][1] * A[3][2] - A[2][2] * A[3][1]))
            - A[0][1] * (A[1][0] * (A[2][2] * A[3][3] - A[2][3] * A[3][2])
                         - A[1][2] * (A[2][0] * A[3][3] - A[2][3] * A[3][0])
                         + A[1][3] * (A[2][0] * A[3][2] - A[2][2] * A[3][0]))
            + A[0][2] * (A[1][0] * (A[2][1] * A[3][3] - A[2][3] * A[3][1])
                         - A[1][1] * (A[2][0] * A[3][3] - A[2][3] * A[3][0])
                         + A[1][3] * (A[2][0] * A[3][1] - A[2][1] * A[3][0]))
            - A[0][3] * (A[1][0] * (A[2][1] * A[3][2] - A[2][2] * A[3][1])
                         - A[1][1] * (A[2][0] * A[3][2] - A[2][2] * A[3][0])
                         + A[1][2] * (A[2][0] * A[3][1] - A[2][1] * A[3][0]))
        )

        if abs(det) < 1e-30:
            return None

        inv_det = 1.0 / det
        cx = inv_det * (
            b[0] * (A[1][1] * (A[2][2] * A[3][3] - A[2][3] * A[3][2])
                    - A[1][2] * (A[2][1] * A[3][3] - A[2][3] * A[3][1])
                    + A[1][3] * (A[2][1] * A[3][2] - A[2][2] * A[3][1]))
            - A[0][1] * (b[1] * (A[2][2] * A[3][3] - A[2][3] * A[3][2])
                         - A[1][2] * (b[2] * A[3][3] - A[2][3] * b[3])
                         + A[1][3] * (b[2] * A[3][2] - A[2][2] * b[3]))
            + A[0][2] * (b[1] * (A[2][1] * A[3][3] - A[2][3] * A[3][1])
                         - A[1][1] * (b[2] * A[3][3] - A[2][3] * b[3])
                         + A[1][3] * (b[2] * A[3][1] - A[2][1] * b[3]))
            - A[0][3] * (b[1] * (A[2][1] * A[3][2] - A[2][2] * A[3][1])
                         - A[1][1] * (b[2] * A[3][2] - A[2][2] * b[3])
                         + A[1][2] * (b[2] * A[3][1] - A[2][1] * b[3]))
        )
        cy = inv_det * (
            A[0][0] * (b[1] * (A[2][2] * A[3][3] - A[2][3] * A[3][2])
                       - A[1][2] * (b[2] * A[3][3] - A[2][3] * b[3])
                       + A[1][3] * (b[2] * A[3][2] - A[2][2] * b[3]))
            - b[0] * (A[1][0] * (A[2][2] * A[3][3] - A[2][3] * A[3][2])
                      - A[1][2] * (A[2][0] * A[3][3] - A[2][3] * A[3][0])
                      + A[1][3] * (A[2][0] * A[3][2] - A[2][2] * A[3][0]))
            + A[0][2] * (A[1][0] * (b[2] * A[3][3] - A[2][3] * b[3])
                         - b[1] * (A[2][0] * A[3][3] - A[2][3] * A[3][0])
                         + A[1][3] * (A[2][0] * b[3] - b[2] * A[3][0]))
            - A[0][3] * (A[1][0] * (b[2] * A[3][2] - A[2][2] * b[3])
                         - b[1] * (A[2][0] * A[3][2] - A[2][2] * A[3][0])
                         + A[1][2] * (A[2][0] * b[3] - b[2] * A[3][0]))
        )
        cz = inv_det * (
            A[0][0] * (A[1][1] * (b[2] * A[3][3] - A[2][3] * b[3])
                       - b[1] * (A[2][1] * A[3][3] - A[2][3] * A[3][1])
                       + A[1][3] * (A[2][1] * b[3] - b[2] * A[3][1]))
            - A[0][1] * (A[1][0] * (b[2] * A[3][3] - A[2][3] * b[3])
                         - b[1] * (A[2][0] * A[3][3] - A[2][3] * A[3][0])
                         + A[1][3] * (A[2][0] * b[3] - b[2] * A[3][0]))
            + b[0] * (A[1][0] * (A[2][1] * A[3][3] - A[2][3] * A[3][1])
                      - A[1][1] * (A[2][0] * A[3][3] - A[2][3] * A[3][0])
                      + A[1][3] * (A[2][0] * A[3][1] - A[2][1] * A[3][0]))
            - A[0][3] * (A[1][0] * (A[2][1] * b[3] - b[2] * A[3][1])
                         - A[1][1] * (A[2][0] * b[3] - b[2] * A[3][0])
                         + b[1] * (A[2][0] * A[3][1] - A[2][1] * A[3][0]))
        )
    except (ZeroDivisionError, IndexError):
        return None

    center = (cx, cy, cz)

    r_sq_sum = 0.0
    for p in points:
        dx = p[0] - cx
        dy = p[1] - cy
        dz = p[2] - cz
        r_sq_sum += dx * dx + dy * dy + dz * dz
    radius = math.sqrt(r_sq_sum / n)

    error_sum = 0.0
    for p in points:
        dx = p[0] - cx
        dy = p[1] - cy
        dz = p[2] - cz
        d = math.sqrt(dx * dx + dy * dy + dz * dz)
        error_sum += (d - radius) ** 2
    error = math.sqrt(error_sum / n) / max(radius, 1e-8)

    return center, radius, error


def _project_to_sphere(point, center, radius):
    """将点投影到球面上"""
    dx = point[0] - center[0]
    dy = point[1] - center[1]
    dz = point[2] - center[2]
    d = math.sqrt(dx * dx + dy * dy + dz * dz)
    if d < 1e-8:
        return (center[0], center[1], center[2] + radius)
    s = radius / d
    return (center[0] + dx * s, center[1] + dy * s, center[2] + dz * s)


def _fill_hole_smart(vertices, faces, loop, groups_list=None):
    """
    使用球体拟合进行曲率感知的边界环填充。
    若周围曲面能较好地拟合为球体，新顶点将投影到该球面上。
    否则回退到基于质心的线性填充。
    """
    if len(loop) < 3:
        return [], [], []

    loop_centroid = _compute_loop_centroid(loop, vertices)
    loop_normal = _compute_loop_normal(loop, vertices)

    nearby = _collect_nearby_vertices(loop, vertices, faces, rings=2)

    span = max(
        _v3_len(_v3_sub(vertices[vi - 1], loop_centroid))
        for vi in loop
    ) if loop else 0.0

    use_sphere = False
    sphere_center = None
    sphere_radius = None

    if len(nearby) >= 6:
        sphere_result = _fit_sphere(nearby)
        if sphere_result is not None:
            sc, sr, serr = sphere_result
            if serr < 0.05:
                use_sphere = True
                sphere_center = sc
                sphere_radius = sr

    new_vertices = []
    new_faces = []
    new_groups = []

    vtx_offset = len(vertices)

    center_pt = loop_centroid
    if use_sphere:
        center_pt = _project_to_sphere(loop_centroid, sphere_center, sphere_radius)

    center_idx = vtx_offset + 1
    vertices.append(center_pt)
    new_vertices.append(center_pt)

    n = len(loop)
    for i in range(n):
        a = loop[i]
        b = loop[(i + 1) % n]
        face = [center_idx, b, a]
        faces.append(face)
        new_faces.append(face)
        if groups_list is not None:
            groups_list.append("ZMeshMend_Fill")

    return new_vertices, new_faces, ["ZMeshMend_Fill"] * len(new_faces)


def do_close_all_holes(sender=""):
    """功能 1：自动闭合当前网格所有孔洞"""
    _log("=" * 50)
    _log("闭合所有孔洞")
    _log("=" * 50)

    if not _ensure_edit_mode():
        return

    vtx_before = _get_vertex_count()
    face_before = _get_face_count()
    _log(f"之前：{vtx_before} 顶点，{face_before} 面")

    _close_holes()
    zbc.update(redraw_ui=True)

    vtx_after = _get_vertex_count()
    face_after = _get_face_count()
    _log(f"之后：{vtx_after} 顶点，{face_after} 面")
    _log(f"新增：{face_after - face_before} 面")

    _progress("完成！", 1.0)
    zbc.message_ok(
        f"闭合孔洞完成！\n\n"
        f"已闭合当前网格上的孔洞。\n"
        f"新增面数：{face_after - face_before}",
        "ZMeshMend"
    )
    _clear_progress()


def do_remove_small_fragments(sender=""):
    """功能 2：移除小型分离碎片"""
    _log("=" * 50)
    _log("移除小型碎片")
    _log("=" * 50)

    if not _ensure_edit_mode():
        return

    vtx_before = _get_vertex_count()
    face_before = _get_face_count()
    _log(f"之前：{vtx_before} 顶点，{face_before} 面")

    tmp_obj = os.path.join(tempfile.gettempdir(), "zmeshmend_frag.obj")

    _progress("正在导出网格……", 0.1)
    if not _export_obj(tmp_obj):
        _log("错误：导出网格失败")
        _clear_progress()
        return

    _progress("正在分析碎片……", 0.4)
    removed = _remove_small_fragments(
        tmp_obj, face_before,
        CONFIG["fragmentMinFraction"],
        CONFIG["fragmentMinFaces"]
    )

    if removed:
        _progress("正在导入清理后的网格……", 0.7)
        if _import_obj(tmp_obj):
            zbc.update(redraw_ui=True)
            vtx_after = _get_vertex_count()
            face_after = _get_face_count()
            _log(f"之后：{vtx_after} 顶点，{face_after} 面")
            _log(f"已移除：{face_before - face_after} 面")
        else:
            _log("错误：导入失败，网格可能处于不一致状态")
    else:
        _log("未发现可移除的小型碎片")

    try:
        os.remove(tmp_obj)
    except Exception:
        pass

    _progress("完成！", 1.0)
    zbc.message_ok(
        "碎片移除完成！\n\n"
        "小型分离碎片已移除。\n"
        "详情请查看控制台。",
        "ZMeshMend"
    )
    _clear_progress()


def do_smooth_open_edges(sender=""):
    """功能：平滑所有开放边界环（Smooth Open Edge）。

    导出 OBJ → CGAL Chaikin 平滑边界 + Laplacian 平滑邻域 →
    切平面投影保持体积 → 导入 OBJ 回 ZBrush。

    PolyGroup 完全保留，不增删顶点/面。
    """
    _log("=" * 50)
    _log("平滑开放边界环（Smooth Open Edge）")
    _log("=" * 50)

    if not _ensure_edit_mode():
        return

    vtx_before = _get_vertex_count()
    face_before = _get_face_count()
    _log(f"之前：{vtx_before} 顶点，{face_before} 面")

    if not _cgal_available():
        _log("CGAL 核心未找到，无法执行平滑。")
        zbc.message_ok(
            "CGAL 核心未找到！\n\n"
            "请将 zmeshmend_core.exe 编译到 ZMeshMendData/ 目录。",
            "ZMeshMend"
        )
        return

    tmp_in = os.path.join(tempfile.gettempdir(), "zmeshmend_smooth_in.obj")
    tmp_out = os.path.join(tempfile.gettempdir(), "zmeshmend_smooth_out.obj")

    _progress("正在导出网格……", 0.10)
    if not _export_obj(tmp_in):
        _log("错误：导出网格失败")
        _clear_progress()
        return

    _progress("CGAL 正在平滑边界……", 0.30)
    success = _call_cgal_smooth_border(tmp_in, tmp_out)

    if success and os.path.exists(tmp_out) and _count_faces_in_obj(tmp_out) > 0:
        _progress("正在导入平滑后的网格……", 0.70)
        if _import_obj(tmp_out):
            zbc.update(redraw_ui=True)
            vtx_after = _get_vertex_count()
            face_after = _get_face_count()
            _log(f"之后：{vtx_after} 顶点，{face_after} 面")
            _log("平滑完成：仅移动边界顶点，拓扑不变")
            _progress("完成！", 1.0)
            zbc.message_ok(
                "平滑开放边界环完成！\n\n"
                "边界环经 Chaikin 平滑 + 多圈 falloff，\n"
                "保持洞口体积（切平面投影）。\n"
                "顶点数和面数不变，PolyGroup 完全保留。",
                "ZMeshMend"
            )
        else:
            _log("错误：导入平滑结果失败")
            zbc.message_ok("导入失败！", "ZMeshMend - 错误")
    else:
        _log("CGAL 平滑失败或输出为空")
        zbc.message_ok(
            "平滑失败！\n\n"
            "请检查 CGAL 控制台输出以获取详细信息。",
            "ZMeshMend - 错误"
        )

    for f in [tmp_in, tmp_out]:
        try:
            os.remove(f)
        except Exception:
            pass

    _clear_progress()


def do_relax_wireframe(sender=""):
    """功能：布线放松（Relax Wireframe）。

    - 无遮罩：放松整个模型。
    - 有遮罩：仅放松遮罩区域顶点（mask < 0xFFFF 的顶点参与放松）。

    流程：导出 GoZ binary（含 MASK16_LIST）→ CGAL 读 m_mask 构造 vertex_allow →
          仅对遮罩区顶点做 oaRelaxVerts 风格放松 → 写回 GoZ → 导入。
    放松后保留原始遮罩，不修改 polygroup。

    不改变拓扑，顶点/面数不变。
    """
    _log("=" * 50)
    _log("布线放松（Relax Wireframe）")
    _log("=" * 50)

    if not _ensure_edit_mode():
        return

    vtx_before = _get_vertex_count()
    face_before = _get_face_count()
    _log(f"之前：{vtx_before} 顶点，{face_before} 面")

    if not _cgal_available():
        _log("CGAL 核心未找到，无法执行布线放松。")
        zbc.message_ok(
            "CGAL 核心未找到！\n\n"
            "请将 zmeshmend_core.exe 编译到 ZMeshMendData/ 目录。",
            "ZMeshMend"
        )
        return

    tmp_in = os.path.join(tempfile.gettempdir(), "zmeshmend_relax_in.GoZ")
    tmp_out = os.path.join(tempfile.gettempdir(), "zmeshmend_relax_out.GoZ")

    _progress("正在导出 GoZ（含遮罩信息）……", 0.20)
    if not _export_obj(tmp_in):
        _log("错误：导出 GoZ 失败")
        _clear_progress()
        return

    _progress("CGAL 正在放松布线……", 0.40)
    success = _call_cgal_relax_wireframe(tmp_in, tmp_out)

    if success and os.path.exists(tmp_out):
        _progress("正在导入放松后的网格……", 0.70)
        if _import_obj(tmp_out):
            zbc.update(redraw_ui=True)
            vtx_after = _get_vertex_count()
            face_after = _get_face_count()
            _log(f"之后：{vtx_after} 顶点，{face_after} 面")
            _log("放松完成：仅沿切平面移动顶点，拓扑不变")
            _progress("完成！", 1.0)
            zbc.message_ok(
                "布线放松完成！\n\n"
                "有遮罩则仅放松遮罩区，否则放松全模型。\n"
                "Laplacian 1-ring + AABB 投影回原始表面，\n"
                "保持体积和细节，边界顶点固定。",
                "ZMeshMend"
            )
        else:
            _log("错误：导入放松结果失败")
            zbc.message_ok("导入失败！", "ZMeshMend - 错误")
    else:
        _log("CGAL 布线放松失败或输出为空")
        zbc.message_ok(
            "布线放松失败！\n\n"
            "请检查 CGAL 控制台输出以获取详细信息。",
            "ZMeshMend - 错误"
        )

    for f in [tmp_in, tmp_out]:
        try:
            os.remove(f)
        except Exception:
            pass

    _clear_progress()


def do_close_with_polygroup_mask(sender=""):
    """功能 3+4：使用 CGAL refine+fair 闭合孔洞，为填充区域分配新 PolyGroup。

    导出 OBJ → CGAL 填充 → 导入 GoZ（内嵌 PolyGroup）。
    GoZ 格式原生携带 PolyGroup ID 和遮罩数据，
    因此填充面自动获得独立的 PolyGroup。
    """
    _log("=" * 50)
    if _cgal_available():
        _log("闭合孔洞 + PolyGroup（CGAL refine+fair + GoZ）")
    else:
        _log("闭合孔洞 + PolyGroup（ZBrush 内置）")
    _log("=" * 50)

    if not _ensure_edit_mode():
        return

    vtx_before = _get_vertex_count()
    face_before = _get_face_count()
    _log(f"之前：{vtx_before} 顶点，{face_before} 面")

    if _cgal_available():
        tmp_in = os.path.join(tempfile.gettempdir(), "zmeshmend_cgal_in.obj")
        tmp_patch = os.path.join(tempfile.gettempdir(), "zmeshmend_cgal_patch.obj")

        _progress("正在导出网格供 CGAL 使用……", 0.10)
        if not _export_obj(tmp_in):
            _log("错误：导出网格失败")
            _clear_progress()
            return

        _progress("CGAL 三角剖分 refine 并 fair 孔洞……", 0.20)
        success, faces_added = _call_cgal_fill(tmp_in, tmp_patch)

        merged = False
        if success and os.path.exists(tmp_patch) and _count_faces_in_obj(tmp_patch) > 0:
            _progress("正在合并填充补丁……", 0.70)
            merged = _merge_patch_and_weld(tmp_patch, orig_obj_path=tmp_in)

        if merged:
            vtx_after = _get_vertex_count()
            face_after = _get_face_count()
            _log(f"之后：{vtx_after} 顶点，{face_after} 面")
            _log(f"新增：{face_after - face_before} 面")
            _log("  补丁通过 OBJ 焊接合并（原始四边形已保留）")
        else:
            _log("补丁合并失败或为空。回退到 ZBrush 内置闭合……")
            _close_holes()
            zbc.update(redraw_ui=True)
            _auto_groups()
            zbc.update(redraw_ui=True)

            vtx_after = _get_vertex_count()
            face_after = _get_face_count()
            _log(f"之后（回退）：{vtx_after} 顶点，{face_after} 面")

        for f in [tmp_in, tmp_patch]:
            try:
                os.remove(f)
            except Exception:
                pass
    else:
        _log("  未找到 CGAL EXE，使用 ZBrush 内置闭合孔洞")
        _log("  启用 CGAL：请将 zmeshmend_core.exe 编译到 ZMeshMendData/ 目录")
        _close_holes()
        zbc.update(redraw_ui=True)
        _auto_groups()
        zbc.update(redraw_ui=True)

        vtx_after = _get_vertex_count()
        face_after = _get_face_count()
        _log(f"之后：{vtx_after} 顶点，{face_after} 面")
        _log(f"新增：{face_after - face_before} 面")

    _progress("完成！", 1.0)
    zbc.message_ok(
        "MendHoles + PolyGroup 完成！\n\n"
        "孔洞已用 CGAL refine+fair 填充。\n"
        "补丁通过 OBJ 焊接合并；原始四边形已保留。\n"
        "填充面已标记为 'ZMeshMend_Fill' PolyGroup。\n\n"
        "提示：Ctrl+Shift+点击 PolyGroup 可对其遮罩。",
        "ZMeshMend"
    )
    _clear_progress()


def do_mask_based_cleanup(sender=""):
    """功能 5：基于遮罩的网格清理工作流。
    步骤：锐化遮罩 → 扩展遮罩 → 反转 → 隐藏遮罩区 → 删除 →
           闭合孔洞 → 分组
    遮罩指示要删除的区域。遮罩区域被移除后，
    产生的孔洞会被填充；未遮罩区域保持不变。
    """
    _log("=" * 50)
    _log("基于遮罩的清理工作流")
    _log("=" * 50)

    if not _ensure_edit_mode():
        return

    vtx_before = _get_vertex_count()
    face_before = _get_face_count()
    _log(f"之前：{vtx_before} 顶点，{face_before} 面")

    sharpen_passes = CONFIG["maskSharpenPasses"]
    grow_rings = CONFIG["maskGrowRings"]

    _progress("步骤 1/5：锐化遮罩……", 0.05)
    _log(f"  锐化遮罩（{sharpen_passes} 遍）")
    _sharpen_mask(sharpen_passes)
    zbc.update()

    _progress("步骤 2/5：扩展遮罩……", 0.20)
    _log(f"  扩展遮罩（{grow_rings} 环）")
    _grow_mask(grow_rings)
    zbc.update()

    _progress("步骤 3/5：删除遮罩面……", 0.35)
    _log("  反转遮罩使未遮罩区域变为隐藏……")
    _invert_mask()
    zbc.update()
    _log("  HidePt 隐藏未遮罩的点（即要删除的区域）……")
    _hide_masked()
    zbc.update()
    _log("  删除隐藏面（即遮罩区域）……")
    _delete_hidden()
    zbc.update()
    _show_all()
    zbc.update()

    face_after_delete = _get_face_count()
    _log(f"  删除后面数：{face_after_delete}（已删除 {face_before - face_after_delete}）")

    _progress("步骤 5/6：闭合孔洞（CGAL 补丁合并）……", 0.60)
    cgal_merged = False
    if _cgal_available():
        tmp_in = os.path.join(tempfile.gettempdir(), "zmeshmend_mask_in.obj")
        tmp_patch = os.path.join(tempfile.gettempdir(), "zmeshmend_mask_patch.obj")

        if _export_obj(tmp_in):
            success, cgal_added = _call_cgal_fill(tmp_in, tmp_patch)
            if success and os.path.exists(tmp_patch) and _count_faces_in_obj(tmp_patch) > 0:
                if _merge_patch_and_weld(tmp_patch, orig_obj_path=tmp_in):
                    cgal_merged = True
                    _log(f"  CGAL：新增 {cgal_added} 面，通过 OBJ 焊接合并")
                else:
                    _log("  补丁合并失败，回退到 ZBrush 闭合孔洞")
                    _close_holes()
            else:
                _log("  CGAL 失败或补丁为空，回退到 ZBrush 闭合孔洞")
                _close_holes()
            for f in [tmp_in, tmp_patch]:
                try:
                    os.remove(f)
                except Exception:
                    pass
        else:
            _log("  导出失败，使用 ZBrush 内置")
            _close_holes()
    else:
        _close_holes()
    zbc.update(redraw_ui=True)

    face_after_close = _get_face_count()
    _log(f"  闭合后面数：{face_after_close}（新增 {face_after_close - face_after_delete}）")

    _progress("步骤 6/6：为填充区域分组……", 0.85)
    if cgal_merged:
        _log("  CGAL 补丁已合并：ZMeshMend_Fill PolyGroup 从 OBJ 标签保留")
    else:
        _log("  CGAL 未合并 - 应用 Auto Groups 回退")
        _auto_groups()
    zbc.update(redraw_ui=True)

    vtx_after = _get_vertex_count()
    face_after = _get_face_count()
    _log(f"最终：{vtx_after} 顶点，{face_after} 面")
    _log(f"总计新增面数：{face_after - face_before}")

    _progress("完成！", 1.0)
    zbc.message_ok(
        "基于遮罩的清理完成！\n\n"
        f"操作前面数：{face_before}\n"
        f"删除面数：{face_before - face_after_delete}\n"
        f"填充面数：{face_after_close - face_after_delete}\n"
        f"最终面数：{face_after}\n\n"
        "补丁通过 OBJ 焊接合并；原始四边形已保留。\n"
        "填充面已标记为 'ZMeshMend_Fill' PolyGroup。\n"
        "使用 Ctrl+Shift+点击可按 PolyGroup 遮罩。",
        "ZMeshMend"
    )
    _clear_progress()


def do_export_mesh(sender=""):
    """导出当前网格为 OBJ 供外部处理"""
    _log("=" * 50)
    _log("导出网格供处理")
    _log("=" * 50)

    if not _ensure_edit_mode():
        return

    save_path = zbc.ask_filename("*.obj", "mesh_export.obj", "导出网格 OBJ")
    if not save_path:
        _log("导出已取消")
        return

    if _export_obj(save_path):
        vtx = _get_vertex_count()
        face = _get_face_count()
        _log(f"已导出：{vtx} 顶点，{face} 面")
        _log(f"路径：{save_path}")
        zbc.message_ok(
            f"导出完成！\n\n"
            f"顶点数：{vtx}\n"
            f"面数：{face}\n"
            f"保存至：{save_path}",
            "ZMeshMend"
        )
    else:
        _log("错误：导出失败")
        zbc.message_ok("导出失败！", "ZMeshMend - 错误")


def do_import_mesh(sender=""):
    """导入外部处理过的 OBJ 网格"""
    _log("=" * 50)
    _log("导入处理后的网格")
    _log("=" * 50)

    load_path = zbc.ask_filename("*.obj", "", "导入 OBJ 网格")
    if not load_path:
        _log("导入已取消")
        return

    if _import_obj(load_path):
        zbc.update(redraw_ui=True)
        vtx = _get_vertex_count()
        face = _get_face_count()
        _log(f"已导入：{vtx} 顶点，{face} 面")
        zbc.message_ok(
            f"导入完成！\n\n"
            f"顶点数：{vtx}\n"
            f"面数：{face}",
            "ZMeshMend"
        )
    else:
        _log("错误：导入失败")
        zbc.message_ok("导入失败！", "ZMeshMend - 错误")


def _on_close_holes_click(sender=""):
    _freeze_op(lambda: do_close_all_holes(sender), "正在闭合所有孔洞……")


def _on_remove_frag_click(sender=""):
    _freeze_op(lambda: do_remove_small_fragments(sender), "正在移除小型碎片……")


def _on_close_group_mask_click(sender=""):
    _freeze_op(lambda: do_close_with_polygroup_mask(sender), "正在闭合孔洞并检测曲率……")


def _on_mask_cleanup_click(sender=""):
    _freeze_op(lambda: do_mask_based_cleanup(sender), "基于遮罩的清理工作流……")


def _on_export_click(sender=""):
    _freeze_op(lambda: do_export_mesh(sender), "正在导出网格……")


def _on_import_click(sender=""):
    _freeze_op(lambda: do_import_mesh(sender), "正在导入处理后的网格……")


def _on_smooth_open_edges_click(sender=""):
    _freeze_op(lambda: do_smooth_open_edges(sender), "正在平滑开放边界环……")


def _on_relax_wireframe_click(sender=""):
    _freeze_op(lambda: do_relax_wireframe(sender), "正在放松全模型布线……")


def _on_config_change(sender="", value=0.0):
    """处理配置滑块/开关的变更"""
    name = sender.split(":")[-1] if ":" in sender else sender

    if name == "Fragment Min Fraction":
        CONFIG["fragmentMinFraction"] = value
        save_config()
    elif name == "Fragment Min Faces":
        CONFIG["fragmentMinFaces"] = int(value)
        save_config()
    elif name == "Mask Grow Rings":
        CONFIG["maskGrowRings"] = int(value)
        save_config()
    elif name == "Mask Sharpen Passes":
        CONFIG["maskSharpenPasses"] = int(value)
        save_config()
    elif name == "Remove Small Fragments":
        CONFIG["removeSmallFragments"] = bool(value)
        save_config()
    elif name == "Smooth Iterations":
        CONFIG["smoothIterations"] = int(value)
        save_config()
    elif name == "Smooth Rings":
        CONFIG["smoothRings"] = int(value)
        save_config()
    elif name == "Relax Iterations":
        CONFIG["relaxIterations"] = int(value)
        save_config()
    elif name == "Relax Factor":
        CONFIG["relaxFactor"] = round(float(value), 2)
        save_config()


def _on_info_click(sender=""):
    """显示插件信息"""
    zbc.message_ok(
        "ZMeshMend v1.2.0\n\n"
        "1. CGAL 曲率感知补洞 + PolyGroup 标记\n"
        "2. 遮罩驱动网格清理\n"
        "3. 小碎片自动移除\n"
        "4. 平滑开放边缘（边界 Chaikin + Laplacian）\n"
        "5. 全模型布线放松（CGAL smooth_shape）\n"
        "6. ZBrush 内置 Close Holes 兜底",
        "ZMeshMend"
    )


def build_ui():
    """构建 ZMeshMend 插件 UI"""
    load_config()

    if zbc.exists(PALETTE_NAME):
        zbc.close(PALETTE_NAME)

    zbc.add_palette(PALETTE_NAME, docking_bar=1)

    zbc.add_subpalette(SUBPALETTE_MAIN, title_mode=2)

    zbc.add_button(
        _ui_path("Close Holes:Close All Holes"),
        "自动闭合当前网格上的所有开放孔洞。",
        _on_close_holes_click,
        width=1.0,
    )

    zbc.add_button(
        _ui_path("Close Holes:MendHoles + PolyGroup"),
        "使用曲率感知智能填充闭合孔洞，为新填充区域"
        "创建 PolyGroup，便于遮罩操作。",
        _on_close_group_mask_click,
        width=1.0,
    )

    zbc.add_button(
        _ui_path("Close Holes:Mask-Based Cleanup"),
        "完整工作流：锐化遮罩 > 扩展 > 删除遮罩面 "
        "> 闭合孔洞 > 为填充区域创建新 PolyGroup。",
        _on_mask_cleanup_click,
        width=1.0,
    )

    zbc.add_button(
        _ui_path("Close Holes:Remove Small Fragments"),
        "分析网格连通性，移除小型"
        "分离碎片和网格碎屑。",
        _on_remove_frag_click,
        width=1.0,
    )

    zbc.add_button(
        _ui_path("Close Holes:Smooth Open Edges"),
        "对开放边界环做 Chaikin 平滑 + 多圈 falloff，"
        "使洞口边缘更流畅，不改变拓扑。",
        _on_smooth_open_edges_click,
        width=1.0,
    )

    zbc.add_button(
        _ui_path("Close Holes:Relax Wireframe"),
        "使用 CGAL smooth_shape 对全模型布线做切线方向放松，"
        "保持体积和细节，边界顶点固定不动。",
        _on_relax_wireframe_click,
        width=1.0,
    )

    zbc.add_subpalette(SUBPALETTE_CONF, title_mode=0)

    zbc.add_switch(
        _ui_path("Settings:Remove Small Fragments"),
        CONFIG["removeSmallFragments"],
        "清理时自动移除小型碎片。",
        _on_config_change,
        width=1.0,
    )

    zbc.add_slider(
        _ui_path("Settings:Fragment Min Fraction"),
        CONFIG["fragmentMinFraction"],
        100,
        0.0,
        0.5,
        "保留碎片所需的最小面数占比（0.0-0.5）。",
        _on_config_change,
        width=1.0,
    )

    zbc.add_slider(
        _ui_path("Settings:Fragment Min Faces"),
        float(CONFIG["fragmentMinFaces"]),
        1,
        1.0,
        500.0,
        "保留碎片所需的最小绝对面数。",
        _on_config_change,
        width=1.0,
    )

    zbc.add_slider(
        _ui_path("Settings:Mask Grow Rings"),
        float(CONFIG["maskGrowRings"]),
        1,
        0.0,
        5.0,
        "删除面前扩展遮罩的环数。",
        _on_config_change,
        width=1.0,
    )

    zbc.add_slider(
        _ui_path("Settings:Mask Sharpen Passes"),
        float(CONFIG["maskSharpenPasses"]),
        1,
        0.0,
        5.0,
        "扩展前锐化遮罩的遍数。",
        _on_config_change,
        width=1.0,
    )

    zbc.add_slider(
        _ui_path("Settings:Smooth Iterations"),
        float(CONFIG["smoothIterations"]),
        1,
        1.0,
        20.0,
        "边界 Chaikin 平滑迭代次数（1-20）。",
        _on_config_change,
        width=1.0,
    )

    zbc.add_slider(
        _ui_path("Settings:Smooth Rings"),
        float(CONFIG["smoothRings"]),
        1,
        1.0,
        20.0,
        "边界平滑向内扩展圈数（1-20）。",
        _on_config_change,
        width=1.0,
    )

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

    zbc.add_slider(
        _ui_path("Settings:Relax Factor"),
        float(CONFIG["relaxFactor"]),
        0.05,
        0.1,
        1.0,
        "全局布线放松阻尼（0.1-1.0）。越小越保守，越大越激进。",
        _on_config_change,
        width=1.0,
    )

    zbc.add_subpalette(SUBPALETTE_INFO, title_mode=0)

    zbc.add_button(
        _ui_path("Info:Export Current Mesh"),
        "导出当前网格为 OBJ 供外部处理。",
        _on_export_click,
        width=1.0,
    )

    zbc.add_button(
        _ui_path("Info:Import Processed Mesh"),
        "导入外部处理过的 OBJ 网格。",
        _on_import_click,
        width=1.0,
    )

    zbc.add_button(
        _ui_path("Info:About ZMeshMend"),
        "显示插件信息和版本号。",
        _on_info_click,
        width=1.0,
    )

    zbc.maximize(SUBPALETTE_MAIN)
    zbc.maximize(SUBPALETTE_CONF)
    zbc.maximize(SUBPALETTE_INFO)

    _log("ZMeshMend v1.2.0 已加载")
    _log(f"配置路径：{CONFIG_PATH}")
    if _cgal_available():
        _log(f"CGAL 核心：{_CGAL_EXE_REL}")
    else:
        _log(f"CGAL 核心：未找到（使用 ZBrush 内置闭合孔洞）")
        _log(f"  预期位置：{_CGAL_EXE_REL}")
        _log(f"  构建方式：ZMeshMendData/build.bat")


def main():
    """入口点 - 构建插件 UI"""
    build_ui()


if __name__ == "__main__":
    main()
