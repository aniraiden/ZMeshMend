/**
 * ZMeshMend CGAL 核心 v2.0 (GoZ)
 * ==================================
 * 基于 CGAL 的孔洞填充引擎，用于 ZMeshMend ZBrush 插件。
 *
 * 读取 GoZ 网格（包含 PolyGroups、Mask、UV），使用
 * CGAL::Polygon_mesh_processing::triangulate_refine_and_fair_hole()
 * 填充所有边界孔洞，并将结果写回 GoZ。
 *
 * 新增的填充面将被分配一个新的 PolyGroup ID（已有最大 ID + 1）。
 * 原有的 PolyGroups 和 Mask 将被保留。
 *
 * 构建：
 *   mkdir build && cd build && cmake .. && cmake --build . --config Release
 *
 * 用法：
 *   zmeshmend_core <input.GoZ> <output.GoZ> [fill.GoZ]
 *
 * 如果指定了 fill.GoZ，则仅将新填充的面写入该文件
 * （用于作为 ZBrush 子工具进行 PolyGroup 合并）。
 */

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>

#include <CGAL/Polygon_mesh_processing/triangulate_hole.h>
#include <CGAL/Polygon_mesh_processing/triangulate_faces.h>
#include <CGAL/Polygon_mesh_processing/border.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Polygon_mesh_processing/repair_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/stitch_borders.h>
#include <CGAL/Polygon_mesh_processing/connected_components.h>
#include <CGAL/boost/graph/helpers.h>
#include <CGAL/IO/polygon_soup_io.h>

#include "GoZ_Mesh.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <cstdlib>
#include <unordered_set>
#include <cstring>
#include <cmath>
#include <algorithm>

#ifdef _WIN32
#include <Windows.h>
#endif

typedef CGAL::Exact_predicates_inexact_constructions_kernel Kernel;
typedef Kernel::Point_3                                          Point;
typedef Kernel::Vector_3                                         Vector;
typedef CGAL::Surface_mesh<Point>                                Mesh;

namespace PMP = CGAL::Polygon_mesh_processing;

static void write_progress(float value)
{
    std::ofstream pf("progress.txt");
    if (pf.is_open())
    {
        pf << static_cast<int>(value * 100.0f) << std::endl;
    }
}

static bool g_pause_on_exit = false;
static void pause_if_needed()
{
    if (!g_pause_on_exit) return;
    std::cout << std::endl;
    std::cout << "===============================================" << std::endl;
    std::cout << " ZMeshMend CGAL finished. Press Enter to close." << std::endl;
    std::cout << "===============================================" << std::endl;
    std::cout.flush();
    std::cin.get();
}

// ============================================================================
// 边界平滑：仅对开放边界环及其向内 N 圈邻域做位移平滑。
//   Ring 0 (边界) ── Chaikin 切线方向平滑，权重 1.0
//   Ring 1..N    ── Laplacian 邻域平均，权重 0.5^ring
// 平滑位移在叠加后做法线切平面投影，仅允许沿表面滑动，保持洞口体积。
// 不增删顶点/面，PolyGroup 完全保留。
// ============================================================================

// 提取所有开放边界环（每环只保留一个 seed halfedge）。
static std::vector<Mesh::Halfedge_index>
extract_border_seeds(const Mesh& mesh)
{
    std::vector<Mesh::Halfedge_index> all;
    PMP::extract_boundary_cycles(mesh, std::back_inserter(all));

    std::vector<Mesh::Halfedge_index> seeds;
    for (auto h : all)
    {
        if (!CGAL::is_border(h, mesh))
            continue;

        bool dup = false;
        for (auto e : seeds)
        {
            Mesh::Halfedge_index cur = e;
            do {
                if (cur == h) { dup = true; break; }
                cur = mesh.next(cur);
            } while (cur != e);
            if (dup) break;
        }
        if (!dup) seeds.push_back(h);
    }
    return seeds;
}

// 沿边界环收集顶点序列（按 next() 方向）。
static std::vector<Mesh::Vertex_index>
collect_border_loop(const Mesh& mesh, Mesh::Halfedge_index seed)
{
    std::vector<Mesh::Vertex_index> loop;
    Mesh::Halfedge_index h = seed;
    do {
        loop.push_back(mesh.target(h));
        h = mesh.next(h);
    } while (h != seed);
    return loop;
}

// 从已有顶点集合 BFS 扩展一圈（仅取尚未访问过的邻居）。
static std::vector<Mesh::Vertex_index>
expand_one_ring(const Mesh& mesh,
                const std::vector<Mesh::Vertex_index>& src,
                std::unordered_set<std::size_t>& visited)
{
    std::vector<Mesh::Vertex_index> next;
    for (auto v : src)
    {
        Mesh::Halfedge_index h0 = mesh.halfedge(v);
        if (h0 == Mesh::null_halfedge()) continue;
        Mesh::Halfedge_index h = h0;
        do {
            Mesh::Vertex_index nb = mesh.source(h);
            if (visited.insert(static_cast<std::size_t>(nb)).second)
                next.push_back(nb);
            h = mesh.next(mesh.opposite(h));
        } while (h != h0);
    }
    return next;
}

// 计算顶点处邻接面的平均法线（面积加权）。
static Vector
vertex_normal(const Mesh& mesh, Mesh::Vertex_index v)
{
    Vector n(0.0, 0.0, 0.0);
    Mesh::Halfedge_index h0 = mesh.halfedge(v);
    if (h0 == Mesh::null_halfedge()) return n;
    Mesh::Halfedge_index h = h0;
    do {
        Mesh::Face_index f = mesh.face(h);
        if (f != Mesh::null_face())
        {
            auto a = mesh.point(mesh.source(h));
            auto b = mesh.point(mesh.target(h));
            auto c = mesh.point(mesh.target(mesh.next(h)));
            Vector fn = CGAL::cross_product(b - a, c - a);
            n = n + fn;
        }
        h = mesh.next(mesh.opposite(h));
    } while (h != h0);

    double len = std::sqrt(n.squared_length());
    if (len > 1e-20) n = n / len;
    return n;
}

// Chaikin 变体：Vi' = 0.25*V_{i-1} + 0.5*Vi + 0.25*V_{i+1}（闭合环）。
// 迭代 iterations 次。
static std::vector<Point>
chaikin_smooth_loop(const std::vector<Point>& pts, int iterations)
{
    std::vector<Point> cur = pts;
    const std::size_t n = cur.size();
    if (n < 3) return cur;

    std::vector<Point> nxt(n);
    for (int it = 0; it < iterations; ++it)
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            const Point& pp = cur[(i + n - 1) % n];
            const Point& pi = cur[i];
            const Point& pn = cur[(i + 1) % n];
            double x = 0.25 * pp.x() + 0.5 * pi.x() + 0.25 * pn.x();
            double y = 0.25 * pp.y() + 0.5 * pi.y() + 0.25 * pn.y();
            double z = 0.25 * pp.z() + 0.5 * pi.z() + 0.25 * pn.z();
            nxt[i] = Point(x, y, z);
        }
        cur.swap(nxt);
    }
    return cur;
}

