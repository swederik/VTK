/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtk3DLinearGridPlaneCutter.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtk3DLinearGridPlaneCutter.h"

#include "vtkPlane.h"
#include "vtkUnstructuredGrid.h"
#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkTetra.h"
#include "vtkHexahedron.h"
#include "vtkWedge.h"
#include "vtkPyramid.h"
#include "vtkVoxel.h"
#include "vtkTriangle.h"
#include "vtkPointData.h"
#include "vtkPolyData.h"
#include "vtkFloatArray.h"
#include "vtkStaticPointLocator.h"
#include "vtkStaticEdgeLocatorTemplate.h"
#include "vtkArrayListTemplate.h" // For processing attribute data
#include "vtkStaticCellLinksTemplate.h"
#include "vtkCompositeDataIterator.h"
#include "vtkCompositeDataSet.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkGarbageCollector.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkObjectFactory.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkSMPTools.h"
#include "vtkSMPThreadLocalObject.h"

vtkStandardNewMacro(vtk3DLinearGridPlaneCutter);
vtkCxxSetObjectMacro(vtk3DLinearGridPlaneCutter,Plane,vtkPlane);

//-----------------------------------------------------------------------------
// Classes to support threaded execution. Note that there is only one
// strategy at this time: a path that pre-computes plane function values and
// uses these to cull non-intersected cells. Sphere trees may be supported in
// the future.

// Macros immediately below are just used to make code easier to
// read. Invokes functor _op _num times depending on serial (_seq==1) or
// parallel processing mode. The _REDUCE_ version is used to called functors
// with a Reduce() method).
#define EXECUTE_SMPFOR(_seq,_num,_op) \
  if ( !_seq) \
  {\
    vtkSMPTools::For(0,_num,_op); \
  }\
  else \
  {\
  _op(0,_num);\
  }

#define EXECUTE_REDUCED_SMPFOR(_seq,_num,_op,_nt)  \
  if ( !_seq) \
  {\
    vtkSMPTools::For(0,_num,_op); \
  }\
  else \
  {\
  _op.Initialize();\
  _op(0,_num);\
  _op.Reduce();\
  }\
  _nt = _op.NumThreadsUsed;


namespace {

//========================= CELL MACHINERY ====================================

// Implementation note: this filter currently handles 3D linear cells. It
// could be extended to handle other 3D cell types if the contouring
// operation can be expressed as transformation of scalar values from cell
// vertices to a case table of triangles.

// The maximum number of verts per cell
#define MAX_CELL_VERTS 8

// Base class to represent cells
struct BaseCell
{
  unsigned char CellType;
  unsigned char NumVerts;
  unsigned char NumEdges;
  unsigned short *Cases;
  static unsigned char Mask[MAX_CELL_VERTS];

  BaseCell(int cellType) : CellType(cellType), NumVerts(0), NumEdges(0), Cases(nullptr) {}
  virtual ~BaseCell() {}

  // Set up the case table. This is done by accessing standard VTK cells and
  // repackaging the case table for efficiency. The format of the case table
  // is as follows: a linear array, organized into two parts: 1) offsets into
  // the second part, and 2) the cases. The first 2^NumVerts entries are the
  // offsets which refer to the 2^NumVerts cases in the second part. Each
  // case is represented by the number of edges, followed by pairs of
  // vertices (v0,v1) for each edge. Note that groups of three contiguous
  // edges form a triangle.
  virtual void BuildCases() = 0;
  void BuildCases(int numCases, int **edges, int **cases, unsigned short *caseArray);
};
// Used to generate case mask
unsigned char BaseCell::Mask[MAX_CELL_VERTS] = {1,2,4,8,16,32,64,128};
// Build repackaged case table and place into cases array.
void BaseCell::BuildCases(int numCases, int **edges, int **cases,
                          unsigned short *caseArray)
{
  int caseOffset = numCases;
  for (int caseNum=0; caseNum < numCases; ++caseNum)
  {
    caseArray[caseNum] = caseOffset;
    int *triCases = cases[caseNum];

    // Count the number of edges
    int count;
    for (count=0; triCases[count] != (-1); ++count) {}
    caseArray[caseOffset++] = count;

    // Now populate the edges
    int *edge;
    for (count=0; triCases[count] != (-1); ++count)
    {
      edge = edges[triCases[count]];
      caseArray[caseOffset++] = edge[0];
      caseArray[caseOffset++] = edge[1];
    }
  }//for all cases
}

// Contour tetrahedral cell------------------------------------------------------
// Repackages case table for more efficient processing.
struct TetCell : public BaseCell
{
  static unsigned short TetCases[152];

  TetCell() : BaseCell(VTK_TETRA)
  {
    this->NumVerts = 4;
    this->NumEdges = 6;
    this->BuildCases();
    this->Cases = this->TetCases;
  }
  ~TetCell() override {}
  void BuildCases() override;
};
// Dummy initialization filled in later at initialization. The lengtth of the
// array is determined from the equation length=(2*NumCases + 3*2*NumTris).
unsigned short TetCell::TetCases[152] = { 0 };
// Load and transform vtkTetra case table. The case tables are repackaged for
// efficiency (e.g., support the GetCase() method).
void TetCell::BuildCases()
{
  int **edges = new int*[this->NumEdges];
  int numCases = std::pow(2,this->NumVerts);
  int **cases = new int*[numCases];
  for ( int i=0; i<this->NumEdges; ++i)
  {
    edges[i] = vtkTetra::GetEdgeArray(i);
  }
  for ( int i=0; i < numCases; ++i)
  {
    cases[i] = vtkTetra::GetTriangleCases(i);
  }

  BaseCell::BuildCases(numCases, edges, cases, this->TetCases);

  delete [] edges;
  delete [] cases;
}

// Contour hexahedral cell------------------------------------------------------
struct HexCell : public BaseCell
{
  static unsigned short HexCases[5432];

  HexCell() : BaseCell(VTK_HEXAHEDRON)
  {
    this->NumVerts = 8;
    this->NumEdges = 12;
    this->BuildCases();
    this->Cases = this->HexCases;
  }
  ~HexCell() override {}
  void BuildCases() override;
};
// Dummy initialization filled in later at instantiation
unsigned short HexCell::HexCases[5432] = { 0 };
// Load and transform marching cubes case table. The case tables are
// repackaged for efficiency (e.g., support the GetCase() method).
void HexCell::BuildCases()
{
  int **edges = new int*[this->NumEdges];
  int numCases = std::pow(2,this->NumVerts);
  int **cases = new int*[numCases];
  for ( int i=0; i<this->NumEdges; ++i)
  {
    edges[i] = vtkHexahedron::GetEdgeArray(i);
  }
  for ( int i=0; i < numCases; ++i)
  {
    cases[i] = vtkHexahedron::GetTriangleCases(i);
  }

  BaseCell::BuildCases(numCases, edges, cases, this->HexCases);

  delete [] edges;
  delete [] cases;
}

// Contour wedge cell ------------------------------------------------------
struct WedgeCell : public BaseCell
{
  static unsigned short WedgeCases[968];

