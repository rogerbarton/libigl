// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "arap.h"
#include "colon.h"
#include "cotmatrix.h"
#include "massmatrix.h"
#include "group_sum_matrix.h"
#include "covariance_scatter_matrix.h"
#include "speye.h"
#include "mode.h"
#include "project_isometrically_to_plane.h"
#include "slice.h"
#include "arap_rhs.h"
#include "repdiag.h"
#include "columnize.h"
#include "fit_rotations.h"
#include <cassert>
#include <iostream>

template <
  typename DerivedV,
  typename DerivedF,
  typename Derivedb>
IGL_INLINE bool igl::arap_precomputation(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedF> & F,
  const int dim,
  const Eigen::PlainObjectBase<Derivedb> & b,
  ARAPData<typename DerivedV::Scalar> & data)
{
  using namespace std;
  using namespace Eigen;
  typedef DerivedV::Scalar Scalar;
  typedef Matrix<Scalar, Dynamic, Dynamic> MatrixS;

  // number of vertices
  const int n = V.rows();
  data.n = n;
  assert((b.size() == 0 || b.maxCoeff() < n) && "b out of bounds");
  assert((b.size() == 0 || b.minCoeff() >=0) && "b out of bounds");
  // remember b
  data.b = b;
  //assert(F.cols() == 3 && "For now only triangles");
  // dimension
  //const int dim = V.cols();
  assert((dim == 3 || dim ==2) && "dim should be 2 or 3");
  data.dim = dim;
  //assert(dim == 3 && "Only 3d supported");
  // Defaults
  data.f_ext = MatrixS::Zero(n,data.dim);

  assert(data.dim <= V.cols() && "solve dim should be <= embedding");
  bool flat = (V.cols() - data.dim)==1;

  DerivedV plane_V;
  DerivedF plane_F;
  typedef SparseMatrix<DerivedV::Scalar> SparseMatrixS;
  SparseMatrixS ref_map,ref_map_dim;
  if(flat)
  {
    project_isometrically_to_plane(V,F,plane_V,plane_F,ref_map);
    repdiag(ref_map,dim,ref_map_dim);
  }
  const PlainObjectBase<DerivedV>& ref_V = (flat?plane_V:V);
  const PlainObjectBase<DerivedF>& ref_F = (flat?plane_F:F);
  SparseMatrixS L;
  cotmatrix(V,F,L);

  ARAPEnergyType eff_energy = data.energy;
  if(eff_energy == ARAP_ENERGY_TYPE_DEFAULT)
  {
    switch(F.cols())
    {
      case 3:
        if(data.dim == 3)
        {
          eff_energy = ARAP_ENERGY_TYPE_SPOKES_AND_RIMS;
        }else
        {
          eff_energy = ARAP_ENERGY_TYPE_ELEMENTS;
        }
        break;
      case 4:
        eff_energy = ARAP_ENERGY_TYPE_ELEMENTS;
        break;
      default:
        assert(false);
    }
  }


  // Get covariance scatter matrix, when applied collects the covariance
  // matrices used to fit rotations to during optimization
  covariance_scatter_matrix<Scalar>(ref_V,ref_F,eff_energy,data.CSM);
  if(flat)
  {
    data.CSM = (data.CSM * ref_map_dim.transpose()).eval();
  }
  assert(data.CSM.cols() == V.rows()*data.dim);

  // Get group sum scatter matrix, when applied sums all entries of the same
  // group according to G
  SparseMatrixS G_sum;
  if(data.G.size() == 0)
  {
    if(eff_energy == ARAP_ENERGY_TYPE_ELEMENTS)
    {
      speye(F.rows(),G_sum);
    }else
    {
      speye(n,G_sum);
    }
  }else
  {
    // groups are defined per vertex, convert to per face using mode
    if(eff_energy == ARAP_ENERGY_TYPE_ELEMENTS)
    {
      Eigen::Matrix<int,Eigen::Dynamic,1> GG;
      MatrixXi GF(F.rows(),F.cols());
      for(int j = 0;j<F.cols();j++)
      {
        Matrix<int,Eigen::Dynamic,1> GFj;
        slice(data.G,F.col(j),GFj);
        GF.col(j) = GFj;
      }
      mode<int>(GF,2,GG);
      data.G=GG;
    }
    //printf("group_sum_matrix()\n");
    group_sum_matrix(data.G,G_sum);
  }
  SparseMatrixS G_sum_dim;
  repdiag(G_sum,data.dim,G_sum_dim);
  assert(G_sum_dim.cols() == data.CSM.rows());
  data.CSM = (G_sum_dim * data.CSM).eval();


  arap_rhs(ref_V,ref_F,data.dim,eff_energy,data.K);
  if(flat)
  {
    data.K = (ref_map_dim * data.K).eval();
  }
  assert(data.K.rows() == data.n*data.dim);

  SparseMatrixS Q = (-L).eval();

  if(data.with_dynamics)
  {
    const Scalar h = data.h;
    assert(h != 0);
    SparseMatrixS M;
    massmatrix(V,F,MASSMATRIX_TYPE_DEFAULT,data.M);
    const Scalar dw = (1./data.ym)*(h*h);
    SparseMatrixS DQ = dw * 1./(h*h)*data.M;
    Q += DQ;
    // Dummy external forces
    data.f_ext = MatrixS::Zero(n,data.dim);
    data.vel = MatrixS::Zero(n,data.dim);
  }

  return min_quad_with_fixed_precompute(
    Q,b,SparseMatrixS(),true,data.solver_data);
}