// Laplacian 平滑：对一组顶点取邻域平均位置（一次迭代）。
// 使用当前 mesh 中的位置作为邻居参考，不修改 mesh。
static std::vector<Point>
laplacian_smooth_ring(const Mesh& mesh,
                      const std::vector<Mesh::Vertex_index>& ring,
                      int iterations)
{
    std::vector<Point> cur(ring.size());
    for (std::size_t i = 0; i < ring.size(); ++i)
        cur[i] = mesh.point(ring[i]);

    std::vector<Point> nxt = cur;
    for (int it = 0; it < iterations; ++it)
    {
        for (std::size_t i = 0; i < ring.size(); ++i)
        {
            Mesh::Vertex_index v = ring[i];
            Mesh::Halfedge_index h0 = mesh.halfedge(v);
            if (h0 == Mesh::null_halfedge()) { nxt[i] = cur[i]; continue; }

            double sx = 0, sy = 0, sz = 0;
            int cnt = 0;
            Mesh::Halfedge_index h = h0;
            do {
                const Point& p = mesh.point(mesh.source(h));
                sx += p.x(); sy += p.y(); sz += p.z();
                ++cnt;
                h = mesh.next(mesh.opposite(h));
            } while (h != h0);

            if (cnt > 0)
                nxt[i] = Point(sx / cnt, sy / cnt, sz / cnt);
            else
                nxt[i] = cur[i];
        }
        cur.swap(nxt);
    }
    return cur;
}

// 沿法线方向投影到 original 所在的切平面，消除法线分量偏移。
static Point
project_to_tangent_plane(const Point& displaced,
                         const Point& original,
                         const Vector& normal)
{
    Vector off = displaced - original;
    double d = off * normal;
    return Point(displaced.x() - d * normal.x(),
                 displaced.y() - d * normal.y(),
                 displaced.z() - d * normal.z());
}

// 主入口：平滑所有开放边界环及向内 num_rings 圈邻域。
static void
smooth_open_borders(Mesh& mesh, int iterations, int num_rings)
{
    if (iterations <= 0) return;
    if (num_rings < 1) num_rings = 1;

    auto seeds = extract_border_seeds(mesh);
    if (seeds.empty())
    {
        std::cout << "Smooth: no open boundaries found." << std::endl;
        return;
    }

    std::cout << "Smooth: found " << seeds.size() << " border loop(s), "
              << "iterations=" << iterations << ", rings=" << num_rings << std::endl;

    std::size_t total_moved = 0;

    for (std::size_t li = 0; li < seeds.size(); ++li)
    {
        auto loop = collect_border_loop(mesh, seeds[li]);
        if (loop.size() < 3) continue;

        // 1. 收集每圈顶点（vector<ring> with ring[0] = border loop）
        std::vector<std::vector<Mesh::Vertex_index>> rings;
        rings.push_back(loop);

        std::unordered_set<std::size_t> visited;
        for (auto v : loop) visited.insert(static_cast<std::size_t>(v));

        for (int r = 1; r <= num_rings; ++r)
        {
            auto next = expand_one_ring(mesh, rings.back(), visited);
            if (next.empty()) break;
            rings.push_back(next);
        }

        // 2. 保存每圈的原始位置和法线
        std::vector<std::vector<Point>>  orig_rings(rings.size());
        std::vector<std::vector<Vector>> normal_rings(rings.size());
        for (std::size_t r = 0; r < rings.size(); ++r)
        {
            orig_rings[r].resize(rings[r].size());
            normal_rings[r].resize(rings[r].size());
            for (std::size_t i = 0; i < rings[r].size(); ++i)
            {
                orig_rings[r][i] = mesh.point(rings[r][i]);
                normal_rings[r][i] = vertex_normal(mesh, rings[r][i]);
            }
        }

        // 3. 计算每圈的平滑目标位置
        std::vector<std::vector<Point>> smoothed(rings.size());
        smoothed[0] = chaikin_smooth_loop(orig_rings[0], iterations);
        for (std::size_t r = 1; r < rings.size(); ++r)
            smoothed[r] = laplacian_smooth_ring(mesh, rings[r], iterations);

        // 4. 加权位移 + 切平面投影
        for (std::size_t r = 0; r < rings.size(); ++r)
        {
            double weight = std::pow(0.5, static_cast<double>(r));
            for (std::size_t i = 0; i < rings[r].size(); ++i)
            {
                const Point& orig = orig_rings[r][i];
                const Point& sm   = smoothed[r][i];
                Vector disp(sm.x() - orig.x(), sm.y() - orig.y(), sm.z() - orig.z());
                Point displaced(orig.x() + disp.x() * weight,
                                orig.y() + disp.y() * weight,
                                orig.z() + disp.z() * weight);
                Point projected = project_to_tangent_plane(displaced, orig, normal_rings[r][i]);
                mesh.point(rings[r][i]) = projected;
                ++total_moved;
            }
        }
    }

    std::cout << "Smooth: moved " << total_moved << " vertex position(s)." << std::endl;
}

static void
relax_wireframe(Mesh& mesh, int iterations, double factor)
{
    if (iterations <= 0) return;

    std::cout << "Relax: collecting boundary vertices..." << std::endl;

    std::set<Mesh::Vertex_index> boundary_set;
    for (auto h : mesh.halfedges())
    {
        if (CGAL::is_border(h, mesh))
        {
            boundary_set.insert(mesh.target(h));
        }
    }

    int total_vtx = (int)mesh.number_of_vertices();
    int boundary_count = (int)boundary_set.size();
    std::cout << "Relax: " << boundary_count << " boundary vertices constrained, "
              << (total_vtx - boundary_count) << " interior vertices free, "
              << "iterations=" << iterations << " factor=" << factor << std::endl;

    std::vector<Point> new_positions(total_vtx);
    std::vector<Vector> normals(total_vtx, Vector(0, 0, 0));

    for (int it = 0; it < iterations; ++it)
    {
        for (auto v : mesh.vertices())
        {
            normals[(size_t)v] = vertex_normal(mesh, v);
        }

        for (auto v : mesh.vertices())
        {
            if (boundary_set.count(v))
            {
                new_positions[(size_t)v] = mesh.point(v);
                continue;
            }

            Mesh::Halfedge_index h0 = mesh.halfedge(v);
            if (h0 == Mesh::null_halfedge())
            {
                new_positions[(size_t)v] = mesh.point(v);
                continue;
            }

            Vector laplacian(0, 0, 0);
            int neighbor_count = 0;
            Mesh::Halfedge_index h = h0;
            do {
                laplacian = laplacian + (mesh.point(mesh.target(h)) - CGAL::ORIGIN);
                ++neighbor_count;
                h = mesh.next(mesh.opposite(h));
            } while (h != h0);

            if (neighbor_count == 0)
            {
                new_positions[(size_t)v] = mesh.point(v);
                continue;
            }

            laplacian = laplacian / (double)neighbor_count;
            Point smoothed(laplacian.x(), laplacian.y(), laplacian.z());

            Vector disp = smoothed - mesh.point(v);

            const Vector& n = normals[(size_t)v];
            double dot = disp * n;
            Vector tangent_disp = (disp - n * dot) * factor;

            Point moved = mesh.point(v) + tangent_disp;
            new_positions[(size_t)v] = moved;
        }

        for (auto v : mesh.vertices())
        {
            mesh.point(v) = new_positions[(size_t)v];
        }
    }

    int moved_count = total_vtx - boundary_count;
    std::cout << "Relax: " << moved_count << " interior vertices relaxed, done." << std::endl;
}