  WedgeCell() : BaseCell(VTK_WEDGE)
  {
    this->NumVerts = 6;
    this->NumEdges = 9;
    this->BuildCases();
    this->Cases = this->WedgeCases;
  }
  ~WedgeCell() override {}
  void BuildCases() override;
};
// Dummy initialization filled in later at instantiation
unsigned short WedgeCell::WedgeCases[968] = { 0 };
// Load and transform marching cubes case table. The case tables are
// repackaged for efficiency (e.g., support the GetCase() method).
void WedgeCell::BuildCases()
{
  int **edges = new int*[this->NumEdges];
  int numCases = std::pow(2,this->NumVerts);
  int **cases = new int*[numCases];
  for ( int i=0; i<this->NumEdges; ++i)
  {
    edges[i] = vtkWedge::GetEdgeArray(i);
  }
  for ( int i=0; i < numCases; ++i)
  {
    cases[i] = vtkWedge::GetTriangleCases(i);
  }

  BaseCell::BuildCases(numCases, edges, cases, this->WedgeCases);

  delete [] edges;
  delete [] cases;
}

// Contour pyramid cell------------------------------------------------------
struct PyrCell : public BaseCell
{
  static unsigned short PyrCases[448];

  PyrCell() : BaseCell(VTK_PYRAMID)
  {
    this->NumVerts = 5;
    this->NumEdges = 8;
    this->BuildCases();
    this->Cases = this->PyrCases;
  }
  ~PyrCell() override {}
  void BuildCases() override;
};
// Dummy initialization filled in later at instantiation
unsigned short PyrCell::PyrCases[448] = { 0 };
// Load and transform marching cubes case table. The case tables are
// repackaged for efficiency (e.g., support the GetCase() method).
void PyrCell::BuildCases()
{
  int **edges = new int*[this->NumEdges];
  int numCases = std::pow(2,this->NumVerts);
  int **cases = new int*[numCases];
  for ( int i=0; i<this->NumEdges; ++i)
  {
    edges[i] = vtkPyramid::GetEdgeArray(i);
  }
  for ( int i=0; i < numCases; ++i)
  {
    cases[i] = vtkPyramid::GetTriangleCases(i);
  }

  BaseCell::BuildCases(numCases, edges, cases, this->PyrCases);

  delete [] edges;
  delete [] cases;
}

// Contour voxel cell------------------------------------------------------
struct VoxCell : public BaseCell
{
  static unsigned short VoxCases[5432];

  VoxCell() : BaseCell(VTK_VOXEL)
  {
    this->NumVerts = 8;
    this->NumEdges = 12;
    this->BuildCases();
    this->Cases = this->VoxCases;
  }
  ~VoxCell() override {};
  void BuildCases() override;
};
// Dummy initialization filled in later at instantiation
unsigned short VoxCell::VoxCases[5432] = { 0 };
// Load and transform marching cubes case table. The case tables are
// repackaged for efficiency (e.g., support the GetCase() method). Note that
// the MC cases (vtkMarchingCubesTriangleCases) are specified for the
// hexahedron; voxels require a transformation to produce correct output.
void VoxCell::BuildCases()
{
  // Map the voxel points consistent with the hex edges and cases, Basically
  // the hex points (2,3,6,7) are ordered (3,2,7,6) on the voxel.
  int **edges = new int*[this->NumEdges];
  int voxEdges[12][2] = { {0,1}, {1,3}, {2,3}, {0,2}, {4,5}, {5,7},
                          {6,7}, {4,6}, {0,4}, {1,5}, {2,6}, {3,7} };

  for ( int i=0; i<this->NumEdges; ++i)
  {
    edges[i] = voxEdges[i];
  }

  // Build the voxel cases. Have to shuffle them around due to different
  // vertex ordering.
  unsigned int numCases = std::pow(2,this->NumVerts);
  int **cases = new int*[numCases];
  unsigned int hexCase, voxCase;
  for ( hexCase=0; hexCase < numCases; ++hexCase)
  {
    voxCase  = ((hexCase & BaseCell::Mask[0]) ? 1 : 0) << 0;
    voxCase |= ((hexCase & BaseCell::Mask[1]) ? 1 : 0) << 1;
    voxCase |= ((hexCase & BaseCell::Mask[2]) ? 1 : 0) << 3;
    voxCase |= ((hexCase & BaseCell::Mask[3]) ? 1 : 0) << 2;
    voxCase |= ((hexCase & BaseCell::Mask[4]) ? 1 : 0) << 4;
    voxCase |= ((hexCase & BaseCell::Mask[5]) ? 1 : 0) << 5;
    voxCase |= ((hexCase & BaseCell::Mask[6]) ? 1 : 0) << 7;
    voxCase |= ((hexCase & BaseCell::Mask[7]) ? 1 : 0) << 6;
    cases[voxCase] = vtkHexahedron::GetTriangleCases(hexCase);
  }

  BaseCell::BuildCases(numCases, edges, cases, this->VoxCases);

  delete [] edges;
  delete [] cases;
}

// Contour empty cell. These cells are skipped.---------------------------------
struct EmptyCell : public BaseCell
{
  static unsigned short EmptyCases[2];

  EmptyCell() : BaseCell(VTK_EMPTY_CELL)
  {
    this->NumVerts = 0;
    this->NumEdges = 0;
    this->Cases = this->EmptyCases;
  }
  ~EmptyCell() override {}
  void BuildCases() override {}
};
// No triangles generated
unsigned short EmptyCell::EmptyCases[2] = { 0,0 };


// This is a general iterator which assumes that the unstructured grid has a
// mix of cells. Any cell that is not processed by this contouring algorithm
// (i.e., not one of tet, hex, pyr, wedge, voxel) is skipped.
struct CellIter
{
  // Current active cell, and whether it is a copy (which controls
  // the destruction process).
  bool Copy;
  BaseCell *Cell;

  // The iteration state.
  unsigned char NumVerts;
  const unsigned short *Cases;
  vtkIdType Incr;

  // References to unstructured grid for cell traversal.
  vtkIdType NumCells;
  const unsigned char *Types;
  const vtkIdType *Conn;
  const vtkIdType *Locs;

  // All possible cell types. The iterator switches between them when
  // processing. All unsupported cells are of type EmptyCell.
  TetCell *Tet;
  HexCell *Hex;
  PyrCell *Pyr;
  WedgeCell *Wedge;
  VoxCell *Vox;
  EmptyCell *Empty;

  CellIter() : Copy(true), Cell(nullptr), NumVerts(0), Cases(nullptr), Incr(0),
               NumCells(0), Types(nullptr), Conn(nullptr), Locs(nullptr), Tet(nullptr),
               Hex(nullptr), Pyr(nullptr), Wedge(nullptr), Vox(nullptr), Empty(nullptr)
  {}