template <
  typename Derivedbc,
  typename DerivedU>
IGL_INLINE bool igl::arap_solve(
  const Eigen::PlainObjectBase<Derivedbc> & bc,
  ARAPData<typename DerivedU::Scalar> & data,
  Eigen::PlainObjectBase<DerivedU> & U)
{
  using namespace Eigen;
  using namespace std;
  typedef DerivedU::Scalar Scalar;
  typedef Matrix<Scalar, Dynamic, Dynamic> MatrixS;
  typedef Matrix<Scalar, Dynamic, 1> VectorS;

	assert(data.b.size() == bc.rows());
  if(bc.size() > 0)
  {
    assert(bc.cols() == data.dim && "bc.cols() match data.dim");
  }
  const int n = data.n;
  int iter = 0;
  if(U.size() == 0)
  {
    // terrible initial guess.. should at least copy input mesh
#ifndef NDEBUG
    cerr<<"arap_solve: Using terrible initial guess for U. Try U = V."<<endl;
#endif
    U = MatrixS::Zero(data.n,data.dim);
  }else
  {
    assert(U.cols() == data.dim && "U.cols() match data.dim");
  }
  // changes each arap iteration
  MatrixS U_prev = U;
  // doesn't change for fixed with_dynamics timestep
  MatrixS U0;
  if(data.with_dynamics)
  {
    U0 = U_prev;
  }
  while(iter < data.max_iter)
  {
    U_prev = U;
    // enforce boundary conditions exactly
    for(int bi = 0;bi<bc.rows();bi++)
    {
      U.row(data.b(bi)) = bc.row(bi);
    }

    const auto & Udim = U.replicate(data.dim,1);
    assert(U.cols() == data.dim);
    // As if U.col(2) was 0
    MatrixS S = data.CSM * Udim;
    // THIS NORMALIZATION IS IMPORTANT TO GET SINGLE PRECISION SVD CODE TO WORK
    // CORRECTLY.
    S /= S.array().abs().maxCoeff();

    const int Rdim = data.dim;
    MatrixS R(Rdim,data.CSM.rows());
    if(R.rows() == 2)
    {
      fit_rotations_planar(S,R);
    }else
    {
      fit_rotations(S,true,R);
//#ifdef __SSE__ // fit_rotations_SSE will convert to float if necessary
//      fit_rotations_SSE(S,R);
//#else
//      fit_rotations(S,true,R);
//#endif
    }
    //for(int k = 0;k<(data.CSM.rows()/dim);k++)
    //{
    //  R.block(0,dim*k,dim,dim) = MatrixS::Identity(dim,dim);
    //}


    // Number of rotations: #vertices or #elements
    int num_rots = data.K.cols()/Rdim/Rdim;
    // distribute group rotations to vertices in each group
    MatrixS eff_R;
    if(data.G.size() == 0)
    {
      // copy...
      eff_R = R;
    }else
    {
      eff_R.resize(Rdim,num_rots*Rdim);
      for(int r = 0;r<num_rots;r++)
      {
        eff_R.block(0,Rdim*r,Rdim,Rdim) =
          R.block(0,Rdim*data.G(r),Rdim,Rdim);
      }
    }

    MatrixS Dl;
    if(data.with_dynamics)
    {
      assert(data.M.rows() == n &&
        "No mass matrix. Call arap_precomputation if changing with_dynamics");
      const Scalar h = data.h;
      assert(h != 0);
      //Dl = 1./(h*h*h)*M*(-2.*V0 + Vm1) - fext;
      // data.vel = (V0-Vm1)/h
      // h*data.vel = (V0-Vm1)
      // -h*data.vel = -V0+Vm1)
      // -V0-h*data.vel = -2V0+Vm1
      const Scalar dw = (1./data.ym)*(h*h);
      Dl = dw * (1./(h*h)*data.M*(-U0 - h*data.vel) - data.f_ext);
    }

    VectorS Rcol;
    columnize(eff_R,num_rots,2,Rcol);
    VectorS Bcol = -data.K * Rcol;
    assert(Bcol.size() == data.n*data.dim);
    for(int c = 0;c<data.dim;c++)
    {
      VectorS Uc,Bc,bcc,Beq;
      Bc = Bcol.block(c*n,0,n,1);
      if(data.with_dynamics)
      {
        Bc += Dl.col(c);
      }
      if(bc.size()>0)
      {
        bcc = bc.col(c);
      }
      min_quad_with_fixed_solve(
        data.solver_data,
        Bc,bcc,Beq,
        Uc);
      U.col(c) = Uc;
    }

    iter++;
  }
  if(data.with_dynamics)
  {
    // Keep track of velocity for next time
    data.vel = (U-U0)/data.h;
  }

  return true;
}

#ifdef IGL_STATIC_LIBRARY
template bool igl::arap_solve<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, igl::ARAPData&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template bool igl::arap_precomputation<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, igl::ARAPData&);
#endif
