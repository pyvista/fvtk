// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkPLYReader.h"

#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkDataArray.h"
#include "vtkFloatArray.h"
#include "vtkIncrementalOctreePointLocator.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMathUtilities.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkPLY.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkPolyData.h"
#include "vtkPolygon.h"
#include "vtkSmartPointer.h"
#include "vtkStringArray.h"
#include "vtkTypeInt32Array.h"
#include "vtkTypeInt64Array.h"
#include "vtkUnsignedCharArray.h"

#include "miniply.h"

#include <vtksys/SystemTools.hxx>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkPLYReader);

namespace
{
/**
 * Create an extra point in 'data' with the same coordinates and data as
 * the point at cellPointIndex inside cell. This is to avoid texture artifacts
 * when you have one point with two different texture values (so the latter
 * value override the first. This results in a texture discontinuity which results
 * in artifacts).
 */
vtkIdType duplicateCellPoint(vtkPolyData* data, vtkCell* cell, int cellPointIndex)
{
  // get the old point id
  vtkIdList* pointIds = cell->GetPointIds();
  vtkIdType pointId = pointIds->GetId(cellPointIndex);

  // duplicate that point and all associated data
  vtkPoints* points = data->GetPoints();
  double* point = data->GetPoint(pointId);
  vtkIdType newPointId = points->InsertNextPoint(point);
  for (int i = 0; i < data->GetPointData()->GetNumberOfArrays(); ++i)
  {
    vtkDataArray* a = data->GetPointData()->GetArray(i);
    a->InsertTuple(newPointId, a->GetTuple(pointId));
  }
  // make cell use the new point
  pointIds->SetId(cellPointIndex, newPointId);
  return newPointId;
}

/**
 * Set a newPointId at cellPointIndex inside cell.
 */
void setCellPoint(vtkCell* cell, int cellPointIndex, vtkIdType newPointId)
{
  // get the old point id
  vtkIdList* pointIds = cell->GetPointIds();
  // make cell use the new point
  pointIds->SetId(cellPointIndex, newPointId);
}

/**
 * Compare two points for equality
 */
bool FuzzyEqual(double* f, double* s, double t)
{
  return vtkMathUtilities::FuzzyCompare(f[0], s[0], t) &&
    vtkMathUtilities::FuzzyCompare(f[1], s[1], t) && vtkMathUtilities::FuzzyCompare(f[2], s[2], t);
}
}

// Construct object with merging set to true.
vtkPLYReader::vtkPLYReader()
{
  this->Comments = vtkStringArray::New();
  this->ReadFromInputString = false;
  this->FaceTextureTolerance = 0.000001;
  this->DuplicatePointsForFaceTexture = true;
}

vtkPLYReader::~vtkPLYReader()
{
  this->Comments->Delete();
  this->Comments = nullptr;
}

namespace
{ // required so we don't violate ODR
typedef struct _plyVertex
{
  float x[3]; // the usual 3-space position of a vertex
  float tex[2];
  float normal[3];
  unsigned char red;
  unsigned char green;
  unsigned char blue;
  unsigned char alpha;
} plyVertex;

typedef struct _plyFace
{
  unsigned char intensity; // optional face attributes
  unsigned char red;
  unsigned char green;
  unsigned char blue;
  unsigned char alpha;
  unsigned char nverts;    // number of vertex indices in list
  int* verts;              // vertex index list
  unsigned char ntexcoord; // number of texcoord in list
  float* texcoord;         // texcoord list
} plyFace;
}

