/*
  Copyright (c)2010 - Pixologic Inc. All rights reserved.

  THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY
  EXPRESSED OR IMPLIED. ANY USE IS AT YOUR OWN RISK.

  Permission is hereby granted to use or copy this source code ONLY
  in order to exchange data between Pixologic ZBrush and any other
  application, provided the above notices are retained on all copies.

  IT IS ABSOLUTELY FORBIDDEN TO USE THIS SOURCE CODE AND/OR ANY
  STRUCTURE/DATA IT CONTAINS FOR ANY OTHER PURPOSE, AS FOR EXAMPLE
  EXCHANGING DATA BETWEEN 2 OTHER APPLICATIONS.

  Permission to modify the code and to distribute modified code is
  granted, provided the above notices are retained, and a notice that
  the code was modified is included with the above copyright notice.
*/

#include "GoZ_Mesh.h"


GoZ_Mesh::GoZ_Mesh()
: m_flags(0),
  m_vertexCount(0),
  m_faceCount(0), m_faceType(0),
  m_uvFaceType(0),
  m_displacementScale(1.0f)
{
}

GoZ_Mesh::~GoZ_Mesh()
{
}

void
GoZ_Mesh::clear()
{
  m_name.clear();
  m_material.clear();
  m_flags = 0;
  m_vertexCount = 0; m_vertices.clear();
  m_faceCount = 0; m_faceType = 0; m_vertexIndices.clear();
  m_uvFaceType = 0; m_uvs.clear();
  m_mask.clear();
  m_mrgb.clear();
  m_groups.clear();
  m_diffuseMap.clear();
  m_normalMap.clear();
  m_crease.clear();
  m_displacementScale = 1.0f; m_displacementMap.clear();
}

bool
GoZ_Mesh::readMesh(const char* fileName)
{
  clear();

  FILE *file = GoZ_Utils::openGoZFile4Read(fileName);
  if (!file)
    return false;

  GoZ_Header blocHeader;
  bool noErr=true, cont=true;
  int count;
  while (noErr && cont)
  {
    if ((noErr = GoZ_Utils::readGoZBlocHeader(file, &blocHeader)) != 0)
    {
      switch (blocHeader.tag)
      {
      case GoZ_TAG_END_OF_FILE:
        cont = false;
        break;
      case GoZ_TAG_MESH:
        {
        count = blocHeader.size-sizeof(GoZ_Header);
        if (count > 0) {std::vector<char> tmp(count); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, tmp.data()); m_name.assign(tmp.data());}
        }
        break;
      case GoZ_TAG_MATERIAL:
        {
        count = blocHeader.size-sizeof(GoZ_Header);
        if (count > 0) {std::vector<char> tmp(count); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, tmp.data()); m_material.assign(tmp.data());}
        }
        break;
      case GoZ_TAG_FLAGS:
        noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, &m_flags);
        break;
      case GoZ_TAG_POINT_LIST:
        m_vertexCount = blocHeader.iCount;
        if (m_vertexCount) {m_vertices.resize(3*m_vertexCount); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, m_vertices.data());}
        break;
      case GoZ_TAG_FACE4_LIST_FORMAT_1:
      case GoZ_TAG_FACE4_LIST_FORMAT_2:
        m_faceCount = blocHeader.iCount;
        m_faceType = blocHeader.tag;
        if (m_faceCount) {m_vertexIndices.resize(4*m_faceCount); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, m_vertexIndices.data());}
        break;
      case GoZ_TAG_FACE3_LIST:
        m_faceCount = blocHeader.iCount;
        m_faceType = blocHeader.tag;
        if (m_faceCount) {m_vertexIndices.resize(3*m_faceCount); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, m_vertexIndices.data());}
        break;
      case GoZ_TAG_UV4_LIST:
        count = blocHeader.iCount;
        m_uvFaceType = blocHeader.tag;
        if (count) {m_uvs.resize(8*count); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, m_uvs.data());}
        break;
      case GoZ_TAG_UV3_LIST:
        count = blocHeader.iCount;
        m_uvFaceType = blocHeader.tag;
        if (count) {m_uvs.resize(6*count); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, m_uvs.data());}
        break;
      case GoZ_TAG_MASK16_LIST:
        count = blocHeader.iCount;
        if (count) {m_mask.resize(count); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, m_mask.data());}
        break;
      case GoZ_TAG_MRGB_LIST:
        count = blocHeader.iCount;
        if (count) {m_mrgb.resize(count); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, m_mrgb.data());}
        break;
      case GoZ_TAG_GROUPS_LIST:
        count = blocHeader.iCount;
        if (count) {m_groups.resize(count); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, m_groups.data());}
        break;
      case GoZ_TAG_TEXTURE_MAP_PATH:
        {
        count = blocHeader.size-sizeof(GoZ_Header);
        if (count > 0) {std::vector<char> tmp(count); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, tmp.data()); m_diffuseMap.assign(tmp.data());}
        }
        break;
      case GoZ_TAG_NORMAL_MAP_PATH:
        {
        count = blocHeader.size-sizeof(GoZ_Header);
        if (count > 0) {std::vector<char> tmp(count); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, tmp.data()); m_normalMap.assign(tmp.data());}
        }
        break;
      case GoZ_TAG_DISPLACEMENT_MAP_PATH:
        {
        m_displacementScale = blocHeader.modifier;
        count = blocHeader.size-sizeof(GoZ_Header);
        if (count > 0) {std::vector<char> tmp(count); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, tmp.data()); m_displacementMap.assign(tmp.data());}
        }
        break;
      case GoZ_TAG_CREASE_LIST:
        count = blocHeader.iCount;
        if (count) { m_crease.resize(count); noErr = GoZ_Utils::readGoZBlocData(file, &blocHeader, m_crease.data()); }
        break;
      default:
        noErr = GoZ_Utils::skipGoZBloc(file, &blocHeader);
        break;
      }
    }
  }
  GoZ_Utils::closeGoZFile(file);
  return noErr;
}

