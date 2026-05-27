/**
 * ZMeshMend CGAL Core v2.0 (GoZ)
 * =================================
 * CGAL-based hole filling engine for the ZMeshMend ZBrush plugin.
 *
 * Reads a GoZ mesh (with PolyGroups, Mask, UVs), fills all boundary holes
 * using CGAL::Polygon_mesh_processing::triangulate_refine_and_fair_hole(),
 * and writes the result back to GoZ.
 *
 * New fill faces are assigned a new PolyGroup ID (max existing + 1).
 * Original PolyGroups and Mask are preserved.
 *
 * Build:
 *   mkdir build && cd build && cmake .. && cmake --build . --config Release
 *
 * Usage:
 *   zmeshmend_core <input.GoZ> <output.GoZ> [fill.GoZ]
 *
 * If fill.GoZ is specified, only the newly filled faces are written
 * to that file (for use as a ZBrush subtool for PolyGroup merging).
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
#include <algorithm>

#ifdef _WIN32
#include <Windows.h>
#endif

typedef CGAL::Exact_predicates_inexact_constructions_kernel Kernel;
typedef Kernel::Point_3                                          Point;
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
#endif
    bool zero_arg_mode = false;
    std::string in_path;
    std::string out_path;
    std::string fill_path;
    std::string debug_obj;
    bool write_fill_only = false;
    double opt_min_frac  = 0.0;
    int    opt_min_faces = 0;
    bool   opt_full_obj  = true;

    if (argc < 3)
    {
        zero_arg_mode = true;
#ifdef _WIN32
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
#endif
        {
            FILE* s = fopen("zmeshmend_startup.log", "w");
            if (s) { fprintf(s, "started\n"); fclose(s); }
        }
        //ZCloseHoles mode: read everything from zmeshmend_config.txt,
        //input = zmeshmend_export.obj, output = zmeshmend_import.obj.
        in_path  = "zmeshmend_export.obj";
        out_path = "zmeshmend_import.obj";

        FILE* cf = fopen("zmeshmend_config.txt", "r");
        if (cf)
        {
            char line[256];
            while (fgets(line, sizeof(line), cf))
            {
                int vi = 0; float vf = 0.0f;
                if (sscanf(line, "maskSharpenPasses=%d", &vi) == 1)      { /*pass*/ }
                else if (sscanf(line, "maskGrowRings=%d", &vi) == 1)      { /*pass*/ }
                else if (sscanf(line, "removeSmallFragments=%d", &vi) == 1)
                {
                    if (vi) { opt_min_frac = 0.01; opt_min_faces = 50; }
                }
                else if (sscanf(line, "fragmentMinFraction=%f", &vf) == 1) { opt_min_frac = vf; }
                else if (sscanf(line, "fragmentMinFaces=%d", &vi) == 1)   { opt_min_faces = vi; }
            }
            fclose(cf);

            if (opt_min_frac <= 0.0 && opt_min_faces <= 0)
                opt_min_frac = opt_min_faces = 0; //both must be set to enable
        }
        //zero-arg mode: always pause so user can see console output.
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
        else if (!a.empty() && a[0] != '-')
        {
            // first positional after out_path -> fill_path; second -> debug_obj
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
    else
    {
        // Read OBJ as polygon soup, repair (merge duplicate vertices, drop
        // degenerate/duplicate polygons), orient, then convert to a Surface_mesh.
        // This is the only way ZBrush-exported OBJs (which often have
        // un-stitched per-face vertices) become a manifold mesh that CGAL can
        // detect borders on.
        std::vector<Point> soup_points;
        std::vector<std::vector<std::size_t>> soup_polys;
        if (!CGAL::IO::read_polygon_soup(in_path, soup_points, soup_polys))
        {
            std::cerr << "ERROR: Cannot read OBJ as polygon soup: " << in_path << std::endl;
            pause_if_needed();
            return 1;
        }
        std::cout << "OBJ soup: " << soup_points.size() << " vertices, "
                  << soup_polys.size() << " polygons" << std::endl;

        PMP::repair_polygon_soup(soup_points, soup_polys);
        std::cout << "After repair: " << soup_points.size() << " vertices, "
                  << soup_polys.size() << " polygons" << std::endl;

        if (!PMP::orient_polygon_soup(soup_points, soup_polys))
        {
            std::cout << "WARNING: orient_polygon_soup found non-orientable patches." << std::endl;
        }

        if (!PMP::is_polygon_soup_a_polygon_mesh(soup_polys))
        {
            std::cerr << "ERROR: polygon soup is not a valid polygon mesh after repair." << std::endl;
            pause_if_needed();
            return 1;
        }

        PMP::polygon_soup_to_polygon_mesh(soup_points, soup_polys, mesh);

        if (!CGAL::is_triangle_mesh(mesh))
        {
            std::cout << "Triangulating non-triangle faces..." << std::endl;
            PMP::triangulate_faces(mesh);
        }

        // Stitch any remaining duplicate boundary edges (extra safety).
        PMP::stitch_borders(mesh);

        std::cout << "OBJ Input (final): " << mesh.number_of_vertices()
                  << " vertices, " << mesh.number_of_faces() << " faces" << std::endl;
    }

    write_progress(0.10f);

    std::cout << "CGAL mesh: " << mesh.number_of_vertices() << " vertices, "
              << mesh.number_of_faces() << " faces" << std::endl;

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
                // Full mesh OBJ - all faces are original (group_1)
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
                out << "g group_1\n";
                std::size_t fcount = 0;
                for (auto f : mesh.faces())
                {
                    out << 'f';
                    auto h0 = mesh.halfedge(f);
                    auto h = h0;
                    do
                    {
                        out << ' ' << vidx[mesh.target(h)];
                        h = mesh.next(h);
                    } while (h != h0);
                    out << '\n';
                    ++fcount;
                }
                std::cout << "Output: " << out_path << " (full mesh OBJ, watertight)" << std::endl;
                std::cout << "Vertices: " << vcount << ", Faces: " << fcount << std::endl;
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

        auto result = PMP::triangulate_refine_and_fair_hole(mesh, h);
        bool ok = std::get<0>(result);

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

    // ---------- Remove small disconnected fragments ----------
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
                    // Refresh original_faces: faces removed below should not appear in
                    // either fill or original sets when serializing output.
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

                    // Drop removed faces from original_faces tracker
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
    // ---------- end fragment removal ----------

    if (out_is_obj_patch)
    {
        if (opt_full_obj)
        {
            // Full mesh OBJ with PolyGroup-as-group lines so Tool:Import in
            // ZBrush rebuilds PolyGroups (orig=1, fill=2).
            std::ofstream out(out_path);
            if (!out)
            {
                std::cerr << "ERROR: Cannot open OBJ for write: " << out_path << std::endl;
                pause_if_needed();
                return 1;
            }
            out << "# ZMeshMend full mesh (PolyGroup as 'g group_N')\n";

            // Rebuild a vertex index map (skip removed vertices/faces).
            std::map<Mesh::Vertex_index, std::size_t> vidx;
            std::size_t vcount = 0;
            for (auto v : mesh.vertices())
            {
                const auto& p = mesh.point(v);
                out << "v " << p.x() << ' ' << p.y() << ' ' << p.z() << '\n';
                vidx[v] = ++vcount;
            }

            // Group faces by ZMeshMend tag: 1=original, 2=fill.
            // Faces not in original_faces are fill (newly created during stitching).
            // We emit all original faces first under "g group_1", then fill under "g group_2".
            auto emit_group = [&](int gid, bool fill) {
                bool first = true;
                for (auto f : mesh.faces())
                {
                    bool is_orig = (original_faces.find(f) != original_faces.end());
                    if (fill ? is_orig : !is_orig) continue;
                    if (first)
                    {
                        out << "g group_" << gid << '\n';
                        first = false;
                    }
                    out << 'f';
                    auto h0 = mesh.halfedge(f);
                    auto h = h0;
                    do
                    {
                        out << ' ' << vidx[mesh.target(h)];
                        h = mesh.next(h);
                    } while (h != h0);
                    out << '\n';
                }
            };
            emit_group(1, /*fill=*/false);
            emit_group(2, /*fill=*/true);

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