// ============================================================================

static bool load_goz_to_cgal(GoZ_Mesh& goz, Mesh& mesh,
                              std::vector<int>& face_to_goz_face,
                              std::vector<int>& goz_face_to_cgal_offset)
{
    face_to_goz_face.clear();
    goz_face_to_cgal_offset.assign(goz.m_faceCount + 1, 0);

    int cgal_face_idx = 0;
    for (int fi = 0; fi < goz.m_faceCount; ++fi)
    {
        const int* idx = goz.m_vertexIndices.data() + fi * 4;
        int v0 = idx[0], v1 = idx[1], v2 = idx[2], v3 = idx[3];

        Mesh::Vertex_index a(v0), b(v1), c(v2);

        if (v3 < 0 || v3 == v2)
        {
            mesh.add_face(a, b, c);
            face_to_goz_face.push_back(fi);
            ++cgal_face_idx;
        }
        else
        {
            Mesh::Vertex_index d(v3);
            mesh.add_face(a, b, c);
            face_to_goz_face.push_back(fi);
            ++cgal_face_idx;
            mesh.add_face(a, c, d);
            face_to_goz_face.push_back(fi);
            ++cgal_face_idx;
        }

        goz_face_to_cgal_offset[fi + 1] = cgal_face_idx;
    }

    return true;
}

static void extract_cgal_vertices(const Mesh& mesh, std::vector<float>& out_verts)
{
    out_verts.clear();
    out_verts.reserve(mesh.number_of_vertices() * 3);
    std::map<Mesh::Vertex_index, int> vmap;
    int idx = 0;
    for (auto v : mesh.vertices())
    {
        Point p = mesh.point(v);
        out_verts.push_back(static_cast<float>(p.x()));
        out_verts.push_back(static_cast<float>(p.y()));
        out_verts.push_back(static_cast<float>(p.z()));
        vmap[v] = idx++;
    }
}

static void build_goz_vertex_map_and_faces(const Mesh& mesh,
                                              const std::unordered_set<Mesh::Face_index>& orig_faces,
                                              GoZ_Mesh& out_goz,
                                              std::map<Mesh::Vertex_index, int>& full_vmap)
{
    int out_vc = 0;
    for (auto v : mesh.vertices())
        full_vmap[v] = out_vc++;

    out_goz.m_vertexCount = out_vc;
    out_goz.m_vertices.resize(out_goz.m_vertexCount * 3);
    for (auto v : mesh.vertices())
    {
        Point p = mesh.point(v);
        int idx = full_vmap[v];
        out_goz.m_vertices[idx * 3 + 0] = static_cast<float>(p.x());
        out_goz.m_vertices[idx * 3 + 1] = static_cast<float>(p.y());
        out_goz.m_vertices[idx * 3 + 2] = static_cast<float>(p.z());
    }

    out_goz.m_faceType = GoZ_TAG_FACE3_LIST;
    out_goz.m_faceCount = static_cast<int>(mesh.number_of_faces());
    out_goz.m_vertexIndices.resize(out_goz.m_faceCount * 3);
    out_goz.m_groups.resize(out_goz.m_faceCount);
}

static void build_output_goz(GoZ_Mesh& in_goz, const Mesh& mesh,
                              const std::unordered_set<Mesh::Face_index>& orig_faces,
                              const std::vector<int>& face_to_goz_face,
                              GoZ_Mesh& out_goz)
{
    out_goz.clear();

    out_goz.m_name = !in_goz.m_name.empty() ? in_goz.m_name : "GoZMesh";
    out_goz.m_material = in_goz.m_material;
    out_goz.m_flags = in_goz.m_flags;
    out_goz.m_diffuseMap = in_goz.m_diffuseMap;
    out_goz.m_normalMap = in_goz.m_normalMap;
    out_goz.m_displacementMap = in_goz.m_displacementMap;
    out_goz.m_displacementScale = in_goz.m_displacementScale;

    int orig_vertex_count = in_goz.m_vertexCount;

    std::map<Mesh::Vertex_index, int> full_vmap;
    build_goz_vertex_map_and_faces(mesh, orig_faces, out_goz, full_vmap);

    short max_group = 0;
    if (!in_goz.m_groups.empty())
    {
        for (int fi = 0; fi < in_goz.m_faceCount; ++fi)
            if (in_goz.m_groups[fi] > max_group)
                max_group = in_goz.m_groups[fi];
    }
    short new_group = max_group + 1;

    int out_fi = 0;
    for (auto f : mesh.faces())
    {
        bool is_new_face = (orig_faces.find(f) == orig_faces.end());

        std::vector<Mesh::Vertex_index> verts;
        for (auto v : CGAL::vertices_around_face(mesh.halfedge(f), mesh))
            verts.push_back(v);

        if (verts.size() >= 3)
        {
            out_goz.m_vertexIndices[out_fi * 3 + 0] = full_vmap[verts[0]];
            out_goz.m_vertexIndices[out_fi * 3 + 1] = full_vmap[verts[1]];
            out_goz.m_vertexIndices[out_fi * 3 + 2] = full_vmap[verts[2]];

            if (is_new_face)
                out_goz.m_groups[out_fi] = new_group;
            else
                out_goz.m_groups[out_fi] = !in_goz.m_groups.empty() ? in_goz.m_groups[face_to_goz_face[out_fi]] : 1;

            ++out_fi;
        }
    }

    out_goz.m_mask.resize(out_goz.m_vertexCount);
    for (auto& pair : full_vmap)
    {
        Mesh::Vertex_index v = pair.first;
        int goz_idx = pair.second;
        if (static_cast<int>(v) < orig_vertex_count && !in_goz.m_mask.empty())
            out_goz.m_mask[goz_idx] = in_goz.m_mask[static_cast<int>(v)];
        else
            out_goz.m_mask[goz_idx] = 0xFFFF;
    }

    if (!in_goz.m_mrgb.empty())
    {
        out_goz.m_mrgb.resize(out_goz.m_vertexCount);
        for (int vi = 0; vi < out_goz.m_vertexCount; ++vi)
            out_goz.m_mrgb[vi] = 0xFFFFFFFF;
    }
}