  CellIter(vtkIdType numCells, unsigned char *types, vtkIdType *conn, vtkIdType *locs) :
    Copy(false), Cell(nullptr), NumVerts(0), Cases(nullptr), Incr(0),
    NumCells(numCells), Types(types), Conn(conn), Locs(locs)
  {
    this->Tet = new TetCell;
    this->Hex = new HexCell;
    this->Pyr = new PyrCell;
    this->Wedge = new WedgeCell;
    this->Vox = new VoxCell;
    this->Empty = new EmptyCell;
  }

  ~CellIter()
  {
    if ( ! this->Copy )
    {
      delete this->Tet;
      delete this->Hex;
      delete this->Pyr;
      delete this->Wedge;
      delete this->Vox;
      delete this->Empty;
    }
  }

  CellIter(const CellIter &) = default; //remove compiler warnings

  // Shallow copy to avoid new/delete.
  CellIter& operator=(const CellIter& cellIter)
  {
    this->Copy = true;
    this->Cell = nullptr;

    this->NumVerts = cellIter.NumVerts;
    this->Cases = cellIter.Cases;
    this->Incr = cellIter.Incr;

    this->NumCells = cellIter.NumCells;
    this->Types = cellIter.Types;
    this->Conn = cellIter.Conn;
    this->Locs = cellIter.Locs;

    this->Tet = cellIter.Tet;
    this->Hex = cellIter.Hex;
    this->Pyr = cellIter.Pyr;
    this->Wedge = cellIter.Wedge;
    this->Vox = cellIter.Vox;
    this->Empty = cellIter.Empty;

    return *this;
  }

  // Decode the case table. (See previous documentation of case table
  // organization.) Note that bounds/range chacking is not performed
  // for efficiency.
  const unsigned short *GetCase(unsigned char caseNum)
  {
    return (this->Cases + this->Cases[caseNum]);
  }

  // Methods for caching traversal. Initialize() sets up the traversal
  // process; Next() advances to the next cell. Note that the public data
  // members representing the iteration state (NumVerts, Cases, Incr) are
  // modified by these methods, and then subsequently read during iteration.
  const vtkIdType* Initialize(vtkIdType cellId)
  {
    this->Cell = this->GetCell(this->Types[cellId]);
    this->NumVerts = this->Cell->NumVerts;
    this->Cases = this->Cell->Cases;

    if ( this->Cell->CellType != VTK_EMPTY_CELL )
    {
      this->Incr = this->NumVerts + 1;
    }
    else // Else have to update the increment differently
    {
      this->Incr = (cellId >= (this->NumCells-1) ? 0 :
                    this->Locs[cellId+1] - this->Locs[cellId]);
    }

    return (this->Conn + this->Locs[cellId] + 1);
  }

  vtkIdType Next(vtkIdType cellId)
  {
    // Guard against end of array condition; only update information if the
    // cell type changes. Note however that empty cells may have to be
    // treated specially.
    if ( cellId >= (this->NumCells-1) ||
         (this->Cell->CellType != VTK_EMPTY_CELL &&
          this->Cell->CellType == this->Types[cellId+1]) )
    {
      return this->Incr;
    }

    // Need to look up new information as cell type has changed.
    vtkIdType incr = this->Incr;
    this->Cell = this->GetCell(this->Types[cellId+1]);
    this->NumVerts = this->Cell->NumVerts;
    this->Cases = this->Cell->Cases;

    if ( this->Cell->CellType != VTK_EMPTY_CELL )
    {
      this->Incr = this->NumVerts + 1;
    }
    else // Else have to update the increment differently
    {
      this->Incr = this->Locs[cellId+2] - this->Locs[cellId+1];
    }

    return incr;
  }

  // Method for random access of cell, no caching
  const vtkIdType* GetCellIds(vtkIdType cellId)
  {
    this->Cell = this->GetCell(this->Types[cellId]);
    this->NumVerts = this->Cell->NumVerts;
    this->Cases = this->Cell->Cases;
    return (this->Conn + this->Locs[cellId] + 1);
  }

  // Switch to the appropriate cell type.
  BaseCell *GetCell(int cellType)
  {
    switch ( cellType )
    {
      case VTK_TETRA:
        return this->Tet;
      case VTK_HEXAHEDRON:
        return this->Hex;
      case VTK_WEDGE:
        return this->Wedge;
      case VTK_PYRAMID:
        return this->Pyr;
      case VTK_VOXEL:
        return this->Vox;
     default:
       return this->Empty;
    }
  }
};

//========================= Quick plane cut culling ===========================
// Compute an array that classifies each point with respect to the current
// plane (i.e. above the plane(=2), below the plane(=1), on the plane(=0)).
// InOutArray is allocated here and should be deleted by the invoking
// code. InOutArray is an unsigned char array to simplify bit fiddling later
// on (i.e., PlaneIntersects() method).
//
// The reason we compute this unsigned char array as compared to an array of
// function values is to reduce the amount of memory used, and the written to
// memory, since these are significant costs for large data.

// Templated for explicit point representations of real type
template <typename TP> struct ClassifyPoints;

struct Classify
{
  unsigned char* InOutArray;
  double Origin[3];
  double Normal[3];

  Classify(vtkPoints *pts, vtkPlane* plane)
  {
    this->InOutArray = new unsigned char [pts->GetNumberOfPoints()];
    plane->GetOrigin(this->Origin);
    plane->GetNormal(this->Normal);
  }

  // Check if a list of points intersects the plane
  static bool PlaneIntersects(const unsigned char *inout,
                              vtkIdType npts, const vtkIdType *pts)
  {
    unsigned char onOneSideOfPlane = inout[pts[0]];
    for ( vtkIdType i=1; onOneSideOfPlane && i < npts; ++i )
    {
      onOneSideOfPlane &= inout[pts[i]];
    }
    return (!onOneSideOfPlane);
  }
};

template <typename TP>
struct ClassifyPoints : public Classify
{
  TP *Points;

  ClassifyPoints(vtkPoints *pts, vtkPlane* plane) :
    Classify(pts,plane)
  {
    this->Points = static_cast<TP*>(pts->GetVoidPointer(0));
  }

