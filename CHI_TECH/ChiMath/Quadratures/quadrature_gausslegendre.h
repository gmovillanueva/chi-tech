#ifndef _quadrature_gausslegendre_h
#define _quadrature_gausslegendre_h

#include "quadrature.h"
#include <stdio.h>

namespace chi_math
{
  class QuadratureGaussLegendre;
}

//######################################################### Class Def
/**Gauss-Legendre quadrature.*/
class chi_math::QuadratureGaussLegendre : public chi_math::Quadrature
{
public:
  //01
  void Initialize(int N, int maxiters=1000,
                  double tol=1.0e-12, bool verbose=false);
  std::vector<double> FindRoots(int N,
                                int max_iters=1000,
                                double tol=1.0e-12);
};

#endif