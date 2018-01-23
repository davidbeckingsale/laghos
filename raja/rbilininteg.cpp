// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.
#include "raja.hpp"

namespace mfem {

// ***************************************************************************
std::map<std::string, RajaDofQuadMaps*> RajaDofQuadMaps::AllDofQuadMaps;

// ***************************************************************************
// * RajaGeometry
// ***************************************************************************
static RajaGeometry *geom=NULL;

RajaGeometry::~RajaGeometry(){
  free(geom->meshNodes);
  free(geom->J);
  free(geom->invJ);
  free(geom->detJ);
  delete[] geom;
}

  // ***************************************************************************
  void RajaGeometry::ReorderByVDim(GridFunction& nodes){
    const FiniteElementSpace *fes=nodes.FESpace();
    const int size = nodes.Size();
    const int vdim = fes->GetVDim();
    const int ndofs = fes->GetNDofs();
    double *data = nodes.GetData();
    double *temp = new double[size];
    int k=0;
    for (int j = 0; j < ndofs; j++)
      for (int i = 0; i < vdim; i++){
        temp[k++] = data[j+i*ndofs];
      }
    for (int i = 0; i < size; i++)
      data[i] = temp[i];
    delete [] temp;
  }

  // ***************************************************************************
  void RajaGeometry::ReorderByNodes(GridFunction& nodes){
    const FiniteElementSpace *fes=nodes.FESpace();
    const int size = nodes.Size();
    const int vdim = fes->GetVDim();
    const int ndofs = fes->GetNDofs();
    double *data = nodes.GetData();
    double *temp = new double[size];
    int k = 0;
    for (int j = 0; j < ndofs; j++)
      for (int i = 0; i < vdim; i++){
        temp[j+i*ndofs] = data[k++];
      }
    for (int i = 0; i < size; i++){
      data[i] = temp[i];
    }
    delete [] temp;
  }
  
// *****************************************************************************
RajaGeometry* RajaGeometry::Get(RajaFiniteElementSpace& fes,
                                const IntegrationRule& ir) {
  geom=new RajaGeometry();
  Mesh& mesh = *(fes.GetMesh());
  if (!mesh.GetNodes()) {
    mesh.SetCurvature(1, false, -1, Ordering::byVDIM);
  }
  GridFunction& nodes = *(mesh.GetNodes());
  const FiniteElementSpace& fespace = *(nodes.FESpace());
  const FiniteElement& fe = *(fespace.GetFE(0));
  const int dims     = fe.GetDim();
  const int elements = fespace.GetNE();
  const int numDofs  = fe.GetDof();
  const int numQuad  = ir.GetNPoints();
  //Ordering::Type originalOrdering = fespace.GetOrdering();
  const bool orderedByNODES = (fespace.GetOrdering() == Ordering::byNODES);
  if (orderedByNODES) ReorderByVDim(nodes);
  Array<double> meshNodes(dims*numDofs*elements);
  const Table& e2dTable = fespace.GetElementToDofTable();
  const int* elementMap = e2dTable.GetJ();
  for (int e = 0; e < elements; ++e) {
    for (int dof = 0; dof < numDofs; ++dof) {
      const int gid = elementMap[dof + numDofs*e];
      for (int dim = 0; dim < dims; ++dim) {
        meshNodes[dim+dims*(dof+numDofs*e)] = nodes[dim + gid*dims];
      }
    }
  }
  geom->meshNodes.allocate(dims, numDofs, elements);
  geom->meshNodes = meshNodes;
  // Reorder the original gf back
  if (orderedByNODES) ReorderByNodes(nodes);
  geom->J.allocate(dims, dims, numQuad, elements);
  geom->invJ.allocate(dims, dims, numQuad, elements);
  geom->detJ.allocate(numQuad, elements);
  const RajaDofQuadMaps* maps = RajaDofQuadMaps::GetSimplexMaps(fe, ir);
  assert(maps);
  rIniGeom(dims,numDofs,numQuad,elements,
           maps->dofToQuadD,
           geom->meshNodes,
           geom->J,
           geom->invJ,
           geom->detJ);
  return geom;
}


// ***************************************************************************
// * RajaDofQuadMaps
// ***************************************************************************

RajaDofQuadMaps* RajaDofQuadMaps::Get(const RajaFiniteElementSpace& fespace,
                                      const IntegrationRule& ir,
                                      const bool transpose) {
  return Get(*fespace.GetFE(0),*fespace.GetFE(0),ir,transpose);
}

RajaDofQuadMaps* RajaDofQuadMaps::Get(const RajaFiniteElementSpace&
                                      trialFESpace,
                                      const RajaFiniteElementSpace& testFESpace,
                                      const IntegrationRule& ir,
                                      const bool transpose) {
  return Get(*trialFESpace.GetFE(0),*testFESpace.GetFE(0),ir,transpose);
}

RajaDofQuadMaps* RajaDofQuadMaps::Get(const FiniteElement& trialFE,
                                      const FiniteElement& testFE,
                                      const IntegrationRule& ir,
                                      const bool transpose) {
  return GetTensorMaps(trialFE, testFE, ir, transpose);
}

// ***************************************************************************
RajaDofQuadMaps* RajaDofQuadMaps::GetTensorMaps(const FiniteElement& trialFE,
                                                const FiniteElement& testFE,
                                                const IntegrationRule& ir,
                                                const bool transpose) {
  const TensorBasisElement& trialTFE =
    dynamic_cast<const TensorBasisElement&>(trialFE);
  const TensorBasisElement& testTFE =
    dynamic_cast<const TensorBasisElement&>(testFE);
  std::stringstream ss;
  ss << "Tensor"
     << "O1:"  << trialFE.GetOrder()
     << "O2:"  << testFE.GetOrder()
     << "BT1:" << trialTFE.GetBasisType()
     << "BT2:" << testTFE.GetBasisType()
     << "Q:"   << ir.GetNPoints();
  std::string hash = ss.str();
  
  // If we've already made the dof-quad maps, reuse them
  if (AllDofQuadMaps.find(hash)!=AllDofQuadMaps.end())
    return AllDofQuadMaps[hash];
  // Otherwise, build them
  RajaDofQuadMaps *maps = new RajaDofQuadMaps();
  AllDofQuadMaps[hash]=maps;
  maps->hash = hash;
  const RajaDofQuadMaps* trialMaps = GetD2QTensorMaps(trialFE, ir);
  const RajaDofQuadMaps* testMaps  = GetD2QTensorMaps(testFE, ir, true);
  maps->dofToQuad   = trialMaps->dofToQuad;
  maps->dofToQuadD  = trialMaps->dofToQuadD;
  maps->quadToDof   = testMaps->dofToQuad;
  maps->quadToDofD  = testMaps->dofToQuadD;
  maps->quadWeights = testMaps->quadWeights;
  return maps;
}
  
// ***************************************************************************
RajaDofQuadMaps* RajaDofQuadMaps::GetD2QTensorMaps(const FiniteElement& fe,
                                                   const IntegrationRule& ir,
                                                   const bool transpose) {
  const TensorBasisElement& tfe = dynamic_cast<const TensorBasisElement&>(fe);
  const Poly_1D::Basis& basis = tfe.GetBasis1D();
  const int order = fe.GetOrder();
  const int dofs = order + 1;
  const int dims = fe.GetDim();
  const IntegrationRule& ir1D = IntRules.Get(Geometry::SEGMENT, ir.GetOrder());
  const int quadPoints = ir1D.GetNPoints();
  const int quadPoints2D = quadPoints*quadPoints;
  const int quadPoints3D = quadPoints2D*quadPoints;
  const int quadPointsND = ((dims == 1) ? quadPoints :
                            ((dims == 2) ? quadPoints2D : quadPoints3D));
  RajaDofQuadMaps *maps = new RajaDofQuadMaps();
  maps->dofToQuad.allocate(quadPoints, dofs,1,1,transpose);
  maps->dofToQuadD.allocate(quadPoints, dofs,1,1,transpose);
  double* quadWeights1DData = NULL;
  if (transpose) {
    // Initialize quad weights only for transpose
    maps->quadWeights.allocate(quadPointsND);
    quadWeights1DData = ::new double[quadPoints];
  }
  mfem::Vector d2q(dofs); 
  mfem::Vector d2qD(dofs);
  Array<double> dofToQuad(quadPoints*dofs);
  Array<double> dofToQuadD(quadPoints*dofs);
  for (int q = 0; q < quadPoints; ++q) {
    const IntegrationPoint& ip = ir1D.IntPoint(q);
    basis.Eval(ip.x, d2q, d2qD);
    if (transpose) {
      quadWeights1DData[q] = ip.weight;
    }
    for (int d = 0; d < dofs; ++d) {
      dofToQuad[maps->dofToQuad.dim()[0]*q + maps->dofToQuad.dim()[1]*d] = d2q[d];
      dofToQuadD[maps->dofToQuad.dim()[0]*q + maps->dofToQuad.dim()[1]*d] = d2qD[d];
    }
  }
  maps->dofToQuad = dofToQuad;
  maps->dofToQuadD = dofToQuadD;
  
  if (transpose) {
    Array<double> quadWeights(quadPointsND);
    for (int q = 0; q < quadPointsND; ++q) {
      const int qx = q % quadPoints;
      const int qz = q / quadPoints2D;
      const int qy = (q - qz*quadPoints2D) / quadPoints;
      double w = quadWeights1DData[qx];
      if (dims > 1) {
        w *= quadWeights1DData[qy];
      }
      if (dims > 2) {
        w *= quadWeights1DData[qz];
      }
      quadWeights[q] = w;
    }
    maps->quadWeights = quadWeights;
    ::delete [] quadWeights1DData;
  }
  assert(maps);
  return maps;
}

// ***************************************************************************
RajaDofQuadMaps* RajaDofQuadMaps::GetSimplexMaps(const FiniteElement& fe,
                                                 const IntegrationRule& ir,
                                                 const bool transpose) {
  return GetSimplexMaps(fe, fe, ir, transpose);
}

// *****************************************************************************
RajaDofQuadMaps* RajaDofQuadMaps::GetSimplexMaps(const FiniteElement& trialFE,
                                                 const FiniteElement& testFE,
                                                 const IntegrationRule& ir,
                                                 const bool transpose) {
  std::stringstream ss;
  ss << "Simplex"
     << "O1:" << trialFE.GetOrder()
     << "O2:" << testFE.GetOrder()
     << "Q:"  << ir.GetNPoints();
  std::string hash = ss.str();
  // If we've already made the dof-quad maps, reuse them
  if (AllDofQuadMaps.find(hash)!=AllDofQuadMaps.end())
    return AllDofQuadMaps[hash];
  RajaDofQuadMaps *maps = new RajaDofQuadMaps();
  AllDofQuadMaps[hash]=maps;
  maps->hash = hash;
  const RajaDofQuadMaps* trialMaps = GetD2QSimplexMaps(trialFE, ir);
  const RajaDofQuadMaps* testMaps  = GetD2QSimplexMaps(testFE, ir, true);
  maps->dofToQuad   = trialMaps->dofToQuad;
  maps->dofToQuadD  = trialMaps->dofToQuadD;
  maps->quadToDof   = testMaps->dofToQuad;
  maps->quadToDofD  = testMaps->dofToQuadD;
  maps->quadWeights = testMaps->quadWeights;
  assert(maps);
  return maps;
}

// ***************************************************************************
RajaDofQuadMaps* RajaDofQuadMaps::GetD2QSimplexMaps(const FiniteElement& fe,
                                                   const IntegrationRule& ir,
                                                   const bool transpose) {
  const int dims = fe.GetDim();
  const int numDofs = fe.GetDof();
  const int numQuad = ir.GetNPoints();
  RajaDofQuadMaps* maps = new RajaDofQuadMaps();
  // Initialize the dof -> quad mapping
  maps->dofToQuad.allocate(numQuad, numDofs,1,1,transpose);
  maps->dofToQuadD.allocate(dims, numQuad, numDofs,1,transpose);
  if (transpose) // Initialize quad weights only for transpose
    maps->quadWeights.allocate(numQuad);
  Vector d2q(numDofs);
  DenseMatrix d2qD(numDofs, dims);
  Array<double> quadWeights(numQuad);
  Array<double> dofToQuad(numQuad*numDofs);
  Array<double> dofToQuadD(dims*numQuad*numDofs);  
  for (int q = 0; q < numQuad; ++q) {
    const IntegrationPoint& ip = ir.IntPoint(q);
    if (transpose) {
      quadWeights[q] = ip.weight;
    }
    fe.CalcShape(ip, d2q);
    fe.CalcDShape(ip, d2qD);
    for (int d = 0; d < numDofs; ++d) {
      const double w = d2q[d];
      dofToQuad[maps->dofToQuad.dim()[0]*q +
                maps->dofToQuad.dim()[1]*d] = w;
      for (int dim = 0; dim < dims; ++dim) {
        const double wD = d2qD(d, dim);
        dofToQuadD[maps->dofToQuadD.dim()[0]*dim +
                   maps->dofToQuadD.dim()[1]*q +
                   maps->dofToQuadD.dim()[2]*d] = wD;
      }
    }
  }
  if (transpose) 
    maps->quadWeights = quadWeights;
  maps->dofToQuad = dofToQuad;
  maps->dofToQuadD = dofToQuadD;
  assert(maps);
  return maps;
}


// ***************************************************************************
// * Base Integrator
// ***************************************************************************

void RajaIntegrator::SetIntegrationRule(const IntegrationRule& ir_) {
  ir = &ir_;
}

const IntegrationRule& RajaIntegrator::GetIntegrationRule() const {
  assert(ir);
  return *ir;
}

void RajaIntegrator::SetupIntegrator(RajaBilinearForm& bform_,
                                     const RajaIntegratorType itype_) {
  mesh = &(bform_.GetMesh());
  trialFESpace = &(bform_.GetTrialFESpace());
  testFESpace  = &(bform_.GetTestFESpace());
  itype = itype_;
  if (ir == NULL) assert(false);
  maps = RajaDofQuadMaps::Get(*trialFESpace,*testFESpace,*ir);
  mapsTranspose = RajaDofQuadMaps::Get(*testFESpace,*trialFESpace,*ir);
  Setup();
}

RajaGeometry* RajaIntegrator::GetGeometry() {
  return RajaGeometry::Get(*trialFESpace, *ir);
}


// ***************************************************************************
// * Mass Integrator
// ***************************************************************************
void RajaMassIntegrator::SetupIntegrationRule() {
  const FiniteElement& trialFE = *(trialFESpace->GetFE(0));
  const FiniteElement& testFE  = *(testFESpace->GetFE(0));
  assert(false);
}

// ***************************************************************************
void RajaMassIntegrator::Assemble() {
  if (op.Size()) return;
  assert(false);
}

// ***************************************************************************
void RajaMassIntegrator::SetOperator(RajaVector& v) { op = v; }

// ***************************************************************************
void RajaMassIntegrator::MultAdd(RajaVector& x, RajaVector& y) {
  const int dim = mesh->Dimension();
  const int quad1D  = IntRules.Get(Geometry::SEGMENT,ir->GetOrder()).GetNPoints();
  const int dofs1D =trialFESpace->GetFE(0)->GetOrder() + 1;
  if (use_share)
    rMassMultAddS(dim,
                  dofs1D,
                  quad1D,
                  mesh->GetNE(),
                  maps->dofToQuad,
                  maps->dofToQuadD,
                  maps->quadToDof,
                  maps->quadToDofD,
                  op,x,y);
  else
    rMassMultAdd(dim,
                 dofs1D,
                 quad1D,
                 mesh->GetNE(),
                 maps->dofToQuad,
                 maps->dofToQuadD,
                 maps->quadToDof,
                 maps->quadToDofD,
                 op,x,y);
}
}