  void operator()(vtkIdType ptId, vtkIdType endPtId)
  {
    double p[3], zero=double(0), eval;
    double *n=this->Normal, *o=this->Origin;
    TP *pts = this->Points + 3*ptId;
    unsigned char *ioa = this->InOutArray + ptId;
    for ( ; ptId < endPtId; ++ptId )
    {
      // Access each point
      p[0] = static_cast<double>(*pts); ++pts;
      p[1] = static_cast<double>(*pts); ++pts;
      p[2] = static_cast<double>(*pts); ++pts;

      // Evaluate position of the point with the plane. Invoke inline,
      // non-virtual version of evaluate method.
      eval = vtkPlane::Evaluate(n,o,p);

      // Point is either above(=2), below(=1), or on(=0) the plane.
      *ioa++ = (eval > zero ? 2 : (eval < zero ? 1 : 0));
    }
  }
};

//========================= Compute edge intersections ========================
// Use vtkStaticEdgeLocatorTemplate for edge-based point merging.
template <typename IDType,typename TIP>
struct ExtractEdgesBase
{
  typedef std::vector<EdgeTuple<IDType,float>> EdgeVectorType;

  // Track local data on a per-thread basis. In the Reduce() method this
  // information will be used to composite the data from each thread.
  struct LocalDataType
  {
    EdgeVectorType LocalEdges;
    CellIter LocalCellIter;

    LocalDataType()
    {
      this->LocalEdges.reserve(2048);
    }
  };

  const TIP *InPts;
  CellIter *Iter;
  MergeTuple<IDType,float> *Edges;
  vtkCellArray *Tris;
  vtkIdType NumTris;
  int NumThreadsUsed;
  double Origin[3];
  double Normal[3];

  // Keep track of generated points and triangles on a per thread basis
  vtkSMPThreadLocal<LocalDataType> LocalData;

  ExtractEdgesBase(TIP *inPts, CellIter *c, vtkPlane *plane, vtkCellArray *tris) :
    InPts(inPts), Iter(c), Edges(nullptr), Tris(tris),
    NumTris(0), NumThreadsUsed(0)
  {
    plane->GetNormal(this->Normal);
    plane->GetOrigin(this->Origin);
  }

  // Set up the iteration process
  void Initialize()
  {
    auto & localData = this->LocalData.Local();
    localData.LocalCellIter = *(this->Iter);
  }

  // operator() provided by subclass

  // Composite local thread data
  void Reduce()
  {
    // Count the number of triangles, and number of threads used.
    vtkIdType numTris = 0;
    this->NumThreadsUsed = 0;
    auto ldEnd = this->LocalData.end();
    for ( auto ldItr=this->LocalData.begin(); ldItr != ldEnd; ++ldItr )
    {
      numTris += static_cast<vtkIdType>(((*ldItr).LocalEdges.size() / 3)); //three edges per triangle
      this->NumThreadsUsed++;
    }

    // Allocate space for VTK triangle output.
    this->NumTris = numTris;
    this->Tris->WritePointer(this->NumTris,4*this->NumTris);

    // Copy local edges to global edge array. Add in the originating edge id
    // used later when merging.
    EdgeVectorType emptyVector;
    this->Edges = new MergeTuple<IDType,float>[3*this->NumTris]; //three edges per triangle
    vtkIdType edgeNum=0;
    for ( auto ldItr=this->LocalData.begin(); ldItr != ldEnd; ++ldItr )
    {
      auto eEnd = (*ldItr).LocalEdges.end();
      for ( auto eItr = (*ldItr).LocalEdges.begin(); eItr != eEnd; ++eItr )
      {
        this->Edges[edgeNum].V0 = eItr->V0;
        this->Edges[edgeNum].V1 = eItr->V1;
        this->Edges[edgeNum].T = eItr->T;
        this->Edges[edgeNum].EId = edgeNum;
        edgeNum++;
      }
      (*ldItr).LocalEdges.swap(emptyVector); //frees memory
    }//For all threads
  }//Reduce
};//ExtractEdgesBase

// Traverse all cells and extract intersected edges (without a sphere tree).
template <typename IDType,typename TIP>
struct ExtractEdges : public ExtractEdgesBase<IDType,TIP>
{
  const unsigned char *InOut;

  ExtractEdges(TIP *inPts, CellIter *c, vtkPlane *plane, unsigned char *inout,
               vtkCellArray *tris) :
    ExtractEdgesBase<IDType,TIP>(inPts, c, plane, tris), InOut(inout)
  {}

  // Set up the iteration process
  void Initialize()
  {
    this->ExtractEdgesBase<IDType,TIP>::Initialize();
  }

  // operator() method extracts edges from cells (edges taken three at a
  // time form a triangle)
  void operator()(vtkIdType cellId, vtkIdType endCellId)
  {
    auto & localData = this->LocalData.Local();
    auto & lEdges = localData.LocalEdges;
    CellIter *cellIter = &localData.LocalCellIter;
    const vtkIdType *c = cellIter->Initialize(cellId); //connectivity array
    unsigned short isoCase, numEdges, i;
    const unsigned short *edges;
    const TIP *x;
    double s[MAX_CELL_VERTS], deltaScalar, xp[3];
    float t;
    unsigned char v0, v1;
    const unsigned char *inout=this->InOut;

    for ( ; cellId < endCellId; ++cellId )
    {
      // Does the plane cut this cell?
      if ( Classify::PlaneIntersects(inout,cellIter->NumVerts,c) )
      {
        // Compute case by repeated masking with function value
        for ( isoCase=0, i=0; i < cellIter->NumVerts; ++i )
        {
          x = this->InPts + 3*c[i];
          xp[0] = static_cast<double>(x[0]);
          xp[1] = static_cast<double>(x[1]);
          xp[2] = static_cast<double>(x[2]);
          s[i] = vtkPlane::Evaluate(this->Normal,this->Origin,xp);
          isoCase |= ( s[i] >= 0.0 ? BaseCell::Mask[i] : 0 );
        }

        edges = cellIter->GetCase(isoCase);
        if ( *edges > 0 )
        {
          numEdges = *edges++;
          for (i=0; i<numEdges; ++i, edges+=2)
          {
            v0 = edges[0];
            v1 = edges[1];
            deltaScalar = s[v1] - s[v0];
            t = ( deltaScalar == 0.0 ? 0.0 : (-s[v0]/deltaScalar) );
            t = ( c[v0] < c[v1] ? t : (1.0-t) ); //edges (v0,v1) must have v0<v1
            lEdges.emplace_back(c[v0],c[v1],t); //edge constructor may swap v0<->v1
          }//for all edges in this case
        }//if contour passes through this cell
      }//if plane intersects
      c += cellIter->Next(cellId); //move to the next cell
    }//for all cells in this batch
  }

  // Composite local thread data
  void Reduce()
  {
    this->ExtractEdgesBase<IDType,TIP>::Reduce();
  }//Reduce
};//ExtractEdges


// Produce points for non-merged points from input edge tuples. Every edge
// produces one point; three edges in a row form a triangle. The merge edges
// contain a interpolation parameter t used to interpolate point oordinates.
// into the final VTK points array. The template parameters correspond to the
// type of input and output points.
template <typename TIP, typename TOP, typename IDType>
struct ProducePoints
{
  typedef MergeTuple<IDType,float> MergeTupleType;

  const MergeTuple<IDType,float> *Edges;
  const TIP *InPts;
  TOP *OutPts;