static void write_fill_only_obj(const Mesh& mesh,
                                 const std::unordered_set<Mesh::Face_index>& orig_faces,
                                 const std::string& fill_path)
{
    std::vector<Mesh::Face_index> fill_faces;
    for (auto f : mesh.faces())
    {
        if (orig_faces.find(f) == orig_faces.end())
            fill_faces.push_back(f);
    }

    std::map<Mesh::Vertex_index, int> used_vmap;
    int next_idx = 1;
    for (auto f : fill_faces)
    {
        for (auto v : CGAL::vertices_around_face(mesh.halfedge(f), mesh))
        {
            if (used_vmap.find(v) == used_vmap.end())
                used_vmap[v] = next_idx++;
        }
    }

    std::ofstream out(fill_path);
    if (!out)
    {
        std::cerr << "WARNING: Cannot write fill OBJ: " << fill_path << std::endl;
        return;
    }

    out << std::setprecision(17);

    out << "# ZMeshMend fill-only patch\n";
    out << "# vertices: " << used_vmap.size()
        << " faces: " << fill_faces.size() << "\n";

    std::vector<std::pair<int, Mesh::Vertex_index>> sorted_verts;
    for (auto& kv : used_vmap)
        sorted_verts.push_back({kv.second, kv.first});
    std::sort(sorted_verts.begin(), sorted_verts.end(),
              [](const std::pair<int, Mesh::Vertex_index>& a,
                 const std::pair<int, Mesh::Vertex_index>& b) { return a.first < b.first; });
    for (auto& pair : sorted_verts)
    {
        Point p = mesh.point(pair.second);
        out << "v " << p.x() << " " << p.y() << " " << p.z() << "\n";
    }

    out << "g ZMeshMend_Fill\n";
    for (auto f : fill_faces)
    {
        std::vector<Mesh::Vertex_index> verts;
        for (auto v : CGAL::vertices_around_face(mesh.halfedge(f), mesh))
            verts.push_back(v);
        if (verts.size() < 3) continue;
        out << "f";
        for (auto v : verts)
            out << " " << used_vmap[v];
        out << "\n";
    }

    std::cout << "Fill-only OBJ: " << used_vmap.size()
              << " vertices, " << fill_faces.size()
              << " faces -> " << fill_path << std::endl;
}

static void write_fill_only_goz(GoZ_Mesh& full_goz, const Mesh& mesh,
                                 const std::unordered_set<Mesh::Face_index>& orig_faces,
                                 const std::string& fill_path)
{
    GoZ_Mesh fill_goz;
    fill_goz.m_name = full_goz.m_name;
    fill_goz.m_name += "_FillOnly";
    fill_goz.m_flags = full_goz.m_flags;
    fill_goz.m_faceType = GoZ_TAG_FACE3_LIST;

    fill_goz.m_vertexCount = full_goz.m_vertexCount;
    fill_goz.m_vertices = full_goz.m_vertices;

    std::vector<int> fill_faces;
    int out_fi = 0;
    for (auto f : mesh.faces())
    {
        if (orig_faces.find(f) == orig_faces.end())
            fill_faces.push_back(out_fi);
        ++out_fi;
    }

    if (fill_faces.empty())
    {
        fill_goz.m_faceCount = 0;
        fill_goz.m_vertexIndices.resize(3);
        fill_goz.m_vertexIndices[0] = 0;
        fill_goz.m_vertexIndices[1] = 0;
        fill_goz.m_vertexIndices[2] = 0;
    }
    else
    {
        fill_goz.m_faceCount = static_cast<int>(fill_faces.size());
        fill_goz.m_vertexIndices.resize(fill_goz.m_faceCount * 3);
        for (std::size_t i = 0; i < fill_faces.size(); ++i)
        {
            int src_fi = fill_faces[i];
            fill_goz.m_vertexIndices[i * 3 + 0] = full_goz.m_vertexIndices[src_fi * 3 + 0];
            fill_goz.m_vertexIndices[i * 3 + 1] = full_goz.m_vertexIndices[src_fi * 3 + 1];
            fill_goz.m_vertexIndices[i * 3 + 2] = full_goz.m_vertexIndices[src_fi * 3 + 2];
        }
    }

    fill_goz.writeMesh(fill_path.c_str());
    std::cout << "Fill-only GoZ: " << fill_goz.m_vertexCount
              << " vertices, " << fill_goz.m_faceCount
              << " faces -> " << fill_path << std::endl;
}

static void build_goz_from_cgal(const Mesh& mesh,
                                 const std::unordered_set<Mesh::Face_index>& orig_faces,
                                 GoZ_Mesh& out_goz,
                                 bool from_obj = true)
{
    out_goz.clear();

    out_goz.m_name = "GoZMesh";
    out_goz.m_material = "DefaultMaterial";
    out_goz.m_faceType = GoZ_TAG_FACE3_LIST;

    std::map<Mesh::Vertex_index, int> full_vmap;
    build_goz_vertex_map_and_faces(mesh, orig_faces, out_goz, full_vmap);

    int out_fi = 0;
    for (auto f : mesh.faces())
    {
        bool is_new_face = (orig_faces.find(f) == orig_faces.end());

        std::vector<Mesh::Vertex_index> verts;
        for (auto v : CGAL::vertices_around_face(mesh.halfedge(f), mesh))
            verts.push_back(v);

        if (verts.size() >= 3)
        {
            out_goz.m_vertexIndices[out_fi * 3 + 0] = full_vmap[verts[0]];
            out_goz.m_vertexIndices[out_fi * 3 + 1] = full_vmap[verts[1]];
            out_goz.m_vertexIndices[out_fi * 3 + 2] = full_vmap[verts[2]];

            out_goz.m_groups[out_fi] = is_new_face ? 2 : 1;
            ++out_fi;
        }
    }

    out_goz.m_mask.resize(out_goz.m_vertexCount);
    for (int vi = 0; vi < out_goz.m_vertexCount; ++vi)
        out_goz.m_mask[vi] = 0xFFFF;
}

