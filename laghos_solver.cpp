// Copyright (c) 2017, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-734707. All Rights
// reserved. See files LICENSE and NOTICE for details.
//
// This file is part of CEED, a collection of benchmarks, miniapps, software
// libraries and APIs for efficient high-order finite element and spectral
// element discretizations for exascale applications. For more information and
// source code availability see http://github.com/ceed.
//
// The CEED research is supported by the Exascale Computing Project 17-SC-20-SC,
// a collaborative effort of two U.S. Department of Energy organizations (Office
// of Science and the National Nuclear Security Administration) responsible for
// the planning and preparation of a capable exascale ecosystem, including
// software, applications, hardware, advanced system engineering and early
// testbed platforms, in support of the nation's exascale computing imperative.

#include "laghos_solver.hpp"

#ifdef MFEM_USE_MPI

using namespace std;

namespace mfem
{

namespace miniapps
{

void VisualizeField(socketstream &sock, const char *vishost, int visport,
                    ParGridFunction &gf, const char *title,
                    int x, int y, int w, int h, bool vec)
{
   ParMesh &pmesh = *gf.ParFESpace()->GetParMesh();
   MPI_Comm comm = pmesh.GetComm();

   int num_procs, myid;
   MPI_Comm_size(comm, &num_procs);
   MPI_Comm_rank(comm, &myid);

   bool newly_opened = false;
   int connection_failed;

   do
   {
      if (myid == 0)
      {
         if (!sock.is_open() || !sock)
         {
            sock.open(vishost, visport);
            sock.precision(8);
            newly_opened = true;
         }
         sock << "solution\n";
      }

      pmesh.PrintAsOne(sock);
      gf.SaveAsOne(sock);

      if (myid == 0 && newly_opened)
      {
         sock << "window_title '" << title << "'\n"
              << "window_geometry "
              << x << " " << y << " " << w << " " << h << "\n"
              << "keys maaAc";
         if ( vec ) { sock << "vvv"; }
         sock << endl;
      }

      if (myid == 0)
      {
         connection_failed = !sock && !newly_opened;
      }
      MPI_Bcast(&connection_failed, 1, MPI_INT, 0, comm);
   }
   while (connection_failed);
}

} // namespace miniapps

namespace hydrodynamics
{

LagrangianHydroOperator::LagrangianHydroOperator(int size,
                                                 ParFiniteElementSpace &h1_fes,
                                                 ParFiniteElementSpace &l2_fes,
                                                 Array<int> &essential_tdofs,
                                                 ParGridFunction &rho0,
                                                 int source_type_, double cfl_,
                                                 double gamma_, bool visc,
                                                 bool pa)
   : TimeDependentOperator(size),
     H1FESpace(h1_fes), L2FESpace(l2_fes),
     H1compFESpace(h1_fes.GetParMesh(), h1_fes.FEColl(), 1),
     ess_tdofs(essential_tdofs),
     dim(h1_fes.GetMesh()->Dimension()),
     zones_cnt(h1_fes.GetMesh()->GetNE()),
     l2dofs_cnt(l2_fes.GetFE(0)->GetDof()),
     h1dofs_cnt(h1_fes.GetFE(0)->GetDof()),
     source_type(source_type_), cfl(cfl_), gamma(gamma_),
     use_viscosity(visc), p_assembly(pa),
     Mv(&h1_fes), Me_inv(l2dofs_cnt, l2dofs_cnt, zones_cnt),
     integ_rule(IntRules.Get(h1_fes.GetMesh()->GetElementBaseGeometry(),
                             3*h1_fes.GetOrder(0) + l2_fes.GetOrder(0) - 1)),
     quad_data(dim, zones_cnt, integ_rule.GetNPoints()),
     quad_data_is_current(false),
     Force(&l2_fes, &h1_fes), ForcePA(&quad_data, h1_fes, l2_fes)
{
   GridFunctionCoefficient rho_coeff(&rho0);

   // Standard local assembly and inversion for energy mass matrices.
   DenseMatrix Me(l2dofs_cnt);
   DenseMatrixInverse inv(&Me);
   MassIntegrator mi(rho_coeff, &integ_rule);
   for (int i = 0; i < zones_cnt; i++)
   {
      mi.AssembleElementMatrix(*l2_fes.GetFE(i),
                               *l2_fes.GetElementTransformation(i), Me);
      inv.Factor();
      inv.GetInverseMatrix(Me_inv(i));
   }

   // Standard assembly for the velocity mass matrix.
   VectorMassIntegrator *vmi = new VectorMassIntegrator(rho_coeff, &integ_rule);
   Mv.AddDomainIntegrator(vmi);
   Mv.Assemble();

   // Values of rho0DetJ0 and Jac0inv at all quadrature points.
   const int nqp = integ_rule.GetNPoints();
   Vector rho_vals(nqp);
   for (int i = 0; i < zones_cnt; i++)
   {
      rho0.GetValues(i, integ_rule, rho_vals);
      ElementTransformation *T = h1_fes.GetElementTransformation(i);
      for (int q = 0; q < nqp; q++)
      {
         const IntegrationPoint &ip = integ_rule.IntPoint(q);
         T->SetIntPoint(&ip);

         DenseMatrixInverse Jinv(T->Jacobian());
         Jinv.GetInverseMatrix(quad_data.Jac0inv(i*nqp + q));

         const double rho0DetJ0 = T->Weight() * rho_vals(q);
         quad_data.rho0DetJ0w(i*nqp + q) = rho0DetJ0 *
                                           integ_rule.IntPoint(q).weight;
      }
   }

   // Initial local mesh size (assumes similar cells).
   double loc_area = 0.0, glob_area;
   int glob_z_cnt;
   ParMesh *pm = H1FESpace.GetParMesh();
   for (int i = 0; i < zones_cnt; i++) { loc_area += pm->GetElementVolume(i); }
   MPI_Allreduce(&loc_area, &glob_area, 1, MPI_DOUBLE, MPI_SUM, pm->GetComm());
   MPI_Allreduce(&zones_cnt, &glob_z_cnt, 1, MPI_INT, MPI_SUM, pm->GetComm());
   switch (pm->GetElementBaseGeometry(0))
   {
      case Geometry::SQUARE:
         quad_data.h0 = sqrt(glob_area / glob_z_cnt); break;
      case Geometry::TRIANGLE:
         quad_data.h0 = sqrt(2.0 * glob_area / glob_z_cnt); break;
      case Geometry::CUBE:
         quad_data.h0 = pow(glob_area / glob_z_cnt, 1.0/3.0); break;
      case Geometry::TETRAHEDRON:
         quad_data.h0 = pow(6.0 * glob_area / glob_z_cnt, 1.0/3.0); break;
      default: MFEM_ABORT("Unknown zone type!");
   }
   quad_data.h0 /= (double) H1FESpace.GetOrder(0);

   ForceIntegrator *fi = new ForceIntegrator(quad_data);
   fi->SetIntRule(&integ_rule);
   Force.AddDomainIntegrator(fi);
   // Make a dummy assembly to figure out the sparsity.
   Force.Assemble(0);
   Force.Finalize(0);

   if (p_assembly)
   {
      tensors1D = new Tensors1D(H1FESpace.GetFE(0)->GetOrder(),
                                L2FESpace.GetFE(0)->GetOrder(),
                                int(floor(0.7 + pow(nqp, 1.0 / dim))));
   }
}

void LagrangianHydroOperator::Mult(const Vector &S, Vector &dS_dt) const
{
   dS_dt = 0.0;

   // Make sure that the mesh positions correspond to the ones in S. This is
   // needed only because some mfem time integrators don't update the solution
   // vector at every intermediate stage (hence they don't change the mesh).
   Vector* sptr = (Vector*) &S;
   ParGridFunction x;
   x.MakeRef(&H1FESpace, *sptr, 0);
   H1FESpace.GetParMesh()->NewNodes(x, false);

   UpdateQuadratureData(S);

   // The monolithic BlockVector stores the unknown fields as follows:
   // - Position
   // - Velocity
   // - Specific Internal Energy

   const int Vsize_l2 = L2FESpace.GetVSize();
   const int Vsize_h1 = H1FESpace.GetVSize();

   ParGridFunction v, e;
   v.MakeRef(&H1FESpace, *sptr, Vsize_h1);
   e.MakeRef(&L2FESpace, *sptr, Vsize_h1*2);

   ParGridFunction dx, dv, de;
   dx.MakeRef(&H1FESpace, dS_dt, 0);
   dv.MakeRef(&H1FESpace, dS_dt, Vsize_h1);
   de.MakeRef(&L2FESpace, dS_dt, Vsize_h1*2);

   // Set dx_dt = v (explicit).
   dx = v;

   if (!p_assembly)
   {
      Force = 0.0;
      Force.Assemble();
   }

   // Solve for velocity.
   Vector one(Vsize_l2), rhs(Vsize_h1), B, X; one = 1.0;
   if (p_assembly)
   {
      ForcePA.Mult(one, rhs); rhs.Neg();

      // Partial assembly solve for each velocity component.
      MassPAOperator VMassPA(&quad_data, H1compFESpace);
      const int size = H1compFESpace.GetVSize();
      for (int c = 0; c < dim; c++)
      {
         Vector rhs_c(rhs.GetData() + c*size, size),
                dv_c(dv.GetData() + c*size, size);

         Array<int> c_tdofs;
         Array<int> ess_bdr(H1FESpace.GetParMesh()->bdr_attributes.Max());
         // Attributes 1/2/3 correspond to fixed-x/y/z boundaries, i.e.,
         // we must enforce v_x/y/z = 0 for the velocity components.
         ess_bdr = 0; ess_bdr[c] = 1;
         // True dofs as if there's only one component.
         H1compFESpace.GetEssentialTrueDofs(ess_bdr, c_tdofs);

         dv_c = 0.0;
         Vector B(H1compFESpace.TrueVSize()), X(H1compFESpace.TrueVSize());
         H1compFESpace.Dof_TrueDof_Matrix()->MultTranspose(rhs_c, B);
         H1compFESpace.GetRestrictionMatrix()->Mult(dv_c, X);

         VMassPA.EliminateRHS(c_tdofs, B);

         CGSolver cg(H1FESpace.GetParMesh()->GetComm());
         cg.SetOperator(VMassPA);
         cg.SetRelTol(1e-8);
         cg.SetAbsTol(0.0);
         cg.SetMaxIter(200);
         cg.SetPrintLevel(0);
         cg.Mult(B, X);
         H1compFESpace.Dof_TrueDof_Matrix()->Mult(X, dv_c);
      }
   }
   else
   {
      Force.Mult(one, rhs); rhs.Neg();
      HypreParMatrix A;
      dv = 0.0;
      Mv.FormLinearSystem(ess_tdofs, dv, rhs, A, X, B);
      CGSolver cg(H1FESpace.GetParMesh()->GetComm());
      cg.SetOperator(A);
      cg.SetRelTol(1e-8); cg.SetAbsTol(0.0);
      cg.SetMaxIter(200);
      cg.SetPrintLevel(0);
      cg.Mult(B, X);
      Mv.RecoverFEMSolution(X, rhs, dv);
   }

   // Solve for energy, assemble the energy source if such exists.
   LinearForm *e_source = NULL;
   if (source_type == 1) // 2D Taylor-Green.
   {
      e_source = new LinearForm(&L2FESpace);
      TaylorCoefficient coeff;
      DomainLFIntegrator *d = new DomainLFIntegrator(coeff, &integ_rule);
      e_source->AddDomainIntegrator(d);
      e_source->Assemble();
   }
   if (p_assembly)
   {
      Vector rhs(Vsize_l2);
      ForcePA.MultTranspose(v, rhs);

      if (e_source) { rhs += *e_source; }

      MassPAOperator EMassPA(&quad_data, L2FESpace);
      CGSolver cg(L2FESpace.GetParMesh()->GetComm());
      cg.SetOperator(EMassPA);
      cg.SetRelTol(1e-8);
      cg.SetAbsTol(0.0);
      cg.SetMaxIter(200);
      cg.SetPrintLevel(0);
      cg.Mult(rhs, de);
   }
   else
   {
      Array<int> l2dofs, h1dofs;
      DenseMatrix loc_Force(h1dofs_cnt * dim, l2dofs_cnt);
      Vector v_vals(h1dofs_cnt * dim), e_rhs(l2dofs_cnt), de_loc(l2dofs_cnt);
      for (int i = 0; i < zones_cnt; i++)
      {
         H1FESpace.GetElementVDofs(i, h1dofs);
         L2FESpace.GetElementDofs(i, l2dofs);
         Force.SpMat().GetSubMatrix(h1dofs, l2dofs, loc_Force);
         v.GetSubVector(h1dofs, v_vals);

         loc_Force.MultTranspose(v_vals, e_rhs);
         if (e_source)
         {
            e_source->GetSubVector(l2dofs, de_loc); // Use de_loc as temporary.
            e_rhs += de_loc;
         }
         Me_inv(i).Mult(e_rhs, de_loc);
         de.SetSubVector(l2dofs, de_loc);
      }
   }

   delete e_source;
   quad_data_is_current = false;
}

double LagrangianHydroOperator::GetTimeStepEstimate(const Vector &S) const
{
   Vector* sptr = (Vector*) &S;
   ParGridFunction x;
   x.MakeRef(&H1FESpace, *sptr, 0);
   H1FESpace.GetParMesh()->NewNodes(x, false);
   UpdateQuadratureData(S);

   double glob_dt_est;
   MPI_Allreduce(&quad_data.dt_est, &glob_dt_est, 1, MPI_DOUBLE, MPI_MIN,
                 H1FESpace.GetParMesh()->GetComm());
   return glob_dt_est;
}

void LagrangianHydroOperator::ResetTimeStepEstimate() const
{
   quad_data.dt_est = numeric_limits<double>::infinity();
}

void LagrangianHydroOperator::ComputeDensity(ParGridFunction &rho)
{
   rho.SetSpace(&L2FESpace);

   DenseMatrix Mrho(l2dofs_cnt);
   Vector rhs(l2dofs_cnt), rho_z(l2dofs_cnt);
   Array<int> dofs(l2dofs_cnt);
   DenseMatrixInverse inv(&Mrho);
   MassIntegrator mi(&integ_rule);
   DensityIntegrator di(quad_data);
   di.SetIntRule(&integ_rule);
   for (int i = 0; i < zones_cnt; i++)
   {
      di.AssembleRHSElementVect(*L2FESpace.GetFE(i),
                                *L2FESpace.GetElementTransformation(i), rhs);
      mi.AssembleElementMatrix(*L2FESpace.GetFE(i),
                               *L2FESpace.GetElementTransformation(i), Mrho);
      inv.Factor();
      inv.Mult(rhs, rho_z);
      L2FESpace.GetElementDofs(i, dofs);
      rho.SetSubVector(dofs, rho_z);
   }
}

LagrangianHydroOperator::~LagrangianHydroOperator()
{
   delete tensors1D;
}

void LagrangianHydroOperator::UpdateQuadratureData(const Vector &S) const
{
   if (quad_data_is_current) { return; }

   const int dim = H1FESpace.GetParMesh()->Dimension();
   const int nqp = integ_rule.GetNPoints();

   ParGridFunction e, v;
   Vector* sptr = (Vector*) &S;
   v.MakeRef(&H1FESpace, *sptr, H1FESpace.GetVSize());
   e.MakeRef(&L2FESpace, *sptr, 2*H1FESpace.GetVSize());
   Vector e_vals;
   DenseMatrix Jpi(dim), sgrad_v(dim), Jinv(dim), stress(dim), stressJiT(dim);
   DenseMatrix v_vals;

   for (int i = 0; i < zones_cnt; i++)
   {
      ElementTransformation *T = H1FESpace.GetElementTransformation(i);
      e.GetValues(i, integ_rule, e_vals);
      v.GetVectorValues(*T, integ_rule, v_vals);
      for (int q = 0; q < nqp; q++)
      {
         const IntegrationPoint &ip = integ_rule.IntPoint(q);
         T->SetIntPoint(&ip);
         const DenseMatrix &Jpr = T->Jacobian();

         const double detJ = T->Weight();
         MFEM_VERIFY(detJ > 0.0, "Bad Jacobian determinant: " << detJ);

         stress = 0.0;
         const double rho = quad_data.rho0DetJ0w(i*nqp + q) / detJ / ip.weight;
         const double e   = max(0.0, e_vals(q));
         for (int d = 0; d < dim; d++)
         {
            stress(d, d) = - MaterialPressure(rho, e);
         }

         // Length scale at the point. The first eigenvector of the symmetric
         // velocity gradient gives the direction of maximal compression. This
         // is used to define the relative change of the initial length scale.
         v.GetVectorGradient(*T, sgrad_v);
         sgrad_v.Symmetrize();
         double eig_val_data[3], eig_vec_data[9];
         sgrad_v.CalcEigenvalues(eig_val_data, eig_vec_data);
         Vector compr_dir(eig_vec_data, dim);
         // Computes the initial->physical transformation Jacobian.
         mfem::Mult(Jpr, quad_data.Jac0inv(i*nqp + q), Jpi);
         Vector ph_dir(dim); Jpi.Mult(compr_dir, ph_dir);
         // Change of the initial mesh size in the compression direction.
         const double h = quad_data.h0 * ph_dir.Norml2() / (compr_dir.Norml2());

         // Time step estimate at the point.
         const double sound_speed = sqrt(gamma * (gamma-1.0) * e);
         quad_data.dt_est = min(quad_data.dt_est, cfl * h / sound_speed);

         if (use_viscosity)
         {
            // Measure of maximal compression.
            const double mu = eig_val_data[0];
            double visc_coeff = 2.0 * rho * h * h * fabs(mu);
            if (mu < 0.0) { visc_coeff += 0.5 * rho * h * sound_speed; }
            stress.Add(visc_coeff, sgrad_v);
         }

         // Quadrature data for partial assembly of the force operator.
         CalcInverse(Jpr, Jinv);
         MultABt(stress, Jinv, stressJiT);
         stressJiT *= integ_rule.IntPoint(q).weight * detJ;
         for (int vd = 0 ; vd < dim; vd++)
         {
            for (int gd = 0; gd < dim; gd++)
            {
               quad_data.stressJinvT(vd)(i*nqp + q, gd) = stressJiT(vd, gd);
            }
         }
      }
   }

   quad_data_is_current = true;
}

} // namespace hydrodynamics

} // namespace mfem

#endif // MFEM_USE_MPI