  ProducePoints(const MergeTuple<IDType,float> *mt, const TIP *inPts, TOP *outPts) :
    Edges(mt), InPts(inPts), OutPts(outPts) {}

  void operator()(vtkIdType ptId, vtkIdType endPtId)
  {
    const MergeTuple<IDType,float> *mergeTuple;
    const TIP *x0, *x1, *inPts=this->InPts;
    TOP *x, *outPts=this->OutPts;
    IDType v0, v1;
    float t;

    for ( ; ptId < endPtId; ++ptId )
    {
      mergeTuple = this->Edges + ptId;
      v0 = mergeTuple->V0;
      v1 = mergeTuple->V1;
      t = mergeTuple->T;
      x0 = inPts + 3*v0;
      x1 = inPts + 3*v1;
      x = outPts + 3*ptId;
      x[0] = x0[0] + t*(x1[0]-x0[0]);
      x[1] = x0[1] + t*(x1[1]-x0[1]);
      x[2] = x0[2] + t*(x1[2]-x0[2]);
    }
  }
};

// Functor to build the VTK triangle list in parallel from the generated,
// non-merged edges. Every three edges represents one triangle.
struct ProduceTriangles
{
  vtkIdType *Tris;

  ProduceTriangles(vtkIdType *tris) : Tris(tris) {}

  void operator()(vtkIdType triId, vtkIdType endTriId)
  {
    vtkIdType *tris = this->Tris + 4*triId;
    vtkIdType ptId = 3*triId;
    for ( ; triId < endTriId; ++triId )
    {
      *tris++ = 3;
      *tris++ = ptId++;
      *tris++ = ptId++;
      *tris++ = ptId++;
    }
  }
};

// If requested, interpolate point data attributes from non-merged
// points. The merge tuple contains an interpolation value t for the merged
// edge. Templated on type of id.
template <typename TIds>
struct ProduceAttributes
{
  const MergeTuple<TIds,float> *Edges; //all edges
  ArrayList *Arrays; //the list of attributes to interpolate

  ProduceAttributes(const MergeTuple<TIds,float> *mt, ArrayList *arrays) :
    Edges(mt), Arrays(arrays)
  {}

  void operator()(vtkIdType ptId, vtkIdType endPtId)
  {
    const MergeTuple<TIds,float> *mergeTuple;
    TIds v0, v1;
    float t;

    for ( ; ptId < endPtId; ++ptId )
    {
      mergeTuple = this->Edges + ptId;
      v0 = mergeTuple->V0;
      v1 = mergeTuple->V1;
      t = mergeTuple->T;
      this->Arrays->InterpolateEdge(v0,v1,t,ptId);
    }
  }
};

// This method generates the output isosurface triangle connectivity list.
template <typename IDType>
struct ProduceMergedTriangles
{
  typedef MergeTuple<IDType,float> MergeTupleType;

  const MergeTupleType *MergeArray;
  const IDType *Offsets;
  vtkIdType NumTris;
  vtkIdType *Tris;
  int NumThreadsUsed; //placeholder

  ProduceMergedTriangles(const MergeTupleType *merge, const IDType *offsets,
                         vtkIdType numTris, vtkIdType *tris) :
    MergeArray(merge), Offsets(offsets), NumTris(numTris), Tris(tris),
    NumThreadsUsed(1)
  {
  }

  void Initialize()
  {
    ;//without this method Reduce() is not called
  }

  // Loop over all merged points and update the ids of the triangle
  // connectivity.  Offsets point to the beginning of a group of equal edges:
  // all edges in the group are updated to the current merged point id.
  void operator()(vtkIdType ptId, vtkIdType endPtId)
  {
    const MergeTupleType *mergeArray = this->MergeArray;
    const IDType *offsets = this->Offsets;
    IDType i, numPtsInGroup, eid, triId;

    for ( ; ptId < endPtId; ++ptId )
    {
      numPtsInGroup = offsets[ptId+1] - offsets[ptId];
      for ( i=0; i<numPtsInGroup; ++i )
      {
        eid = mergeArray[offsets[ptId]+i].EId;
        triId = eid / 3;
        *(this->Tris + 4*triId + eid-(3*triId) + 1) = ptId;
      }//for this group of coincident edges
    }//for all merged points
  }

  // Update the triangle connectivity (numPts for each triangle. This could
  // be done in parallel but it's probably not faster.
  void Reduce()
  {
    vtkIdType *tris = this->Tris;
    for ( IDType triId=0; triId < this->NumTris; ++triId, tris+=4 )
    {
      *tris = 3;
    }
  }
};

// This method generates the output isosurface points. One point per
// merged edge is generated.
template <typename TIP, typename TOP, typename IDType>
struct ProduceMergedPoints
{
  typedef MergeTuple<IDType,float> MergeTupleType;

  const MergeTupleType *MergeArray;
  const IDType *Offsets;
  const TIP *InPts;
  TOP *OutPts;

  ProduceMergedPoints(const MergeTupleType *merge, const IDType *offsets,
                      TIP *inPts, TOP *outPts) :
    MergeArray(merge), Offsets(offsets), InPts(inPts), OutPts(outPts)
  {
  }

  void operator()(vtkIdType ptId, vtkIdType endPtId)
  {
    const MergeTupleType *mergeTuple;
    IDType v0, v1;
    const TIP *x0, *x1, *inPts=this->InPts;
    TOP *x, *outPts=this->OutPts;
    float t;

    for ( ; ptId < endPtId; ++ptId )
    {
      mergeTuple = this->MergeArray + this->Offsets[ptId];
      v0 = mergeTuple->V0;
      v1 = mergeTuple->V1;
      t = mergeTuple->T;
      x0 = inPts + 3*v0;
      x1 = inPts + 3*v1;
      x = outPts + 3*ptId;
      x[0] = x0[0] + t*(x1[0]-x0[0]);
      x[1] = x0[1] + t*(x1[1]-x0[1]);
      x[2] = x0[2] + t*(x1[2]-x0[2]);
    }
  }
};

// If requested, interpolate point data attributes. The merge tuple contains an
// interpolation value t for the merged edge.
template <typename TIds>
struct ProduceMergedAttributes
{
  const MergeTuple<TIds,float> *Edges; //all edges, sorted into groups of merged edges
  const TIds *Offsets; //refer to single, unique, merged edge
  ArrayList *Arrays; //carry list of attributes to interpolate

  ProduceMergedAttributes(const MergeTuple<TIds,float> *mt, const TIds *offsets,
                          ArrayList *arrays) :
    Edges(mt), Offsets(offsets), Arrays(arrays)
  {}