int vtkPLYReader::RequestData(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** vtkNotUsed(inputVector), vtkInformationVector* outputVector)
{
  // get the info object
  vtkInformation* outInfo = outputVector->GetInformationObject(0);

  // get the output
  vtkPolyData* output = vtkPolyData::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));

  // cvista fast path: bulk-column binary-LE read via the vendored miniply parser.
  // Only when reading from a real file (not a stream/string). Declines (-1) for
  // anything outside its narrow byte-exact envelope, falling through to legacy.
  if (!this->ReadFromInputStream && !this->ReadFromInputString && this->FileName &&
    this->FileName[0] != '\0')
  {
    int fastResult = this->ReadPLYFast(output);
    if (fastResult >= 0)
    {
      return fastResult;
    }
  }

  PlyProperty vertProps[] = {
    { "x", PLY_FLOAT, PLY_FLOAT, static_cast<int>(offsetof(plyVertex, x)), 0, 0, 0, 0 },
    { "y", PLY_FLOAT, PLY_FLOAT, static_cast<int>(offsetof(plyVertex, x) + sizeof(float)), 0, 0, 0,
      0 },
    { "z", PLY_FLOAT, PLY_FLOAT,
      static_cast<int>(offsetof(plyVertex, x) + sizeof(float) + sizeof(float)), 0, 0, 0, 0 },
    { "u", PLY_FLOAT, PLY_FLOAT, static_cast<int>(offsetof(plyVertex, tex)), 0, 0, 0, 0 },
    { "v", PLY_FLOAT, PLY_FLOAT, static_cast<int>(offsetof(plyVertex, tex) + sizeof(float)), 0, 0,
      0, 0 },
    { "nx", PLY_FLOAT, PLY_FLOAT, static_cast<int>(offsetof(plyVertex, normal)), 0, 0, 0, 0 },
    { "ny", PLY_FLOAT, PLY_FLOAT, static_cast<int>(offsetof(plyVertex, normal) + sizeof(float)), 0,
      0, 0, 0 },
    { "nz", PLY_FLOAT, PLY_FLOAT, static_cast<int>(offsetof(plyVertex, normal) + 2 * sizeof(float)),
      0, 0, 0, 0 },
    { "red", PLY_UCHAR, PLY_UCHAR, static_cast<int>(offsetof(plyVertex, red)), 0, 0, 0, 0 },
    { "green", PLY_UCHAR, PLY_UCHAR, static_cast<int>(offsetof(plyVertex, green)), 0, 0, 0, 0 },
    { "blue", PLY_UCHAR, PLY_UCHAR, static_cast<int>(offsetof(plyVertex, blue)), 0, 0, 0, 0 },
    { "alpha", PLY_UCHAR, PLY_UCHAR, static_cast<int>(offsetof(plyVertex, alpha)), 0, 0, 0, 0 },
  };
  PlyProperty faceProps[] = {
    { "vertex_indices", PLY_INT, PLY_INT, static_cast<int>(offsetof(plyFace, verts)), 1, PLY_UCHAR,
      PLY_UCHAR, static_cast<int>(offsetof(plyFace, nverts)) },
    { "intensity", PLY_UCHAR, PLY_UCHAR, static_cast<int>(offsetof(plyFace, intensity)), 0, 0, 0,
      0 },
    { "red", PLY_UCHAR, PLY_UCHAR, static_cast<int>(offsetof(plyFace, red)), 0, 0, 0, 0 },
    { "green", PLY_UCHAR, PLY_UCHAR, static_cast<int>(offsetof(plyFace, green)), 0, 0, 0, 0 },
    { "blue", PLY_UCHAR, PLY_UCHAR, static_cast<int>(offsetof(plyFace, blue)), 0, 0, 0, 0 },
    { "alpha", PLY_UCHAR, PLY_UCHAR, static_cast<int>(offsetof(plyFace, alpha)), 0, 0, 0, 0 },
    { "texcoord", PLY_FLOAT, PLY_FLOAT, static_cast<int>(offsetof(plyFace, texcoord)), 1, PLY_UCHAR,
      PLY_UCHAR, static_cast<int>(offsetof(plyFace, ntexcoord)) },
  };

  // open a PLY file for reading
  PlyFile* ply;
  int nelems, numElems, nprops;
  char **elist, *elemName;

  if (this->ReadFromInputStream)
  {
    if (!(ply = vtkPLY::ply_read(this->Stream, &nelems, &elist)))
    {
      vtkWarningMacro(<< "Could not open PLY file");
      return 0;
    }
  }
  else if (this->ReadFromInputString)
  {
    if (!(ply = vtkPLY::ply_open_for_reading_from_string(this->InputString, &nelems, &elist)))
    {
      vtkWarningMacro(<< "Could not open PLY file");
      return 0;
    }
  }
  else
  {
    if (!(ply = vtkPLY::ply_open_for_reading(this->FileName, &nelems, &elist)))
    {
      vtkWarningMacro(<< "Could not open PLY file");
      return 0;
    }
  }

  int numberOfComments = 0;
  char** comments = vtkPLY::ply_get_comments(ply, &numberOfComments);
  this->Comments->Reset();
  for (int i = 0; i < numberOfComments; i++)
  {
    this->Comments->InsertNextValue(comments[i]);
  }

  // Check to make sure that we can read geometry
  PlyElement* elem;
  int index;
  if ((elem = vtkPLY::find_element(ply, "vertex")) == nullptr ||
    vtkPLY::find_property(elem, "x", &index) == nullptr ||
    vtkPLY::find_property(elem, "y", &index) == nullptr ||
    vtkPLY::find_property(elem, "z", &index) == nullptr)
  {
    vtkErrorMacro(<< "Cannot read geometry");
    vtkPLY::ply_close(ply);
    return 0;
  }

  // Check for optional attribute data. We can handle intensity; and the
  // triplet red, green, blue.
  bool intensityAvailable = false;
  vtkSmartPointer<vtkUnsignedCharArray> intensity = nullptr;
  if ((elem = vtkPLY::find_element(ply, "face")) != nullptr &&
    vtkPLY::find_property(elem, "intensity", &index) != nullptr)
  {
    intensity = vtkSmartPointer<vtkUnsignedCharArray>::New();
    intensity->SetName("intensity");
    intensityAvailable = true;
    output->GetCellData()->AddArray(intensity);
    output->GetCellData()->SetActiveScalars("intensity");
  }

  bool rgbCellsAvailable = false;
  bool rgbCellsHaveAlpha = false;
  vtkSmartPointer<vtkUnsignedCharArray> rgbCells = nullptr;
  if ((elem = vtkPLY::find_element(ply, "face")) != nullptr &&
    vtkPLY::find_property(elem, "red", &index) != nullptr &&
    vtkPLY::find_property(elem, "green", &index) != nullptr &&
    vtkPLY::find_property(elem, "blue", &index) != nullptr)
  {
    rgbCellsAvailable = true;
    rgbCells = vtkSmartPointer<vtkUnsignedCharArray>::New();
    if (vtkPLY::find_property(elem, "alpha", &index) != nullptr)
    {
      rgbCells->SetName("RGBA");
      rgbCells->SetNumberOfComponents(4);
      rgbCellsHaveAlpha = true;
    }
    else
    {
      rgbCells->SetName("RGB");
      rgbCells->SetNumberOfComponents(3);
    }
    output->GetCellData()->AddArray(rgbCells);
    output->GetCellData()->SetActiveScalars(rgbCells->GetName());
  }

  bool rgbPointsAvailable = false;
  bool rgbPointsHaveAlpha = false;
  vtkSmartPointer<vtkUnsignedCharArray> rgbPoints = nullptr;
  if ((elem = vtkPLY::find_element(ply, "vertex")) != nullptr)
  {
    if (vtkPLY::find_property(elem, "red", &index) != nullptr &&
      vtkPLY::find_property(elem, "green", &index) != nullptr &&
      vtkPLY::find_property(elem, "blue", &index) != nullptr)
    {
      rgbPointsAvailable = true;
    }
    else if (vtkPLY::find_property(elem, "diffuse_red", &index) != nullptr &&
      vtkPLY::find_property(elem, "diffuse_green", &index) != nullptr &&
      vtkPLY::find_property(elem, "diffuse_blue", &index) != nullptr)
    {
      rgbPointsAvailable = true;
      vertProps[8].name = "diffuse_red";
      vertProps[9].name = "diffuse_green";
      vertProps[10].name = "diffuse_blue";
    }
    if (rgbPointsAvailable)
    {
      rgbPoints = vtkSmartPointer<vtkUnsignedCharArray>::New();
      if (vtkPLY::find_property(elem, "alpha", &index) != nullptr)
      {
        rgbPoints->SetName("RGBA");
        rgbPoints->SetNumberOfComponents(4);
        rgbPointsHaveAlpha = true;
      }
      else
      {
        rgbPoints->SetName("RGB");
        rgbPoints->SetNumberOfComponents(3);
      }
      output->GetPointData()->SetScalars(rgbPoints);
    }
  }

  bool normalPointsAvailable = false;
  vtkSmartPointer<vtkFloatArray> normals = nullptr;
  if ((elem = vtkPLY::find_element(ply, "vertex")) != nullptr &&
    vtkPLY::find_property(elem, "nx", &index) != nullptr &&
    vtkPLY::find_property(elem, "ny", &index) != nullptr &&
    vtkPLY::find_property(elem, "nz", &index) != nullptr)
  {
    normals = vtkSmartPointer<vtkFloatArray>::New();
    normalPointsAvailable = true;
    normals->SetName("Normals");
    normals->SetNumberOfComponents(3);
    output->GetPointData()->SetNormals(normals);
  }

  bool texCoordsPointsAvailable = false;
  vtkSmartPointer<vtkFloatArray> texCoordsPoints = nullptr;
  if ((elem = vtkPLY::find_element(ply, "vertex")) != nullptr)
  {
    if (vtkPLY::find_property(elem, "u", &index) != nullptr &&
      vtkPLY::find_property(elem, "v", &index) != nullptr)
    {
      texCoordsPointsAvailable = true;
    }
    else if (vtkPLY::find_property(elem, "texture_u", &index) != nullptr &&
      vtkPLY::find_property(elem, "texture_v", &index) != nullptr)
    {
      texCoordsPointsAvailable = true;
      vertProps[3].name = "texture_u";
      vertProps[4].name = "texture_v";
    }

    if (texCoordsPointsAvailable)
    {
      texCoordsPoints = vtkSmartPointer<vtkFloatArray>::New();
      texCoordsPoints->SetName("TCoords");
      texCoordsPoints->SetNumberOfComponents(2);
      output->GetPointData()->SetTCoords(texCoordsPoints);
    }
  }

  bool texCoordsFaceAvailable = false;
  if ((elem = vtkPLY::find_element(ply, "face")) != nullptr && !texCoordsPointsAvailable)
  {
    if (vtkPLY::find_property(elem, "texcoord", &index) != nullptr)
    {
      texCoordsFaceAvailable = true;
      texCoordsPoints = vtkSmartPointer<vtkFloatArray>::New();
      texCoordsPoints->SetName("TCoords");
      texCoordsPoints->SetNumberOfComponents(2);
      output->GetPointData()->SetTCoords(texCoordsPoints);
    }
  }
  // Okay, now we can grab the data
  int numPts = 0, numPolys = 0;
  for (int i = 0; i < nelems; i++)
  {
    // get the description of the first element */
    elemName = elist[i];
    vtkPLY::ply_get_element_description(ply, elemName, &numElems, &nprops);

    // if we're on vertex elements, read them in
    if (elemName && !strcmp("vertex", elemName))
    {
      // Create a list of points
      numPts = numElems;
      vtkPoints* pts = vtkPoints::New();
      pts->SetDataTypeToFloat();
      pts->SetNumberOfPoints(numPts);

      // Devirtualized point scatter: pts->SetPoint(j, vertex.x) resolves to a
      // virtual vtkDataArray::SetTuple(j, const float*), which for the float
      // point array above stores static_cast<float>(static_cast<double>(src[c]))
      // == src[c] per component. Grab the raw float pointer ONCE (the points
      // array is always vtkFloatArray here) and copy the 3 coordinates inline;
      // the bytes written are identical. Fall back to the virtual path if the
      // FastDownCast ever fails to hold (it never should given the SetDataType
      // above).
      float* rawPts = nullptr;
      if (auto* fa = vtkFloatArray::FastDownCast(pts->GetData()))
      {
        rawPts = fa->GetPointer(0);
      }

      // Setup to read the PLY elements
      vtkPLY::ply_get_property(ply, elemName, &vertProps[0]);
      vtkPLY::ply_get_property(ply, elemName, &vertProps[1]);
      vtkPLY::ply_get_property(ply, elemName, &vertProps[2]);

      if (texCoordsPointsAvailable)
      {
        vtkPLY::ply_get_property(ply, elemName, &vertProps[3]);
        vtkPLY::ply_get_property(ply, elemName, &vertProps[4]);
        texCoordsPoints->SetNumberOfTuples(numPts);
      }

      if (normalPointsAvailable)
      {
        vtkPLY::ply_get_property(ply, elemName, &vertProps[5]);
        vtkPLY::ply_get_property(ply, elemName, &vertProps[6]);
        vtkPLY::ply_get_property(ply, elemName, &vertProps[7]);
        normals->SetNumberOfTuples(numPts);
      }

      if (rgbPointsAvailable)
      {
        vtkPLY::ply_get_property(ply, elemName, &vertProps[8]);
        vtkPLY::ply_get_property(ply, elemName, &vertProps[9]);
        vtkPLY::ply_get_property(ply, elemName, &vertProps[10]);
        if (rgbPointsHaveAlpha)
        {
          vtkPLY::ply_get_property(ply, elemName, &vertProps[11]);
        }
        rgbPoints->SetNumberOfTuples(numPts);
      }

      plyVertex vertex;
      for (int j = 0; j < numPts; j++)
      {
        vtkPLY::ply_get_element(ply, (void*)&vertex);
        if (rawPts)
        {
          float* p = rawPts + 3 * j;
          p[0] = vertex.x[0];
          p[1] = vertex.x[1];
          p[2] = vertex.x[2];
        }
        else
        {
          pts->SetPoint(j, vertex.x);
        }
        if (texCoordsPointsAvailable)
        {
          texCoordsPoints->SetTuple2(j, vertex.tex[0], vertex.tex[1]);
        }
        if (normalPointsAvailable)
        {
          normals->SetTuple3(j, vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        }
        if (rgbPointsAvailable)
        {
          if (rgbPointsHaveAlpha)
          {
            rgbPoints->SetTuple4(j, vertex.red, vertex.green, vertex.blue, vertex.alpha);
          }
          else
          {
            rgbPoints->SetTuple3(j, vertex.red, vertex.green, vertex.blue);
          }
        }
      }
      output->SetPoints(pts);
      pts->Delete();
    } // if vertex

    else if (elemName && !strcmp("face", elemName))
    {
      // texture coordinates
      vtkNew<vtkPoints> texCoords;
      // We store a list of pointIds (that have the same texture coordinates)
      // at the texture index returned by texLocator
      std::vector<std::vector<vtkIdType>> pointIds;
      pointIds.resize(output->GetNumberOfPoints());
      // Used to detect different texture values at a vertex.
      vtkNew<vtkIncrementalOctreePointLocator> texLocator;
      texLocator->SetTolerance(this->FaceTextureTolerance);
      double bounds[] = { 0.0, 1.0, 0.0, 1.0, 0.0, 0.0 };
      texLocator->InitPointInsertion(texCoords, bounds);

      // Create a polygonal array
      numPolys = numElems;
      vtkSmartPointer<vtkCellArray> polys = vtkSmartPointer<vtkCellArray>::New();
      polys->AllocateEstimate(numPolys, 3);
      plyFace face;
      vtkIdType vtkVerts[256];

      // Get the face properties
      vtkPLY::ply_get_property(ply, elemName, &faceProps[0]);
      if (intensityAvailable)
      {
        vtkPLY::ply_get_property(ply, elemName, &faceProps[1]);
        intensity->SetNumberOfComponents(1);
        intensity->SetNumberOfTuples(numPolys);
      }
      if (rgbCellsAvailable)
      {
        vtkPLY::ply_get_property(ply, elemName, &faceProps[2]);
        vtkPLY::ply_get_property(ply, elemName, &faceProps[3]);
        vtkPLY::ply_get_property(ply, elemName, &faceProps[4]);

        if (rgbCellsHaveAlpha)
        {
          vtkPLY::ply_get_property(ply, elemName, &faceProps[5]);
        }
        rgbCells->SetNumberOfTuples(numPolys);
      }
      if (texCoordsFaceAvailable)
      {
        vtkPLY::ply_get_property(ply, elemName, &faceProps[6]);
        texCoordsPoints->SetNumberOfTuples(numPts);
        if (this->DuplicatePointsForFaceTexture)
        {
          // initialize texture coordinates with invalid value
          for (int j = 0; j < numPts; ++j)
          {
            texCoordsPoints->SetTuple2(j, -1, -1);
          }
        }
      }

      // grab all the face elements
      vtkNew<vtkPolygon> cell;
      for (int j = 0; j < numPolys; j++)
      {
        // grab and element from the file
        vtkPLY::ply_get_element(ply, (void*)&face);
        for (int k = 0; k < face.nverts; k++)
        {
          vtkVerts[k] = face.verts[k];
        }
        free(face.verts); // allocated in vtkPLY::ascii/binary_get_element

        cell->Initialize(face.nverts, vtkVerts, output->GetPoints());
        if (intensityAvailable)
        {
          intensity->SetValue(j, face.intensity);
        }
        if (rgbCellsAvailable)
        {
          if (rgbCellsHaveAlpha)
          {
            rgbCells->SetValue(4 * j, face.red);
            rgbCells->SetValue(4 * j + 1, face.green);
            rgbCells->SetValue(4 * j + 2, face.blue);
            rgbCells->SetValue(4 * j + 3, face.alpha);
          }
          else
          {
            rgbCells->SetValue(3 * j, face.red);
            rgbCells->SetValue(3 * j + 1, face.green);
            rgbCells->SetValue(3 * j + 2, face.blue);
          }
        }
        if (texCoordsFaceAvailable)
        {
          // Test to know if there is a texcoord for every vertex
          if (face.nverts == (face.ntexcoord / 2))
          {
            if (this->DuplicatePointsForFaceTexture)
            {
              for (int k = 0; k < face.nverts; k++)
              {
                // new texture stored at the current face
                float newTex[] = { face.texcoord[k * 2], face.texcoord[k * 2 + 1] };
                // texture stored at vtkVerts[k] point
                float currentTex[2];
                texCoordsPoints->GetTypedTuple(vtkVerts[k], currentTex);
                double newTex3[] = { newTex[0], newTex[1], 0 };
                if (currentTex[0] == -1.0)
                {
                  // newly seen texture coordinates for vertex
                  texCoordsPoints->SetTuple2(vtkVerts[k], newTex[0], newTex[1]);
                  vtkIdType ti;
                  texLocator->InsertUniquePoint(newTex3, ti);
                  pointIds.resize(std::max(ti + 1, static_cast<vtkIdType>(pointIds.size())));
                  pointIds[ti].push_back(vtkVerts[k]);
                }
                else
                {
                  if (!vtkMathUtilities::FuzzyCompare(
                        currentTex[0], newTex[0], this->FaceTextureTolerance) ||
                    !vtkMathUtilities::FuzzyCompare(
                      currentTex[1], newTex[1], this->FaceTextureTolerance))
                  {
                    // different texture coordinate
                    // than stored at point vtkVerts[k]
                    vtkIdType ti;
                    int inserted = texLocator->InsertUniquePoint(newTex3, ti);
                    if (inserted)
                    {
                      // newly seen texture coordinate for vertex
                      // which already has some texture coordinates.
                      vtkIdType dp = duplicateCellPoint(output, cell, k);
                      texCoordsPoints->SetTuple2(dp, newTex[0], newTex[1]);
                      pointIds.resize(std::max(ti + 1, static_cast<vtkIdType>(pointIds.size())));
                      pointIds[ti].push_back(dp);
                    }
                    else
                    {
                      size_t sameTexIndex = 0;

                      double first[3];
                      output->GetPoint(vtkVerts[k], first);
                      for (; sameTexIndex < pointIds[ti].size(); ++sameTexIndex)
                      {
                        double second[3];
                        output->GetPoint(pointIds[ti][sameTexIndex], second);
                        if (FuzzyEqual(first, second, this->FaceTextureTolerance))
                        {
                          break;
                        }
                      }
                      if (sameTexIndex == pointIds[ti].size())
                      {
                        // newly seen point for this texture coordinate
                        vtkIdType dp = duplicateCellPoint(output, cell, k);
                        texCoordsPoints->SetTuple2(dp, newTex[0], newTex[1]);
                        pointIds[ti].push_back(dp);
                      }

                      // texture coordinate already seen before, use the vertex
                      // associated with these texture coordinates
                      vtkIdType vi = pointIds[ti][sameTexIndex];
                      setCellPoint(cell, k, vi);
                    }
                  }
                  // same texture coordinate, nothing to do.
                }
              }
            }
            else
            {
              // if we don't want point duplication we only need to set
              // the texture coordinates
              for (int k = 0; k < face.nverts; k++)
              {
                // new texture stored at the current face
                float newTex[] = { face.texcoord[k * 2], face.texcoord[k * 2 + 1] };
                texCoordsPoints->SetTuple2(vtkVerts[k], newTex[0], newTex[1]);
              }
            }
          }
          else
          {
            vtkWarningMacro(<< "Number of texture coordinates " << face.ntexcoord
                            << " different than number of points " << face.nverts);
          }
          free(face.texcoord);
        }
        polys->InsertNextCell(cell);
      }
      output->SetPolys(polys);
    }

    free(elist[i]); // allocated by ply_open_for_reading
    elist[i] = nullptr;

  } // for all elements of the PLY file
  // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
  free(elist); // allocated by ply_open_for_reading

  vtkDebugMacro(<< "Read: " << numPts << " points, " << numPolys << " polygons");

  // close the PLY file
  vtkPLY::ply_close(ply);

  return 1;
}

