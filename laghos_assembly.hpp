// Copyright (c) 2017, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-XXXXXX. All Rights
// reserved. See file LICENSE for details.
//
// This file is part of CEED, a collection of benchmarks, miniapps, software
// libraries and APIs for efficient high-order finite element and spectral
// element discretizations for exascale applications. For more information and
// source code availability see http://github.com/ceed.
//
// The CEED research is supported by the Exascale Computing Project (17-SC-20-SC)
// a collaborative effort of two U.S. Department of Energy organizations (Office
// of Science and the National Nuclear Security Administration) responsible for
// the planning and preparation of a capable exascale ecosystem, including
// software, applications, hardware, advanced system engineering and early
// testbed platforms, in support of the nation’s exascale computing imperative.

#ifndef MFEM_LAGHOS_ASSEMBLY
#define MFEM_LAGHOS_ASSEMBLY

#include "mfem.hpp"


#ifdef MFEM_USE_MPI

#include <memory>
#include <iostream>

namespace mfem
{

namespace hydrodynamics
{

// Container for all data needed at quadrature points.
struct QuadratureData
{
   // TODO: use QuadratureFunctions?

   // At each quadrature point, the stress and the Jacobian are (dim x dim)
   // matrices. They must be recomputed in every time step.
   DenseTensor stress, Jac;

   // Reference to physical Jacobian for the initial mesh. These are computed
   // only at time zero and stored here.
   DenseTensor Jac0inv;

   // TODO: have this only when PA is on.
   // Quadrature data used for partial assembly of the force operator. It must
   // be recomputed in every time step.
   DenseTensor stressJinvT;

   // At time zero, we compute and store rho0 * det(J0) at the chosen quadrature
   // points. Then at any other time, we compute rho = rho0 * det(J0) / det(J),
   // representing the notion of pointwise mass conservation.
   Vector rho0DetJ0;

   // TODO: have this only when PA is on.
   // Quadrature data used for partial assembly of the mass matrices, namely
   // (rho * detJ * qp_weight) at each quadrature point. These are computed only
   // at time zero and stored here.
   Vector rhoDetJw;

   // Initial length scale. This represents a notion of local mesh size. We
   // assume that all initial zones have similar size.
   double h0;

   // Estimate of the minimum time step over all quadrature points. This is
   // recomputed at every time step to achieve adaptive time stepping.
   double dt_est;

   QuadratureData(int dim, int nzones, int quads_per_zone)
      : stress(dim, dim, nzones * quads_per_zone),
        Jac(dim, dim, nzones * quads_per_zone),
        Jac0inv(dim, dim, nzones * quads_per_zone),
        stressJinvT(nzones * quads_per_zone, dim, dim),
        rho0DetJ0(nzones * quads_per_zone),
        rhoDetJw(nzones * quads_per_zone) { }
};

// Stores values of the one-dimensional shape functions and gradients at all 1D
// quadrature points. All sizes are (dofs1D_cnt x quads1D_cnt).
struct Tensors1D
{
   // H1 shape functions and gradients, L2 shape functions.
   DenseMatrix HQshape1D, HQgrad1D, LQshape1D;

   Tensors1D(int H1order, int L2order, int nqp1D);
};
extern const Tensors1D *tensors1D;

// This class is used only for visualization. It assembles (rho, phi) in each
// zone, which is used by LagrangianHydroOperator::ComputeDensity to do an L2
// projection of the density.
class DensityIntegrator : public LinearFormIntegrator
{
private:
   const QuadratureData &quad_data;

public:
   DensityIntegrator(QuadratureData &quad_data_) : quad_data(quad_data_) { }

   virtual void AssembleRHSElementVect(const FiniteElement &fe,
                                       ElementTransformation &Tr,
                                       Vector &elvect);
};

// Assembles element contributions to the global force matrix. This class is
// used for the full assembly case; it's not used with partial assembly.
class ForceIntegrator : public BilinearFormIntegrator
{
private:
   const QuadratureData &quad_data;

public:
   ForceIntegrator(QuadratureData &quad_data_) : quad_data(quad_data_) { }

   virtual void AssembleElementMatrix2(const FiniteElement &trial_fe,
                                       const FiniteElement &test_fe,
                                       ElementTransformation &Trans,
                                       DenseMatrix &elmat);
};

// Performs partial assembly, which corresponds to (and replaces) the use of
// the LagrangianHydroOperator::Force global matrix.
class ForcePAOperator : public Operator
{
private:
   const int dim, nzones;

   QuadratureData *quad_data;
   ParFiniteElementSpace &H1FESpace, &L2FESpace;

   void MultQuad(const Vector &vecL2, Vector &vecH1) const;
   void MultHex(const Vector &vecL2, Vector &vecH1) const;

   void MultTransposeQuad(const Vector &vecH1, Vector &vecL2) const;
   void MultTransposeHex(const Vector &vecH1, Vector &vecL2) const;

public:
   ForcePAOperator(QuadratureData *quad_data_,
                   ParFiniteElementSpace &h1fes, ParFiniteElementSpace &l2fes)
      : dim(h1fes.GetMesh()->Dimension()), nzones(h1fes.GetMesh()->GetNE()),
        quad_data(quad_data_), H1FESpace(h1fes), L2FESpace(l2fes) { }

   virtual void Mult(const Vector &vecL2, Vector &vecH1) const;
   virtual void MultTranspose(const Vector &vecH1, Vector &vecL2) const;

   ~ForcePAOperator() { }
};

// Performs partial assembly for the velocity and energy mass matrices.
class MassPAOperator : public Operator
{
private:
   const int dim, nzones;

   QuadratureData *quad_data;
   ParFiniteElementSpace &FESpace;

   Array<int> *ess_tdofs;

   mutable ParGridFunction x_gf, y_gf;

   void MultQuad(const Vector &x, Vector &y) const;
   void MultHex(const Vector &x, Vector &y) const;

public:
   MassPAOperator(QuadratureData *quad_data_, ParFiniteElementSpace &fes)
      : Operator(fes.TrueVSize()),
        dim(fes.GetMesh()->Dimension()), nzones(fes.GetMesh()->GetNE()),
        quad_data(quad_data_), FESpace(fes), ess_tdofs(NULL),
        x_gf(&fes), y_gf(&fes)
   { }

   // Can be used for both velocity and specific internal energy.
   // For the case of velocity, we only work with one component at a time.
   virtual void Mult(const Vector &x, Vector &y) const;

   void EliminateRHS(Array<int> &dofs, Vector &b)
   {
      ess_tdofs = &dofs;
      for (int i = 0; i < dofs.Size(); i++) { b(dofs[i]) = 0.0; }
   }
};

} // namespace hydrodynamics

} // namespace mfem

#endif // MFEM_USE_MPI

#endif // MFEM_LAGHOS_ASSEMBLY