bool
GoZ_Mesh::writeMesh(const char* fileName)
{
  FILE* pFile = GoZ_Utils::openGoZFile4Write(fileName, "Exported by GoZ SDK");
  if (!pFile) return false;
  bool ok = true;
  if (ok && !m_name.empty() && !GoZ_Utils::writeGoZBloc(pFile, GoZ_TAG_MESH, 1, (void*)m_name.c_str())) ok = false;
  if (ok && !m_material.empty() && !GoZ_Utils::writeGoZBloc(pFile, GoZ_TAG_MATERIAL, 1, (void*)m_material.c_str())) ok = false;
  if (ok && !GoZ_Utils::writeGoZBloc(pFile, GoZ_TAG_FLAGS, 1, &m_flags)) ok = false;
  if (ok && !GoZ_Utils::writeGoZBloc(pFile, GoZ_TAG_POINT_LIST, m_vertexCount, m_vertices.data())) ok = false;
  if (ok && !GoZ_Utils::writeGoZBloc(pFile, m_faceType, m_faceCount, m_vertexIndices.data())) ok = false;
  if (ok && !m_uvs.empty() && !GoZ_Utils::writeGoZBloc(pFile, m_uvFaceType, m_faceCount, m_uvs.data())) ok = false;
  if (ok && !m_mask.empty() && !GoZ_Utils::writeGoZBloc(pFile, GoZ_TAG_MASK16_LIST, m_vertexCount, m_mask.data())) ok = false;
  if (ok && !m_mrgb.empty() && !GoZ_Utils::writeGoZBloc(pFile, GoZ_TAG_MRGB_LIST, m_vertexCount, m_mrgb.data())) ok = false;
  if (ok && !m_groups.empty() && !GoZ_Utils::writeGoZBloc(pFile, GoZ_TAG_GROUPS_LIST, m_faceCount, m_groups.data())) ok = false;
  if (ok && !m_diffuseMap.empty() && !GoZ_Utils::writeGoZBloc(pFile, GoZ_TAG_TEXTURE_MAP_PATH, 1, (void*)m_diffuseMap.c_str())) ok = false;
  if (ok && !m_normalMap.empty() && !GoZ_Utils::writeGoZBloc(pFile, GoZ_TAG_NORMAL_MAP_PATH, 1, (void*)m_normalMap.c_str())) ok = false;
  if (ok && !m_displacementMap.empty() && !GoZ_Utils::writeGoZBloc(pFile, GoZ_TAG_DISPLACEMENT_MAP_PATH, 1, (void*)m_displacementMap.c_str(), m_displacementScale)) ok = false;
  if (ok && !m_crease.empty() && !GoZ_Utils::writeGoZBloc(pFile, GoZ_TAG_CREASE_LIST, m_faceCount, m_crease.data())) ok = false;
  if (ok) ok = GoZ_Utils::writeGoZBloc(pFile, GoZ_TAG_END_OF_FILE, 0, NULL);
  GoZ_Utils::closeGoZFile(pFile);
  return ok;
}