int main(int argc, char* argv[])
{
    bool zero_arg_mode = false;
    std::string in_path;
    std::string out_path;
    std::string fill_path;
    std::string debug_obj;
    bool write_fill_only = false;
    double opt_min_frac  = 0.0;
    int    opt_min_faces = 0;
    bool   opt_full_obj  = false;
    bool   opt_smooth_border     = false;
    int    opt_smooth_iterations = 2;
    int    opt_smooth_rings      = 3;
    bool   opt_smooth_only       = false;
    bool   opt_relax_wireframe   = false;
    int    opt_relax_iterations  = 3;
    double opt_relax_factor      = 0.3;

    if (argc < 3)
    {
        zero_arg_mode = true;
        opt_full_obj  = true;
#ifdef _WIN32
        {
            char self[MAX_PATH];
            DWORD n = GetModuleFileNameA(NULL, self, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
            {
                char* slash = strrchr(self, '\\');
                if (slash) { *slash = '\0'; SetCurrentDirectoryA(self); }
            }
        }
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
#endif
        {
            FILE* s = fopen("zmeshmend_startup.log", "w");
            if (s) { fprintf(s, "started\n"); fclose(s); }
        }
        //ZCloseHoles 模式：从 zmeshmend_config.txt 读取所有内容，
        //输入 = zmeshmend_export.obj，输出 = zmeshmend_import.obj。
        in_path  = "zmeshmend_export.obj";
        out_path = "zmeshmend_import.obj";

        FILE* cf = fopen("zmeshmend_config.txt", "r");
        if (cf)
        {
            char line[256];
            while (fgets(line, sizeof(line), cf))
            {
                int vi = 0; float vf = 0.0f; double vd = 0.0;
                if (sscanf(line, "maskSharpenPasses=%d", &vi) == 1)      { /*跳过*/ }
                else if (sscanf(line, "maskGrowRings=%d", &vi) == 1)      { /*跳过*/ }
                else if (sscanf(line, "removeSmallFragments=%d", &vi) == 1)
                {
                    if (vi) { opt_min_frac = 0.01; opt_min_faces = 50; }
                }
                else if (sscanf(line, "fragmentMinFraction=%f", &vf) == 1) { opt_min_frac = vf; }
                else if (sscanf(line, "fragmentMinFaces=%d", &vi) == 1)   { opt_min_faces = vi; }
                else if (sscanf(line, "smoothBorder=%d", &vi) == 1)
                {
                    if (vi) { opt_smooth_border = true; opt_smooth_only = true; }
                }
                else if (sscanf(line, "smoothIterations=%d", &vi) == 1)   { opt_smooth_iterations = vi; }
                else if (sscanf(line, "smoothRings=%d", &vi) == 1)        { opt_smooth_rings = vi; }
                else if (sscanf(line, "relaxWireframe=%d", &vi) == 1)
                {
                    if (vi) { opt_relax_wireframe = true; }
                }
                else if (sscanf(line, "relaxIterations=%d", &vi) == 1)    { opt_relax_iterations = vi; }
                else if (sscanf(line, "relaxFactor=%lf", &vd) == 1)      { opt_relax_factor = vd; }
            }
            fclose(cf);

            if (opt_min_frac <= 0.0 && opt_min_faces <= 0)
                opt_min_frac = opt_min_faces = 0; //两个参数都必须设置才能启用
        }
        //零参数模式：始终暂停，以便用户可以看到控制台输出。
        g_pause_on_exit = true;
    }
    else
    {
    in_path = argv[1];
    out_path = argv[2];
    for (int i = 3; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--min-frac" && i + 1 < argc)
        {
            opt_min_frac = std::atof(argv[++i]);
        }
        else if (a == "--min-faces" && i + 1 < argc)
        {
            opt_min_faces = std::atoi(argv[++i]);
        }
        else if (a == "--pause")
        {
            g_pause_on_exit = true;
        }
        else if (a == "--full-obj")
        {
            opt_full_obj = true;
        }
        else if (a == "--smooth-border")
        {
            opt_smooth_border = true;
        }
        else if (a == "--smooth-iterations" && i + 1 < argc)
        {
            opt_smooth_iterations = std::atoi(argv[++i]);
        }
        else if (a == "--smooth-rings" && i + 1 < argc)
        {
            opt_smooth_rings = std::atoi(argv[++i]);
        }
        else if (a == "--smooth-only")
        {
            opt_smooth_only = true;
        }
        else if (a == "--relax-wireframe")
        {
            opt_relax_wireframe = true;
        }
        else if (a == "--relax-iterations" && i + 1 < argc)
        {
            opt_relax_iterations = std::atoi(argv[++i]);
        }
        else if (a == "--relax-factor" && i + 1 < argc)
        {
            opt_relax_factor = std::atof(argv[++i]);
        }
        else if (!a.empty() && a[0] != '-')
        {
            // out_path 之后的第一个位置参数 -> fill_path；第二个 -> debug_obj
            if (!write_fill_only) { fill_path = a; write_fill_only = true; }
            else if (debug_obj.empty()) { debug_obj = a; }
        }
    }
    }

    auto ends_with = [](const std::string& s, const std::string& suf) {
        if (s.size() < suf.size()) return false;
        std::string a = s.substr(s.size() - suf.size());
        std::string b = suf;
        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
        return a == b;
    };
    const bool out_is_obj_patch = ends_with(out_path, ".obj");

    write_progress(0.05f);

    Mesh mesh;
    GoZ_Mesh in_goz;
    std::vector<int> face_to_goz_face;
    bool from_goz = false;

    if (in_goz.readMesh(in_path.c_str()))
    {
        from_goz = true;
        std::cout << "GoZ Input: " << in_goz.m_vertexCount << " vertices, "
                  << in_goz.m_faceCount << " faces" << std::endl;
        std::cout << "Has PolyGroups: " << (!in_goz.m_groups.empty() ? "yes" : "no") << std::endl;
        std::cout << "Has Mask:       " << (!in_goz.m_mask.empty() ? "yes" : "no") << std::endl;

        for (int vi = 0; vi < in_goz.m_vertexCount; ++vi)
        {
            float x = in_goz.m_vertices[vi * 3 + 0];
            float y = in_goz.m_vertices[vi * 3 + 1];
            float z = in_goz.m_vertices[vi * 3 + 2];
            mesh.add_vertex(Point(x, y, z));
        }

        std::vector<int> goz_face_to_cgal_offset;
        load_goz_to_cgal(in_goz, mesh, face_to_goz_face, goz_face_to_cgal_offset);
    }
    else if (zero_arg_mode)
    {
        //ZScript 路径：ZBrush Tool:Export 写入的是未缝合的 OBJ。
        //使用 read_OBJ + stitch_borders 合并重复顶点，
        //保留四边形，使原始面保持其原始形式。
        std::ifstream zin(in_path);
        if (!zin || !CGAL::IO::read_OBJ(zin, mesh))
        {
            std::cerr << "ERROR: Cannot read OBJ: " << in_path << std::endl;
            pause_if_needed();
            return 1;
        }
        std::cout << "OBJ Input: " << mesh.number_of_vertices() << " vertices, "
                  << mesh.number_of_faces() << " faces" << std::endl;

        PMP::stitch_borders(mesh);
        std::cout << "After stitch: " << mesh.number_of_vertices() << " vertices, "
                  << mesh.number_of_faces() << " faces" << std::endl;
    }
    else
    {
        //Python / CLI 路径：原始 read_OBJ，保留四边形和结构。
        std::ifstream in(in_path);
        if (!in || !CGAL::IO::read_OBJ(in, mesh))
        {
            std::cerr << "ERROR: Cannot read input as OBJ: " << in_path << std::endl;
            pause_if_needed();
            return 1;
        }
        std::cout << "OBJ Input: " << mesh.number_of_vertices() << " vertices, "
                  << mesh.number_of_faces() << " faces" << std::endl;
        if (!opt_smooth_only && !opt_relax_wireframe && !CGAL::is_triangle_mesh(mesh))
        {
            std::cout << "Triangulating non-triangle faces..." << std::endl;
            PMP::triangulate_faces(mesh);
        }
    }

    write_progress(0.10f);

    std::cout << "CGAL mesh: " << mesh.number_of_vertices() << " vertices, "
              << mesh.number_of_faces() << " faces" << std::endl;

    if (opt_smooth_border)
    {
        smooth_open_borders(mesh, opt_smooth_iterations, opt_smooth_rings);
    }

    if (opt_smooth_only)
    {
        std::ofstream out(out_path);
        if (!out)
        {
            std::cerr << "ERROR: Cannot open OBJ for write: " << out_path << std::endl;
            pause_if_needed();
            return 1;
        }
        out << "# ZMeshMend smooth open edges\n";
        std::map<Mesh::Vertex_index, std::size_t> vidx;
        std::size_t vcount = 0;
        for (auto v : mesh.vertices())
        {
            const auto& p = mesh.point(v);
            out << "v " << p.x() << ' ' << p.y() << ' ' << p.z() << '\n';
            vidx[v] = ++vcount;
        }
        std::vector<std::string> orig_groups;
        {
            std::ifstream gs(in_path);
            std::string line, cur = "orig";
            while (std::getline(gs, line))
            {
                if (!line.empty() && line[0] == 'g' && line.size() > 1 && line[1] == ' ')
                    cur = line.substr(2);
                else if (!line.empty() && line[0] == 'f' && line.size() > 1 && line[1] == ' ')
                    orig_groups.push_back(cur);
            }
        }
        std::string last_g;
        for (auto f : mesh.faces())
        {
            size_t fidx = (size_t)f;
            std::string gname = (fidx < orig_groups.size()) ? orig_groups[fidx] : "orig";
            if (gname != last_g) { out << "g " << gname << '\n'; last_g = gname; }
            out << 'f';
            auto h0 = mesh.halfedge(f);
            auto h = h0;
            do { out << ' ' << vidx[mesh.target(h)]; h = mesh.next(h); } while (h != h0);
            out << '\n';
        }
        std::cout << "Smooth only: " << vcount << " vertices, "
                  << mesh.number_of_faces() << " faces -> " << out_path << std::endl;
        write_progress(1.0f);
        std::cout << "SUCCESS (smooth open edges)" << std::endl;
        pause_if_needed();
        return 0;
    }

    if (opt_relax_wireframe)
    {
        relax_wireframe(mesh, opt_relax_iterations, opt_relax_factor);

        std::ofstream out(out_path);
        if (!out)
        {
            std::cerr << "ERROR: Cannot open OBJ for write: " << out_path << std::endl;
            pause_if_needed();
            return 1;
        }
        out << "# ZMeshMend relax wireframe\n";
        out << std::fixed << std::setprecision(6);
        std::map<Mesh::Vertex_index, std::size_t> vidx;
        std::size_t vcount = 0;
        for (auto v : mesh.vertices())
        {
            const auto& p = mesh.point(v);
            out << "v " << p.x() << ' ' << p.y() << ' ' << p.z() << '\n';
            vidx[v] = ++vcount;
        }
        std::vector<std::string> orig_groups;
        {
            std::ifstream gs(in_path);
            std::string line, cur = "orig";
            while (std::getline(gs, line))
            {
                if (!line.empty() && line[0] == 'g' && line.size() > 1 && line[1] == ' ')
                    cur = line.substr(2);
                else if (!line.empty() && line[0] == 'f' && line.size() > 1 && line[1] == ' ')
                    orig_groups.push_back(cur);
            }
        }
        std::string last_g;
        for (auto f : mesh.faces())
        {
            size_t fidx = (size_t)f;
            std::string gname = (fidx < orig_groups.size()) ? orig_groups[fidx] : "orig";
            if (gname != last_g) { out << "g " << gname << '\n'; last_g = gname; }
            out << 'f';
            auto h0 = mesh.halfedge(f);
            auto h = h0;
            do { out << ' ' << vidx[mesh.target(h)]; h = mesh.next(h); } while (h != h0);
            out << '\n';
        }
        std::cout << "Relax wireframe: " << vcount << " vertices, "
                  << mesh.number_of_faces() << " faces -> " << out_path << std::endl;
        write_progress(1.0f);
        std::cout << "SUCCESS (relax wireframe)" << std::endl;
        pause_if_needed();
        return 0;
    }

    bool has_boundary = !CGAL::is_closed(mesh);
    std::cout << "Has border edges: " << (has_boundary ? "yes" : "no") << std::endl;

    write_progress(0.15f);

    if (!has_boundary)
    {
        std::cout << "Mesh is watertight - no holes to fill." << std::endl;
        if (out_is_obj_patch)
        {
            if (opt_full_obj)
            {
                // 完整网格 OBJ - 所有面都是原始面（group_1）
                std::ofstream out(out_path);
                if (!out)
                {
                    std::cerr << "ERROR: Cannot open OBJ for write: " << out_path << std::endl;
                    pause_if_needed();
                    return 1;
                }
                out << "# ZMeshMend full mesh (watertight, all original)\n";
                std::map<Mesh::Vertex_index, std::size_t> vidx;
                std::size_t vcount = 0;
                for (auto v : mesh.vertices())
                {
                    const auto& p = mesh.point(v);
                    out << "v " << p.x() << ' ' << p.y() << ' ' << p.z() << '\n';
                    vidx[v] = ++vcount;
                }
                //解析原始 OBJ 以通过 g 行保留 PolyGroups。
                {
                    std::ifstream gs(in_path);
                    std::string line, cur = "orig", last_g;
                    size_t fi = 0;
                    while (std::getline(gs, line))
                    {
                        if (!line.empty() && line[0] == 'g' && line.size() > 1 && line[1] == ' ')
                            cur = line.substr(2);
                        else if (!line.empty() && line[0] == 'f' && line.size() > 1 && line[1] == ' ')
                        {
                            Mesh::Face_index mf(fi);
                            if (fi == 0 || cur != last_g) { out << "g " << cur << '\n'; last_g = cur; }
                            out << 'f';
                            auto h0 = mesh.halfedge(mf);
                            auto h = h0;
                            do { out << ' ' << vidx[mesh.target(h)]; h = mesh.next(h); } while (h != h0);
                            out << '\n';
                            ++fi;
                        }
                    }
                }
                std::cout << "Output: " << out_path << " (full mesh OBJ, watertight)" << std::endl;
                std::cout << "Vertices: " << vcount << ", Faces: " << mesh.number_of_faces() << std::endl;
                write_progress(1.0f);
                std::cout << "SUCCESS (watertight, full OBJ)" << std::endl;
                pause_if_needed();
                return 0;
            }
            std::ofstream out(out_path);
            if (out)
            {
                out << "# ZMeshMend fill-only patch\n";
                out << "# vertices: 0 faces: 0\n";
            }
            if (write_fill_only)
            {
                std::ofstream f(fill_path);
                if (f)
                {
                    f << "# ZMeshMend fill-only patch\n";
                    f << "# vertices: 0 faces: 0\n";
                }
            }
            write_progress(1.0f);
            std::cout << "SUCCESS (watertight, no fill needed)" << std::endl;
            pause_if_needed();
            return 0;
        }
        if (from_goz)
        {
            GoZ_Mesh out_goz;
            out_goz.m_name = in_goz.m_name;
            out_goz.m_faceType = in_goz.m_faceType;
            out_goz.m_vertexCount = in_goz.m_vertexCount;
            out_goz.m_vertices = in_goz.m_vertices;
            out_goz.m_faceCount = in_goz.m_faceCount;
            out_goz.m_vertexIndices = in_goz.m_vertexIndices;
            if (!in_goz.m_groups.empty())
            {
                out_goz.m_groups = in_goz.m_groups;
            }
            if (!in_goz.m_mask.empty())
            {
                out_goz.m_mask = in_goz.m_mask;
            }
            out_goz.writeMesh(out_path.c_str());
            if (write_fill_only)
                out_goz.writeMesh(fill_path.c_str());
        }
        else
        {
            std::unordered_set<Mesh::Face_index> all_faces;
            for (auto f : mesh.faces())
                all_faces.insert(f);
            GoZ_Mesh out_goz;
            build_goz_from_cgal(mesh, all_faces, out_goz);
            out_goz.writeMesh(out_path.c_str());
            if (write_fill_only)
                out_goz.writeMesh(fill_path.c_str());
        }
        write_progress(1.0f);
        std::cout << "SUCCESS (watertight, full mesh re-emitted)" << std::endl;
        pause_if_needed();
        return 0;
    }

    std::unordered_set<Mesh::Face_index> original_faces;
    for (auto f : mesh.faces())
        original_faces.insert(f);

    std::vector<Mesh::Halfedge_index> border_halfedges;
    PMP::extract_boundary_cycles(mesh, std::back_inserter(border_halfedges));

    std::vector<Mesh::Halfedge_index> hole_seeds;
    {
        for (auto h : border_halfedges)
        {
            if (!CGAL::is_border(h, mesh))
                continue;

            bool duplicate = false;
            for (auto existing : hole_seeds)
            {
                Mesh::Halfedge_index cur = existing;
                Mesh::Halfedge_index end = existing;
                do {
                    if (cur == h) { duplicate = true; break; }
                    cur = mesh.next(cur);
                } while (cur != end);
                if (duplicate) break;
            }

            if (!duplicate)
                hole_seeds.push_back(h);
        }
    }

    std::cout << "Found " << hole_seeds.size() << " hole(s)" << std::endl;

    write_progress(0.25f);

    unsigned int filled = 0;
    unsigned int failed = 0;
    Mesh::size_type total_added = 0;

    for (std::size_t i = 0; i < hole_seeds.size(); ++i)
    {
        Mesh::Halfedge_index h = hole_seeds[i];

        unsigned int edge_count = 0;
        {
            Mesh::Halfedge_index cur = h;
            Mesh::Halfedge_index end = h;
            do {
                ++edge_count;
                cur = mesh.next(cur);
            } while (cur != end);
        }

        std::cout << "  Hole " << (i + 1) << " (" << edge_count << " edges): ";

        Mesh::size_type fc_before = mesh.number_of_faces();

        auto [ok, fo, vo] = PMP::triangulate_refine_and_fair_hole(mesh, h);

        Mesh::size_type fc_after = mesh.number_of_faces();
        int added = static_cast<int>(fc_after - fc_before);

        if (ok)
        {
            std::cout << "OK (refine+fair)";
            if (added > 0)
                std::cout << ", " << added << " faces added";
            std::cout << std::endl;
            ++filled;
            total_added += added;
        }
        else
        {
            std::cout << "FAILED" << std::endl;
            ++failed;
        }

        float progress = 0.25f + 0.55f * ((float)(i + 1) / (float)hole_seeds.size());
        write_progress(progress);
    }

    std::cout << "SUMMARY: filled=" << filled << " failed=" << failed
              << " faces_added=" << total_added << std::endl;

    write_progress(0.85f);

    // ---------- 移除小型不连通碎片 ----------
    if (opt_min_frac > 0.0 || opt_min_faces > 0)
    {
        std::size_t total = mesh.number_of_faces();
        std::size_t threshold_frac = static_cast<std::size_t>(opt_min_frac * static_cast<double>(total));
        std::size_t threshold_abs  = static_cast<std::size_t>(opt_min_faces);
        std::size_t threshold = (std::max)(threshold_frac, threshold_abs);

        if (threshold > 1 && total > 1)
        {
            typedef Mesh::Property_map<Mesh::Face_index, std::size_t> FCCMap;
            FCCMap fccmap = mesh.add_property_map<Mesh::Face_index, std::size_t>("f:cc", 0).first;
            std::size_t num_cc = PMP::connected_components(mesh, fccmap);

            if (num_cc > 1)
            {
                std::vector<std::size_t> cc_size(num_cc, 0);
                for (auto f : mesh.faces())
                    ++cc_size[fccmap[f]];

                std::size_t removed_components = 0;
                std::size_t removed_faces = 0;
                std::vector<std::size_t> small_ccs;
                for (std::size_t i = 0; i < num_cc; ++i)
                {
                    if (cc_size[i] < threshold)
                    {
                        small_ccs.push_back(i);
                        ++removed_components;
                        removed_faces += cc_size[i];
                    }
                }

                if (!small_ccs.empty())
                {
                    // 刷新 original_faces：下面被移除的面不应出现在
                    // 序列化输出时的填充面集合或原始面集合中。
                    std::unordered_set<Mesh::Face_index> removed_face_set;
                    for (auto f : mesh.faces())
                    {
                        std::size_t cc = fccmap[f];
                        for (std::size_t s : small_ccs)
                        {
                            if (cc == s) { removed_face_set.insert(f); break; }
                        }
                    }
                    PMP::remove_connected_components(mesh, small_ccs, fccmap);
                    PMP::remove_isolated_vertices(mesh);
                    mesh.collect_garbage();

                    // 从 original_faces 追踪器中移除已删除的面
                    for (auto f : removed_face_set)
                        original_faces.erase(f);
                }

                std::cout << "Fragments: components=" << num_cc
                          << " threshold=" << threshold
                          << " removed=" << removed_components
                          << " (" << removed_faces << " faces)" << std::endl;
            }
            else
            {
                std::cout << "Fragments: single component, nothing to remove" << std::endl;
            }

            mesh.remove_property_map(fccmap);
        }
    }

    write_progress(0.88f);
    // ---------- 碎片移除结束 ----------

    if (out_is_obj_patch)
    {
        if (opt_full_obj)
        {
            // 完整网格 OBJ，使用 PolyGroup 作为 group 行，
            // 以便 ZBrush 中的 Tool:Import 重建 PolyGroups（原始=1，填充=2）。
            std::ofstream out(out_path);
            if (!out)
            {
                std::cerr << "ERROR: Cannot open OBJ for write: " << out_path << std::endl;
                pause_if_needed();
                return 1;
            }
            out << "# ZMeshMend full mesh (PolyGroup preserved + fill)\n";

            // 重建顶点索引映射（跳过已移除的顶点/面）。
            std::map<Mesh::Vertex_index, std::size_t> vidx;
            std::size_t vcount = 0;
            for (auto v : mesh.vertices())
            {
                const auto& p = mesh.point(v);
                out << "v " << p.x() << ' ' << p.y() << ' ' << p.z() << '\n';
                vidx[v] = ++vcount;
            }

            //解析原始 OBJ 的 g 行：经过 read_OBJ + stitch_borders 后，
            //CGAL 面索引 i == OBJ 中第 i 条面行（从 0 开始）。
            std::vector<std::string> orig_groups;
            {
                std::ifstream gs(in_path);
                std::string line, cur = "orig";
                while (std::getline(gs, line))
                {
                    if (!line.empty() && line[0] == 'g' && line.size() > 1 && line[1] == ' ')
                        cur = line.substr(2);
                    else if (!line.empty() && line[0] == 'f' && line.size() > 1 && line[1] == ' ')
                        orig_groups.push_back(cur);
                }
            }

            //单独收集填充面。
            std::vector<Mesh::Face_index> fill_faces;
            for (auto f : mesh.faces())
                if (original_faces.find(f) == original_faces.end())
                    fill_faces.push_back(f);

            //输出原始面：按原始 g-group 名称分组。
            {
                std::string last_g;
                for (auto f : mesh.faces())
                {
                    if (original_faces.find(f) == original_faces.end()) continue;
                    size_t fidx = (size_t)f;
                    std::string gname = (fidx < orig_groups.size()) ? orig_groups[fidx] : "orig";
                    if (gname != last_g) { out << "g " << gname << '\n'; last_g = gname; }
                    out << 'f';
                    auto h0 = mesh.halfedge(f);
                    auto h = h0;
                    do { out << ' ' << vidx[mesh.target(h)]; h = mesh.next(h); } while (h != h0);
                    out << '\n';
                }
            }

            //在单独的 group 下输出填充面。
            if (!fill_faces.empty())
            {
                out << "g ZMeshMend_Fill\n";
                for (auto f : fill_faces)
                {
                    out << 'f';
                    auto h0 = mesh.halfedge(f);
                    auto h = h0;
                    do { out << ' ' << vidx[mesh.target(h)]; h = mesh.next(h); } while (h != h0);
                    out << '\n';
                }
            }

            std::cout << "Output: " << out_path << " (full mesh OBJ + PolyGroups)" << std::endl;
            std::cout << "Vertices: " << vcount << ", Faces: " << mesh.number_of_faces() << std::endl;
            write_progress(1.0f);
            std::cout << "SUCCESS (full OBJ)" << std::endl;
            pause_if_needed();
            return 0;
        }

        write_fill_only_obj(mesh, original_faces, out_path);
        std::cout << "Fill PolyGroup mode: OBJ patch" << std::endl;

        if (write_fill_only)
        {
            write_fill_only_obj(mesh, original_faces, fill_path);
        }

        if (!debug_obj.empty())
        {
            std::ofstream dout(debug_obj);
            if (dout && CGAL::IO::write_OBJ(dout, mesh))
                std::cout << "Debug OBJ: " << debug_obj << std::endl;
        }

        write_progress(1.0f);
        std::cout << "SUCCESS (OBJ patch)" << std::endl;
        std::cout << "Output: " << out_path << std::endl;
        pause_if_needed();
        return 0;
    }

    GoZ_Mesh out_goz;

    if (from_goz)
    {
        build_output_goz(in_goz, mesh, original_faces, face_to_goz_face, out_goz);
    }
    else
    {
        build_goz_from_cgal(mesh, original_faces, out_goz);
    }

    int new_group = 0;
    if (!out_goz.m_groups.empty())
    {
        for (int fi = 0; fi < out_goz.m_faceCount; ++fi)
            if (out_goz.m_groups[fi] > new_group)
                new_group = out_goz.m_groups[fi];
    }
    std::cout << "Fill PolyGroup ID: " << new_group
              << ", vertices: " << out_goz.m_vertexCount
              << ", faces: " << out_goz.m_faceCount << std::endl;

    write_progress(0.90f);

    if (!out_goz.writeMesh(out_path.c_str()))
    {
        std::cerr << "ERROR: Cannot write output GoZ: " << out_path << std::endl;
        pause_if_needed();
        return 1;
    }

    if (write_fill_only)
    {
        write_fill_only_goz(out_goz, mesh, original_faces, fill_path);
    }

    if (!debug_obj.empty())
    {
        std::ofstream dout(debug_obj);
        if (dout && CGAL::IO::write_OBJ(dout, mesh))
            std::cout << "Debug OBJ: " << debug_obj << std::endl;
    }

    write_progress(1.0f);
    std::cout << "SUCCESS (GoZ output)" << std::endl;
    std::cout << "Output: " << out_path << std::endl;
    std::cout << "Vertices: " << out_goz.m_vertexCount
              << ", Faces: " << out_goz.m_faceCount << std::endl;
    pause_if_needed();

    return 0;
}