//------------------------------------------------------------------------------
// cvista fast binary-PLY bulk reader.
//
// Reads a binary little-endian PLY with the vendored miniply parser (MIT,
// Copyright (c) 2019 Vilya Harvey -- see miniply.h) doing columnar bulk
// extraction instead of vtkPLY's per-row property dispatch. It engages only
// when every consumed property's stored type already equals its VTK destination
// type, so each value is a verbatim little-endian copy: the output is then
// byte-identical to the legacy reader, with point and face order preserved and
// polygons left untriangulated (PLY faces index into the point array, so order
// is load-bearing). Any file outside this narrow envelope returns -1 and the
// caller falls back to the legacy reader.
int vtkPLYReader::ReadPLYFast(vtkPolyData* output)
{
  using miniply::PLYElement;
  using miniply::PLYProperty;
  using miniply::PLYPropertyType;
  using miniply::PLYReader;
  constexpr uint32_t kInvalid = miniply::kInvalidIndex;

  PLYReader reader(this->FileName);
  if (!reader.valid())
  {
    return -1; // not openable as PLY; let the legacy path emit the warning
  }
  // Binary little-endian only. ASCII float parsing and big-endian byte swaps
  // risk a last-ULP divergence from vtkPLY, and PLY point positions are sacred
  // (faces reference them by index), so we will not relax them.
  if (reader.file_type() != miniply::PLYFileType::Binary)
  {
    return -1;
  }

  auto isFloat = [&](const PLYElement* e, uint32_t i) {
    return i != kInvalid && e->properties[i].type == PLYPropertyType::Float;
  };
  auto isUChar = [&](const PLYElement* e, uint32_t i) {
    return i != kInvalid && e->properties[i].type == PLYPropertyType::UChar;
  };

  // ---- required: vertex element with float x, y, z ----
  uint32_t vIdx = reader.find_element(miniply::kPLYVertexElement);
  if (vIdx == kInvalid)
  {
    return -1; // legacy errors "Cannot read geometry"
  }
  PLYElement* vElem = reader.get_element(vIdx);
  if (!vElem->fixedSize)
  {
    return -1; // a list property among vertices -> outside the columnar envelope
  }
  uint32_t xi = vElem->find_property("x");
  uint32_t yi = vElem->find_property("y");
  uint32_t zi = vElem->find_property("z");
  if (!isFloat(vElem, xi) || !isFloat(vElem, yi) || !isFloat(vElem, zi))
  {
    return -1;
  }

  // ---- optional vertex texture coords (u,v or texture_u,texture_v), float ----
  bool texCoordsPointsAvailable = false;
  uint32_t ui = kInvalid, vi = kInvalid;
  {
    uint32_t a = vElem->find_property("u");
    uint32_t b = vElem->find_property("v");
    if (a == kInvalid || b == kInvalid)
    {
      a = vElem->find_property("texture_u");
      b = vElem->find_property("texture_v");
    }
    if (a != kInvalid && b != kInvalid)
    {
      if (!isFloat(vElem, a) || !isFloat(vElem, b))
      {
        return -1;
      }
      ui = a;
      vi = b;
      texCoordsPointsAvailable = true;
    }
  }

  // ---- optional vertex normals (nx,ny,nz), float ----
  bool normalPointsAvailable = false;
  uint32_t nxi = kInvalid, nyi = kInvalid, nzi = kInvalid;
  {
    uint32_t a = vElem->find_property("nx");
    uint32_t b = vElem->find_property("ny");
    uint32_t c = vElem->find_property("nz");
    if (a != kInvalid && b != kInvalid && c != kInvalid)
    {
      if (!isFloat(vElem, a) || !isFloat(vElem, b) || !isFloat(vElem, c))
      {
        return -1;
      }
      nxi = a;
      nyi = b;
      nzi = c;
      normalPointsAvailable = true;
    }
  }

  // ---- optional vertex colors (red/green/blue[/alpha] or diffuse_*), uchar ----
  bool rgbPointsAvailable = false;
  bool rgbPointsHaveAlpha = false;
  uint32_t pri = kInvalid, pgi = kInvalid, pbi = kInvalid, pai = kInvalid;
  {
    uint32_t r = vElem->find_property("red");
    uint32_t g = vElem->find_property("green");
    uint32_t b = vElem->find_property("blue");
    if (r == kInvalid || g == kInvalid || b == kInvalid)
    {
      r = vElem->find_property("diffuse_red");
      g = vElem->find_property("diffuse_green");
      b = vElem->find_property("diffuse_blue");
    }
    if (r != kInvalid && g != kInvalid && b != kInvalid)
    {
      if (!isUChar(vElem, r) || !isUChar(vElem, g) || !isUChar(vElem, b))
      {
        return -1;
      }
      pri = r;
      pgi = g;
      pbi = b;
      rgbPointsAvailable = true;
      uint32_t a = vElem->find_property("alpha");
      if (a != kInvalid)
      {
        if (!isUChar(vElem, a))
        {
          return -1;
        }
        pai = a;
        rgbPointsHaveAlpha = true;
      }
    }
  }

  // ---- optional face element with an integer vertex_indices list ----
  bool faceElemPresent = false;
  PLYElement* fElem = nullptr;
  uint32_t viIdx = kInvalid;
  uint32_t fIdx = reader.find_element(miniply::kPLYFaceElement);
  if (fIdx != kInvalid)
  {
    if (fIdx < vIdx)
    {
      return -1; // face declared before vertex: outside our ordering assumption
    }
    faceElemPresent = true;
    fElem = reader.get_element(fIdx);
    viIdx = fElem->find_property("vertex_indices");
    if (viIdx == kInvalid)
    {
      return -1;
    }
    const PLYProperty& vp = fElem->properties[viIdx];
    if (vp.countType == PLYPropertyType::None || vp.type == PLYPropertyType::Float ||
      vp.type == PLYPropertyType::Double)
    {
      return -1; // must be a list of integers (verbatim integer copy)
    }
    // The per-face texcoord path duplicates points in the legacy reader; decline
    // so the legacy reader handles that fidelity-sensitive case.
    if (fElem->find_property("texcoord") != kInvalid && !texCoordsPointsAvailable)
    {
      return -1;
    }
  }

  // ---- optional face intensity (uchar) and face colors (uchar) ----
  bool intensityAvailable = false;
  uint32_t intensityIdx = kInvalid;
  bool rgbCellsAvailable = false;
  bool rgbCellsHaveAlpha = false;
  uint32_t fri = kInvalid, fgi = kInvalid, fbi = kInvalid, fai = kInvalid;
  if (faceElemPresent)
  {
    uint32_t a = fElem->find_property("intensity");
    if (a != kInvalid)
    {
      if (!isUChar(fElem, a))
      {
        return -1;
      }
      intensityIdx = a;
      intensityAvailable = true;
    }
    uint32_t r = fElem->find_property("red");
    uint32_t g = fElem->find_property("green");
    uint32_t b = fElem->find_property("blue");
    if (r != kInvalid && g != kInvalid && b != kInvalid)
    {
      if (!isUChar(fElem, r) || !isUChar(fElem, g) || !isUChar(fElem, b))
      {
        return -1;
      }
      fri = r;
      fgi = g;
      fbi = b;
      rgbCellsAvailable = true;
      uint32_t fa = fElem->find_property("alpha");
      if (fa != kInvalid)
      {
        if (!isUChar(fElem, fa))
        {
          return -1;
        }
        fai = fa;
        rgbCellsHaveAlpha = true;
      }
    }
  }

  // ============================================================================
  // Detection passed: build everything into locals, then attach to `output` only
  // on success so a partial/corrupt read leaves `output` untouched for fallback.
  // ============================================================================
  const uint32_t numPts = vElem->count;
  const uint32_t numPolys = faceElemPresent ? fElem->count : 0;

  vtkSmartPointer<vtkPoints> pts = vtkSmartPointer<vtkPoints>::New();
  pts->SetDataTypeToFloat();
  pts->SetNumberOfPoints(numPts);

  vtkSmartPointer<vtkFloatArray> tcoords;
  if (texCoordsPointsAvailable)
  {
    tcoords = vtkSmartPointer<vtkFloatArray>::New();
    tcoords->SetName("TCoords");
    tcoords->SetNumberOfComponents(2);
    tcoords->SetNumberOfTuples(numPts);
  }
  vtkSmartPointer<vtkFloatArray> normals;
  if (normalPointsAvailable)
  {
    normals = vtkSmartPointer<vtkFloatArray>::New();
    normals->SetName("Normals");
    normals->SetNumberOfComponents(3);
    normals->SetNumberOfTuples(numPts);
  }
  vtkSmartPointer<vtkUnsignedCharArray> rgbPoints;
  if (rgbPointsAvailable)
  {
    rgbPoints = vtkSmartPointer<vtkUnsignedCharArray>::New();
    rgbPoints->SetName(rgbPointsHaveAlpha ? "RGBA" : "RGB");
    rgbPoints->SetNumberOfComponents(rgbPointsHaveAlpha ? 4 : 3);
    rgbPoints->SetNumberOfTuples(numPts);
  }
  vtkSmartPointer<vtkUnsignedCharArray> intensity;
  if (intensityAvailable)
  {
    intensity = vtkSmartPointer<vtkUnsignedCharArray>::New();
    intensity->SetName("intensity");
    intensity->SetNumberOfComponents(1);
    intensity->SetNumberOfTuples(numPolys);
  }
  vtkSmartPointer<vtkUnsignedCharArray> rgbCells;
  if (rgbCellsAvailable)
  {
    rgbCells = vtkSmartPointer<vtkUnsignedCharArray>::New();
    rgbCells->SetName(rgbCellsHaveAlpha ? "RGBA" : "RGB");
    rgbCells->SetNumberOfComponents(rgbCellsHaveAlpha ? 4 : 3);
    rgbCells->SetNumberOfTuples(numPolys);
  }
  vtkSmartPointer<vtkCellArray> polys;

  bool gotVerts = false;
  bool gotFaces = false;
  while (reader.has_element())
  {
    if (reader.element_is(miniply::kPLYVertexElement) && !gotVerts)
    {
      if (!reader.load_element())
      {
        return -1;
      }
      uint32_t idx[4];
      idx[0] = xi;
      idx[1] = yi;
      idx[2] = zi;
      if (!reader.extract_properties(idx, 3, PLYPropertyType::Float, pts->GetVoidPointer(0)))
      {
        return -1;
      }
      if (texCoordsPointsAvailable)
      {
        idx[0] = ui;
        idx[1] = vi;
        if (!reader.extract_properties(idx, 2, PLYPropertyType::Float, tcoords->GetPointer(0)))
        {
          return -1;
        }
      }
      if (normalPointsAvailable)
      {
        idx[0] = nxi;
        idx[1] = nyi;
        idx[2] = nzi;
        if (!reader.extract_properties(idx, 3, PLYPropertyType::Float, normals->GetPointer(0)))
        {
          return -1;
        }
      }
      if (rgbPointsAvailable)
      {
        idx[0] = pri;
        idx[1] = pgi;
        idx[2] = pbi;
        idx[3] = pai;
        uint32_t n = rgbPointsHaveAlpha ? 4 : 3;
        if (!reader.extract_properties(idx, n, PLYPropertyType::UChar, rgbPoints->GetPointer(0)))
        {
          return -1;
        }
      }
      gotVerts = true;
    }
    else if (faceElemPresent && reader.element_is(miniply::kPLYFaceElement) && !gotFaces)
    {
      if (!reader.load_element())
      {
        return -1;
      }
      const uint32_t* counts = reader.get_list_counts(viIdx);
      const uint32_t total = reader.sum_of_list_counts(viIdx);
      std::vector<int32_t> conn(total);
      if (total > 0 && !reader.extract_list_property(viIdx, PLYPropertyType::Int, conn.data()))
      {
        return -1;
      }
      // cvista-wide rule (width-relaxed): default the cell array to 32-bit
      // offsets/connectivity, widening to 64-bit only when a value cannot fit in
      // int32 (numPolys/total or an index >= 2^31). Integer VALUES are identical
      // to stock VTK; only the storage container narrows (stock defaults to
      // 64-bit). This halves the cell-array footprint for the overwhelmingly
      // common case. See [[cvista-int32-default-width-relaxed]].
      // Cheap, robust width check: 32-bit storage is safe iff every value that
      // will be stored fits in int32. Offset values range over [0, total];
      // connectivity values are point indices in [0, numPts). Bounding those two
      // counts bounds every stored value (numPolys <= total, so it needs no
      // separate check). Two unsigned compares -- no per-element scan.
      constexpr uint32_t kI32Max = 0x7FFFFFFFu;
      const bool fits32 = (numPts <= kI32Max) && (total <= kI32Max);
      polys = vtkSmartPointer<vtkCellArray>::New();
      if (fits32)
      {
        vtkNew<vtkTypeInt32Array> offArr;
        offArr->SetNumberOfValues(static_cast<vtkIdType>(numPolys) + 1);
        vtkNew<vtkTypeInt32Array> connArr;
        connArr->SetNumberOfValues(static_cast<vtkIdType>(total));
        vtkTypeInt32* op = offArr->GetPointer(0);
        vtkTypeInt32* cp = connArr->GetPointer(0);
        vtkTypeInt32 acc = 0;
        op[0] = 0;
        for (uint32_t j = 0; j < numPolys; ++j)
        {
          acc += static_cast<vtkTypeInt32>(counts[j]);
          op[j + 1] = acc;
        }
        for (uint32_t i = 0; i < total; ++i)
        {
          cp[i] = static_cast<vtkTypeInt32>(conn[i]);
        }
        polys->SetData(offArr, connArr);
      }
      else
      {
        vtkNew<vtkTypeInt64Array> offArr;
        offArr->SetNumberOfValues(static_cast<vtkIdType>(numPolys) + 1);
        vtkNew<vtkTypeInt64Array> connArr;
        connArr->SetNumberOfValues(static_cast<vtkIdType>(total));
        vtkTypeInt64* op = offArr->GetPointer(0);
        vtkTypeInt64* cp = connArr->GetPointer(0);
        vtkTypeInt64 acc = 0;
        op[0] = 0;
        for (uint32_t j = 0; j < numPolys; ++j)
        {
          acc += static_cast<vtkTypeInt64>(counts[j]);
          op[j + 1] = acc;
        }
        for (uint32_t i = 0; i < total; ++i)
        {
          cp[i] = static_cast<vtkTypeInt64>(conn[i]);
        }
        polys->SetData(offArr, connArr);
      }

      if (intensityAvailable)
      {
        uint32_t idx[1] = { intensityIdx };
        if (!reader.extract_properties(idx, 1, PLYPropertyType::UChar, intensity->GetPointer(0)))
        {
          return -1;
        }
      }
      if (rgbCellsAvailable)
      {
        uint32_t idx[4] = { fri, fgi, fbi, fai };
        uint32_t n = rgbCellsHaveAlpha ? 4 : 3;
        if (!reader.extract_properties(idx, n, PLYPropertyType::UChar, rgbCells->GetPointer(0)))
        {
          return -1;
        }
      }
      gotFaces = true;
    }
    reader.next_element();
  }

  if (!gotVerts || (faceElemPresent && !gotFaces))
  {
    return -1; // file didn't yield the elements its header advertised
  }

  // Comments: miniply discards them, so replicate vtkPLY's header handling to
  // keep GetComments() faithful (a side accessor, not part of the serialized
  // mesh). Re-scan the short ASCII header for "comment" lines.
  this->Comments->Reset();
  if (FILE* hf = vtksys::SystemTools::Fopen(this->FileName, "rb"))
  {
    char line[1024];
    while (std::fgets(line, sizeof(line), hf))
    {
      for (char* p = line; *p; ++p)
      {
        if (*p == '\t')
        {
          *p = ' ';
        }
        else if (*p == '\r' || *p == '\n')
        {
          *p = '\0';
          break;
        }
      }
      if (std::strncmp(line, "end_header", 10) == 0)
      {
        break;
      }
      if (std::strncmp(line, "comment", 7) == 0 && (line[7] == ' ' || line[7] == '\0'))
      {
        int i = 7;
        while (line[i] == ' ')
        {
          ++i;
        }
        this->Comments->InsertNextValue(line + i);
      }
    }
    std::fclose(hf);
  }

  // Attach to output in the legacy reader's array order so the result is
  // structurally identical (same array indices and active attributes).
  if (intensityAvailable)
  {
    output->GetCellData()->AddArray(intensity);
    output->GetCellData()->SetActiveScalars("intensity");
  }
  if (rgbCellsAvailable)
  {
    output->GetCellData()->AddArray(rgbCells);
    output->GetCellData()->SetActiveScalars(rgbCells->GetName());
  }
  output->SetPoints(pts);
  if (rgbPointsAvailable)
  {
    output->GetPointData()->SetScalars(rgbPoints);
  }
  if (normalPointsAvailable)
  {
    output->GetPointData()->SetNormals(normals);
  }
  if (texCoordsPointsAvailable)
  {
    output->GetPointData()->SetTCoords(tcoords);
  }
  if (faceElemPresent)
  {
    output->SetPolys(polys);
  }

  vtkDebugMacro(<< "Read (fast): " << numPts << " points, " << numPolys << " polygons");
  // Optional diagnostic breadcrumb: when CVISTA_PLY_FASTPATH_TRACE is set, report
  // (to stderr) that the fast path handled this file. Off by default -> no cost
  // and no behavior change; used by the byte-exact validation to confirm which
  // files engage the fast path vs fall back to the legacy reader.
  if (std::getenv("CVISTA_PLY_FASTPATH_TRACE"))
  {
    std::fprintf(stderr, "CVISTA_PLY_FAST %s\n", this->FileName ? this->FileName : "");
  }
  return 1;
}

int vtkPLYReader::CanReadFile(const char* filename)
{
  FILE* fd = vtksys::SystemTools::Fopen(filename, "rb");
  if (!fd)
    return 0;

  char line[4] = {};
  const char* result = fgets(line, sizeof(line), fd);
  fclose(fd);
  return (result && strncmp(result, "ply", 3) == 0);
}

void vtkPLYReader::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  os << indent << "Comments:\n";
  indent = indent.GetNextIndent();
  for (int i = 0; i < this->Comments->GetNumberOfValues(); ++i)
  {
    os << indent << this->Comments->GetValue(i) << "\n";
  }
}
VTK_ABI_NAMESPACE_END
