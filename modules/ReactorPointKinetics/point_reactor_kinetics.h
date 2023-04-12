#ifndef CHITECH_POINT_REACTOR_KINETICS_H
#define CHITECH_POINT_REACTOR_KINETICS_H

#include "ChiPhysics/SolverBase/chi_solver.h"
#include "ChiMath/chi_math.h"
#include "ChiMath/dynamic_matrix.h"
#include "ChiMath/dynamic_vector.h"

/** \defgroup prk__TransientSolver prk::TransientSolver
 * \ingroup LuaModules
 * This is really cool
 * */

namespace prk
{
/**General transient solver for point kinetics.
*
\copydoc prk__TransientSolver.txt
* */
class TransientSolver : public chi_physics::Solver
{
private:
  std::vector<double> lambdas_;
  std::vector<double> betas_;
  double gen_time_;
  double rho_;
  double source_strength_;
  size_t num_precursors_;
  double dt_;

  chi_math::DynamicMatrix<double> A_, I_;
  chi_math::DynamicVector<double> x_t_, x_tp1_, q_;
  double beta_ = 1.0;
  double time_ = 0.0;
  double period_tph_ = 0.0;

public:
  static chi_objects::InputParameters GetInputParameters();
  explicit TransientSolver(const chi_objects::InputParameters& params);

  void Initialize() override;
  void Execute() override;
  void Step() override;
  void Advance() override;
};
} // namespace prk

#endif // CHITECH_POINT_REACTOR_KINETICS_H