  void operator()(vtkIdType ptId, vtkIdType endPtId)
  {
    const MergeTuple<TIds,float> *mergeTuple;
    TIds v0, v1;
    float t;

    for ( ; ptId < endPtId; ++ptId )
    {
      mergeTuple = this->Edges + this->Offsets[ptId];
      v0 = mergeTuple->V0;
      v1 = mergeTuple->V1;
      t = mergeTuple->T;
      this->Arrays->InterpolateEdge(v0,v1,t,ptId);
    }
  }
};

// Wrapper to handle multiple template types for generating intersected edges
template <typename TIds>
int ProcessEdges(vtkIdType numCells, vtkPoints *inPts, CellIter *cellIter,
                 vtkPlane *plane, unsigned char *inout, vtkPoints *outPts,
                 vtkCellArray *newPolys, vtkTypeBool mergePts, vtkTypeBool intAttr,
                 vtkPointData *inPD, vtkPointData *outPD, vtkTypeBool seqProcessing,
                 int &numThreads)
{
  // Extract edges that the plane intersects.
  vtkIdType numTris=0, *tris=nullptr;
  MergeTuple<TIds,float> *mergeEdges=nullptr; //may need reference counting

  // Extract edges
  int ptsType=inPts->GetDataType();
  void *pts=inPts->GetVoidPointer(0);
  if ( ptsType == VTK_FLOAT )
  {
    ExtractEdges<TIds,float> extractEdges((float*)pts,cellIter,plane,inout,
                                          newPolys);
    EXECUTE_REDUCED_SMPFOR(seqProcessing,numCells,extractEdges,numThreads);
    numTris = extractEdges.NumTris;
    tris = newPolys->GetPointer();
    mergeEdges = extractEdges.Edges;
  }
  else //if (ptsType == VTK_DOUBLE)
  {
    ExtractEdges<TIds,double> extractEdges((double*)pts,cellIter,plane,inout,
                                           newPolys);
    EXECUTE_REDUCED_SMPFOR(seqProcessing,numCells,extractEdges,numThreads);
    numTris = extractEdges.NumTris;
    tris = newPolys->GetPointer();
    mergeEdges = extractEdges.Edges;
  }
  int nt = numThreads;

  // Make sure data was produced
  if ( numTris <= 0 )
  {
    outPts->SetNumberOfPoints(0);
    delete [] mergeEdges;
    return 1;
  }

  // There are two ways forward: do not merge coincident points; or merge
  // points. Merging typically takes longer, while the output size of
  // unmerged points is larger.
  int inPtsType = inPts->GetDataType();
  void *inPtsPtr = inPts->GetVoidPointer(0);
  int outPtsType = outPts->GetDataType();
  void *outPtsPtr;

  if ( ! mergePts )
  {
    // Produce non-merged points from edges. Each edge produces one point;
    // three edges define an output triangle.
    vtkIdType numPts = 3*numTris;
    outPts->GetData()->WriteVoidPointer(0,3*numPts);
    outPtsPtr = outPts->GetVoidPointer(0);

    if ( inPtsType == VTK_FLOAT )
    {
      if ( outPtsType == VTK_FLOAT )
      {
        ProducePoints<float,float,TIds>
          producePoints(mergeEdges,(float*)inPtsPtr,(float*)outPtsPtr);
        EXECUTE_SMPFOR(seqProcessing,numPts,producePoints);
      }
      else //outPtsType == VTK_DOUBLE
      {
        ProducePoints<float,double,TIds>
          producePoints(mergeEdges,(float*)inPtsPtr,(double*)outPtsPtr);
        EXECUTE_SMPFOR(seqProcessing,numPts,producePoints);
      }
    }
    else //inPtsType == VTK_DOUBLE
    {
      if ( outPtsType == VTK_FLOAT )
      {
        ProducePoints<double,float,TIds>
          producePoints(mergeEdges,(double*)inPtsPtr,(float*)outPtsPtr);
        EXECUTE_SMPFOR(seqProcessing,numPts,producePoints);
      }
      else //outPtsType == VTK_DOUBLE
      {
        ProducePoints<double,double,TIds>
          producePoints(mergeEdges,(double*)inPtsPtr,(double*)outPtsPtr);
        EXECUTE_SMPFOR(seqProcessing,numPts,producePoints);
      }
    }

    // Produce non-merged triangles from edges
    ProduceTriangles produceTris(tris);
    EXECUTE_SMPFOR(seqProcessing,numTris,produceTris);

    // Interpolate attributes if requested
    if ( intAttr )
    {
      ArrayList arrays;
      outPD->InterpolateAllocate(inPD,numPts);
      arrays.AddArrays(numPts,inPD,outPD);
      ProduceAttributes<TIds> interpolate(mergeEdges,&arrays);
      EXECUTE_SMPFOR(seqProcessing,numPts,interpolate);
    }
  }

  else // generate merged output
  {
    // Merge coincident edges. The Offsets refer to the single unique edge
    // from the sorted group of duplicate edges.
    vtkIdType numPts;
    vtkStaticEdgeLocatorTemplate<TIds,float> loc;
    const TIds *offsets = loc.MergeEdges(3*numTris,mergeEdges,numPts);

    // Generate triangles from merged edges.
    ProduceMergedTriangles<TIds> produceTris(mergeEdges,offsets,numTris,tris);
    EXECUTE_REDUCED_SMPFOR(seqProcessing,numPts,produceTris,numThreads);
    numThreads = nt;

    // Generate points (one per unique edge)
    outPts->GetData()->WriteVoidPointer(0,3*numPts);
    outPtsPtr = outPts->GetVoidPointer(0);

    // Only handle combinations of real types
    if ( inPtsType == VTK_FLOAT && outPtsType == VTK_FLOAT )
    {
      ProduceMergedPoints<float,float,TIds>
        producePts(mergeEdges, offsets, (float*)inPtsPtr, (float*)outPtsPtr );
      EXECUTE_SMPFOR(seqProcessing,numPts,producePts);
    }
    else if ( inPtsType == VTK_DOUBLE && outPtsType == VTK_DOUBLE )
    {
      ProduceMergedPoints<double,double,TIds>
        producePts(mergeEdges, offsets, (double*)inPtsPtr, (double*)outPtsPtr );
      EXECUTE_SMPFOR(seqProcessing,numPts,producePts);
    }
    else if ( inPtsType == VTK_FLOAT && outPtsType == VTK_DOUBLE )
    {
      ProduceMergedPoints<float,double,TIds>
        producePts(mergeEdges, offsets, (float*)inPtsPtr, (double*)outPtsPtr );
      EXECUTE_SMPFOR(seqProcessing,numPts,producePts);
    }
    else //if ( inPtsType == VTK_DOUBLE && outPtsType == VTK_FLOAT )
    {
      ProduceMergedPoints<double,float,TIds>
        producePts(mergeEdges, offsets, (double*)inPtsPtr, (float*)outPtsPtr );
      EXECUTE_SMPFOR(seqProcessing,numPts,producePts);
    }

    // Now process point data attributes if requested
    if ( intAttr )
    {
      ArrayList arrays;
      outPD->InterpolateAllocate(inPD,numPts);
      arrays.AddArrays(numPts,inPD,outPD);
      ProduceMergedAttributes<TIds> interpolate(mergeEdges,offsets,&arrays);
      EXECUTE_SMPFOR(seqProcessing,numPts,interpolate);
    }
  }

  // Clean up
  delete [] mergeEdges;
  return 1;
};

// Functor for assigning normals at each point
struct ComputePointNormals
{
  float Normal[3];
  float *PointNormals;

  ComputePointNormals(float normal[3], float *ptNormals) :
    PointNormals(ptNormals)
  {
    this->Normal[0] = normal[0];
    this->Normal[1] = normal[1];
    this->Normal[2] = normal[2];
  }

  void operator()(vtkIdType ptId, vtkIdType endPtId)
  {
    float *n = this->PointNormals + 3*ptId;

    for ( ; ptId < endPtId; ++ptId, n+=3 )
    {
      n[0] = this->Normal[0];
      n[1] = this->Normal[1];
      n[2] = this->Normal[2];
    }
  }

  static void Execute(vtkTypeBool seqProcessing, vtkPoints *pts,
                      vtkPlane *plane, vtkPointData *pd)
  {
    vtkIdType numPts = pts->GetNumberOfPoints();

    vtkFloatArray *ptNormals = vtkFloatArray::New();
    ptNormals->SetName("Normals");
    ptNormals->SetNumberOfComponents(3);
    ptNormals->SetNumberOfTuples(numPts);
    float *ptN = static_cast<float*>(ptNormals->GetVoidPointer(0));

    // Get the normal
    double dn[3];
    plane->GetNormal(dn);
    vtkMath::Normalize(dn);
    float n[3];
    n[0] = static_cast<float>(dn[0]);
    n[1] = static_cast<float>(dn[1]);
    n[2] = static_cast<float>(dn[2]);

    // Process all points, averaging normals
    ComputePointNormals compute(n, ptN);
    EXECUTE_SMPFOR(seqProcessing,numPts,compute);

    // Clean up and get out
    pd->SetNormals(ptNormals);
    ptNormals->Delete();
  }
};

}//anonymous namespace


//-----------------------------------------------------------------------------
// Construct an instance of the class.
vtk3DLinearGridPlaneCutter::vtk3DLinearGridPlaneCutter()
{
  this->Plane = vtkPlane::New();
  this->MergePoints = false;
  this->InterpolateAttributes = true;
  this->ComputeNormals = false;
  this->OutputPointsPrecision = DEFAULT_PRECISION;
  this->SequentialProcessing = false;
  this->NumberOfThreadsUsed = 0;
  this->LargeIds = false;
}

//-----------------------------------------------------------------------------
vtk3DLinearGridPlaneCutter::~vtk3DLinearGridPlaneCutter()
{
  this->SetPlane(nullptr);
}

//-----------------------------------------------------------------------------
// Overload standard modified time function. If the plane definition is modified,
// then this object is modified as well.
vtkMTimeType vtk3DLinearGridPlaneCutter::GetMTime()
{
  vtkMTimeType mTime = this->Superclass::GetMTime();
  if (this->Plane != nullptr)
  {
    vtkMTimeType mTime2 = this->Plane->GetMTime();
    return (mTime2 > mTime ? mTime2 : mTime);
  }
  else
  {
    return mTime;
  }
}

//-----------------------------------------------------------------------------
// Specialized plane cutting filter to handle unstructured grids with 3D
// linear cells (tetrahedras, hexes, wedges, pyradmids, voxels)
//
int vtk3DLinearGridPlaneCutter::
ProcessPiece(vtkUnstructuredGrid *input, vtkPlane *plane, vtkPolyData *output)
{
  // Make sure there is input data to process
  vtkPoints *inPts = input->GetPoints();
  vtkIdType numPts = inPts->GetNumberOfPoints();
  vtkCellArray *cells = input->GetCells();
  vtkIdType numCells = cells->GetNumberOfCells();
  if ( numPts <= 0 || numCells <= 0 )
  {
    vtkWarningMacro(<<"Empty input");
    return 0;
  }

  // Check the input point type. Only real types are supported.
  int inPtsType = inPts->GetDataType();
  if ( (inPtsType != VTK_FLOAT && inPtsType != VTK_DOUBLE) )
  {
    vtkErrorMacro(<<"Input point type not supported");
    return 0;
  }

  // Create the output points. Only real types are supported.
  vtkPoints *outPts = vtkPoints::New();
  if ( this->OutputPointsPrecision == vtkAlgorithm::DEFAULT_PRECISION )
  {
    outPts->SetDataType(inPts->GetDataType());
  }
  else if(this->OutputPointsPrecision == vtkAlgorithm::SINGLE_PRECISION)
  {
    outPts->SetDataType(VTK_FLOAT);
  }
  else if(this->OutputPointsPrecision == vtkAlgorithm::DOUBLE_PRECISION)
  {
    outPts->SetDataType(VTK_DOUBLE);
  }

  // Output triangles go here.
  vtkCellArray *newPolys = vtkCellArray::New();

  // Set up the cells for processing. A specialized iterator is used to traverse the cells.
  vtkIdType *conn = cells->GetPointer();
  unsigned char *cellTypes = static_cast<unsigned char*>(input->GetCellTypesArray()->GetVoidPointer(0));
  vtkIdType *locs = static_cast<vtkIdType*>(input->GetCellLocationsArray()->GetVoidPointer(0));
  CellIter *cellIter = new CellIter(numCells,cellTypes,conn,locs);

  // Compute plane-cut scalars
  unsigned char *inout=nullptr;
  int ptsType=inPts->GetDataType();
  if ( ptsType == VTK_FLOAT )
  {
    ClassifyPoints<float> classify(inPts,plane);
    vtkSMPTools::For(0,numPts, classify);
    inout = classify.InOutArray;
  }
  else if ( ptsType == VTK_DOUBLE )
  {
    ClassifyPoints<double> classify(inPts,plane);
    vtkSMPTools::For(0,numPts, classify);
    inout = classify.InOutArray;
  }

  vtkPointData *inPD = input->GetPointData();
  vtkPointData *outPD = output->GetPointData();

  // Determine the size/type of point and cell ids needed to index points
  // and cells. Using smaller ids results in a greatly reduced memory footprint
  // and faster processing.
  this->LargeIds = ( numPts >= VTK_INT_MAX || numCells >= VTK_INT_MAX ? true : false );

  // Generate all of the merged points and triangles
  if ( this->LargeIds == false )
  {
    if ( ! ProcessEdges<int>(numCells, inPts, cellIter, plane, inout,
                             outPts, newPolys, this->MergePoints,
                             this->InterpolateAttributes, inPD, outPD,
                             this->SequentialProcessing, this->NumberOfThreadsUsed) )
    {
      return 0;
    }
  }
  else
  {
    if ( ! ProcessEdges<vtkIdType>(numCells, inPts, cellIter, plane, inout,
                                   outPts, newPolys, this->MergePoints,
                                   this->InterpolateAttributes, inPD, outPD,
                                   this->SequentialProcessing, this->NumberOfThreadsUsed) )
    {
      return 0;
    }
  }

  // If requested, compute point normals. Just set the point normals to the
  // plane normal.
  if ( this->ComputeNormals )
  {
    ComputePointNormals::Execute(this->SequentialProcessing,outPts,
                                 plane,outPD);
  }

  // Report the results of execution
  vtkDebugMacro(<<"Created: " << outPts->GetNumberOfPoints() << " points, "
                << newPolys->GetNumberOfCells() << " triangles");

  // Clean up
  if ( inout != nullptr )
  {
    delete [] inout;
  }
  delete cellIter;
  output->SetPoints(outPts);
  outPts->Delete();
  output->SetPolys(newPolys);
  newPolys->Delete();

  return 1;
}

//----------------------------------------------------------------------------
// The output dataset type varies dependingon the input type.
int vtk3DLinearGridPlaneCutter::
RequestDataObject(vtkInformation*,
                  vtkInformationVector** inputVector,
                  vtkInformationVector* outputVector)
{
  vtkInformation* inInfo = inputVector[0]->GetInformationObject(0);
  if (!inInfo)
  {
    return 0;
  }

  vtkDataObject* inputDO = vtkDataObject::GetData(inputVector[0], 0);
  vtkDataObject* outputDO = vtkDataObject::GetData(outputVector, 0);
  assert(inputDO != nullptr);

  vtkInformation* outInfo = outputVector->GetInformationObject(0);

  if (vtkUnstructuredGrid::SafeDownCast(inputDO))
  {
    if (vtkPolyData::SafeDownCast(outputDO) == nullptr)
    {
      outputDO = vtkPolyData::New();
      outInfo->Set(vtkDataObject::DATA_OBJECT(), outputDO);
      outputDO->Delete();
    }
    return 1;
  }

  if (vtkCompositeDataSet::SafeDownCast(inputDO))
  {
    // For any composite dataset, we're create a vtkMultiBlockDataSet as output;
    if (vtkMultiBlockDataSet::SafeDownCast(outputDO) == nullptr)
    {
      outputDO = vtkMultiBlockDataSet::New();
      outInfo->Set(vtkDataObject::DATA_OBJECT(), outputDO);
      outputDO->Delete();
    }
    return 1;
  }

  vtkErrorMacro("Not sure what type of output to create!");
  return 0;
}

//-----------------------------------------------------------------------------
// Specialized plane cutting filter to handle unstructured grids with 3D
// linear cells (tetrahedras, hexes, wedges, pyradmids, voxels)
//
int vtk3DLinearGridPlaneCutter::
RequestData(vtkInformation*, vtkInformationVector** inputVector,
            vtkInformationVector* outputVector)
{
  // Get the input and output
  vtkInformation* inInfo = inputVector[0]->GetInformationObject(0);
  vtkInformation* outInfo = outputVector->GetInformationObject(0);

  vtkUnstructuredGrid *inputGrid =
    vtkUnstructuredGrid::SafeDownCast(inInfo->Get(vtkDataObject::DATA_OBJECT()));
  vtkPolyData *outputPolyData =
    vtkPolyData::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));

  vtkCompositeDataSet *inputCDS =
    vtkCompositeDataSet::SafeDownCast(inInfo->Get(vtkDataObject::DATA_OBJECT()));
  vtkMultiBlockDataSet *outputMBDS =
    vtkMultiBlockDataSet::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));

  // Make sure we have valid input and output of some form
  if ( (inputGrid == nullptr  || outputPolyData == nullptr) &&
       (inputCDS == nullptr  || outputMBDS == nullptr) )
  {
    return 0;
  }

  // Need a plane to do the cutting
  vtkPlane *plane=this->Plane;
  if ( ! plane )
  {
    vtkErrorMacro(<<"Cut plane not defined");
    return 0;
  }

  // If the input is an unstructured grid, then simply process this single
  // grid producing a single output vtkPolyData.
  if ( inputGrid )
  {
    this->ProcessPiece(inputGrid, plane, outputPolyData);
  }

  // Otherwise it is an input composite data set and each unstructured grid
  // contained in it is processed, producing a vtkPolyData that is added to
  // the output multiblock dataset.
  else
  {
    vtkUnstructuredGrid *grid;
    vtkPolyData *polydata;
    outputMBDS->CopyStructure(inputCDS);
    vtkSmartPointer<vtkCompositeDataIterator> inIter;
    inIter.TakeReference(inputCDS->NewIterator());
    for (inIter->InitTraversal(); !inIter->IsDoneWithTraversal(); inIter->GoToNextItem())
    {
      auto ds = inIter->GetCurrentDataObject();
      if ( (grid=vtkUnstructuredGrid::SafeDownCast(ds)) )
      {
        polydata = vtkPolyData::New();
        this->ProcessPiece(grid, plane, polydata);
        outputMBDS->SetDataSet(inIter, polydata);
        polydata->Delete();
      }
      else
      {
        vtkDebugMacro(<<"This filter only processes unstructured grids");
      }
    }
  }

  return 1;
}

//-----------------------------------------------------------------------------
void vtk3DLinearGridPlaneCutter::SetOutputPointsPrecision(int precision)
{
  this->OutputPointsPrecision = precision;
  this->Modified();
}

//-----------------------------------------------------------------------------
int vtk3DLinearGridPlaneCutter::GetOutputPointsPrecision() const
{
  return this->OutputPointsPrecision;
}

//-----------------------------------------------------------------------------
int vtk3DLinearGridPlaneCutter::FillInputPortInformation(int, vtkInformation *info)
{
  info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkUnstructuredGrid");
  info->Append(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkCompositeDataSet");
  return 1;
}

//-----------------------------------------------------------------------------
void vtk3DLinearGridPlaneCutter::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);

  os << indent << "Plane: " << this->Plane << "\n";

  os << indent << "Merge Points: "
     << (this->MergePoints ? "true\n" : "false\n");
  os << indent << "Interpolate Attributes: "
     << (this->InterpolateAttributes ? "true\n" : "false\n");
  os << indent << "Compute Normals: "
     << (this->ComputeNormals ? "true\n" : "false\n");

  os << indent << "Precision of the output points: "
     << this->OutputPointsPrecision << "\n";

  os << indent << "Sequential Processing: "
     << (this->SequentialProcessing ? "true\n" : "false\n");
  os << indent << "Large Ids: "
     << (this->LargeIds ? "true\n" : "false\n");

}

#undef EXECUTE_SMPFOR
#undef EXECUTE_REDUCED_SMPFOR
#undef MAX_CELL_VERTS
