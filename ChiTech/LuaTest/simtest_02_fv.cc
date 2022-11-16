#include "chi_lua.h"

#include "chi_runtime.h"
#include "chi_log.h"

#include "ChiMesh/MeshHandler/chi_meshhandler.h"
#include "ChiMesh/MeshContinuum/chi_meshcontinuum.h"

#include "ChiMath/SpatialDiscretization/FiniteVolume/fv.h"
#include "ChiMath/PETScUtils/petsc_utils.h"

#include "ChiPhysics/FieldFunction2/fieldfunction2.h"

#include "ChiMath/VectorGhostCommunicator/vector_ghost_communicator.h"

namespace chi_unit_sim_tests
{

/**This is a simple test of the Finite Volume spatial discretization applied
 * to Laplace's problem. */
int chiSimTest02_FV(lua_State* L)
{
  const int num_args = lua_gettop(L);
  chi::log.Log() << "chiSimTest02_FV num_args = " << num_args;

  //============================================= Get grid
  auto grid_ptr = chi_mesh::GetCurrentHandler().GetGrid();
  const auto& grid = *grid_ptr;

  chi::log.Log() << "Global num cells: " << grid.GetGlobalNumberOfCells();

  //============================================= Make SDM
  typedef std::shared_ptr<chi_math::SpatialDiscretization> SDMPtr;
  SDMPtr sdm_ptr = chi_math::SpatialDiscretization_FV::New(grid_ptr);
  const auto& sdm = *sdm_ptr;

  const auto& OneDofPerNode = sdm.UNITARY_UNKNOWN_MANAGER;

  const size_t num_local_dofs = sdm.GetNumLocalDOFs(OneDofPerNode);
  const size_t num_globl_dofs = sdm.GetNumGlobalDOFs(OneDofPerNode);

  chi::log.Log() << "Num local DOFs: " << num_local_dofs;
  chi::log.Log() << "Num globl DOFs: " << num_globl_dofs;

  //============================================= Initializes Mats and Vecs
  const auto n = static_cast<int64_t>(num_local_dofs);
  const auto N = static_cast<int64_t>(num_globl_dofs);
  Mat A;
  Vec x,b;

  A = chi_math::PETScUtils::CreateSquareMatrix(n,N);
  x = chi_math::PETScUtils::CreateVector(n,N);
  b = chi_math::PETScUtils::CreateVector(n,N);

  std::vector<int64_t> nodal_nnz_in_diag;
  std::vector<int64_t> nodal_nnz_off_diag;
  sdm.BuildSparsityPattern(nodal_nnz_in_diag,nodal_nnz_off_diag,OneDofPerNode);

  chi_math::PETScUtils::InitMatrixSparsity(A,
                                           nodal_nnz_in_diag,
                                           nodal_nnz_off_diag);

  //============================================= Assemble the system
  chi::log.Log() << "Assembling system: ";
  for (const auto& cell : grid.local_cells)
  {
    const auto& cell_mapping = sdm.GetCellMapping(cell);
    const auto& xp = cell.centroid;
    const int64_t imap = sdm.MapDOF(cell,0);
    const int64_t jpmap = imap;

    const double V = cell_mapping.CellVolume();

    size_t f=0;
    for (const auto& face : cell.faces)
    {
      const auto Af = face.normal * cell_mapping.FaceArea(f);

      if (face.has_neighbor)
      {
        const auto& adj_cell = grid.cells[face.neighbor_id];
        const int64_t jnmap = sdm.MapDOF(adj_cell,0);

        const auto& xn = adj_cell.centroid;

        const auto xpn = xn - xp;

        const auto c = Af.Dot(xpn)/xpn.NormSquare();

        MatSetValue(A, imap, jpmap,  c, ADD_VALUES);
        MatSetValue(A, imap, jnmap, -c, ADD_VALUES);
      }
      else
      {
        const auto& xn = xp + 2.0*(face.centroid - xp);
        const auto xpn = xn - xp;

        const auto c = Af.Dot(xpn)/xpn.NormSquare();

        MatSetValue(A, imap, jpmap,  c, ADD_VALUES);
      }
      ++f;
    }//for face

    VecSetValue(b, imap, 1.0*V, ADD_VALUES);
  }//for cell i

  chi::log.Log() << "Global assembly";

  MatAssemblyBegin(A, MAT_FINAL_ASSEMBLY);
  MatAssemblyEnd(A, MAT_FINAL_ASSEMBLY);
  VecAssemblyBegin(b);
  VecAssemblyEnd(b);

  chi::log.Log() << "Done global assembly";

  //============================================= Create Krylov Solver
  chi::log.Log() << "Solving: ";
  auto petsc_solver =
  chi_math::PETScUtils::CreateCommonKrylovSolverSetup(
    A,               //Matrix
    "FVDiffSolver",  //Solver name
    KSPCG,           //Solver type
    PCGAMG,          //Preconditioner type
    1.0e-6,          //Relative residual tolerance
    1000);            //Max iterations

  //============================================= Solve
  KSPSolve(petsc_solver.ksp,b,x);

  chi::log.Log() << "Done solving";

  //============================================= Create Field Function
  auto ff = std::make_shared<chi_physics::FieldFunction2>(
    "Phi",
    sdm_ptr,
    chi_math::Unknown(chi_math::UnknownType::SCALAR)
  );

  //============================================= Update field function
  std::vector<double> field;
  sdm.LocalizePETScVector(x,field,sdm.UNITARY_UNKNOWN_MANAGER);

  ff->UpdateFieldVector(field);
  ff->ExportToVTK("SimTest_02_FV");

  //============================================= Make ghosted vectors
  std::vector<int64_t> ghost_ids =
    sdm.GetGhostDOFIndices(sdm.UNITARY_UNKNOWN_MANAGER);

  chi_math::VectorGhostCommunicator vgc(num_local_dofs,
                                        num_globl_dofs,
                                        ghost_ids,
                                        MPI_COMM_WORLD);
  std::vector<double> field_wg = vgc.MakeGhostedVector(field);

  vgc.CommunicateGhostEntries(field_wg);

  //============================================= Compute the gradient
  chi_math::UnknownManager grad_uk_man({
    chi_math::Unknown{chi_math::UnknownType::VECTOR_3,3}});

  const size_t num_grad_dofs = sdm.GetNumLocalDOFs(grad_uk_man);

  std::vector<double> grad_phi(num_grad_dofs, 0.0);
  for (const auto& cell : grid.local_cells)
  {
    const auto& cell_mapping = sdm.GetCellMapping(cell);
    const int64_t pmap = sdm.MapDOFLocal(cell, 0);
    const double  phi_P = field_wg[pmap];

    chi_mesh::Vector3 grad_phi_P(0,0,0);

    for (const auto& face : cell.faces)
    {
      double phi_N = 0.0;
      if (face.has_neighbor)
      {
        const auto& adj_cell = grid.cells[face.neighbor_id];
        const int64_t nmap = sdm.MapDOFLocal(adj_cell, 0);
        phi_N = field_wg[nmap];
      }
      grad_phi_P += 0.5*(phi_N + phi_P)*face.normal;
    }//for face
    grad_phi_P /= cell_mapping.CellVolume();

    const int64_t xmap = sdm.MapDOFLocal(cell, 0, grad_uk_man, 0, 0);
    const int64_t ymap = sdm.MapDOFLocal(cell, 0, grad_uk_man, 0, 1);
    const int64_t zmap = sdm.MapDOFLocal(cell, 0, grad_uk_man, 0, 2);

    grad_phi[xmap] = grad_phi_P.x;
    grad_phi[ymap] = grad_phi_P.y;
    grad_phi[zmap] = grad_phi_P.z;
  }//for cell

  //============================================= Create Field Function
  auto grad_ff = std::make_shared<chi_physics::FieldFunction2>(
    "GradPhi",
    sdm_ptr,
    chi_math::Unknown(chi_math::UnknownType::VECTOR_3,3)
  );

  grad_ff->UpdateFieldVector(grad_phi);

  grad_ff->ExportToVTK("SimTest_02_FV_grad");

  //============================================= Clean up
  KSPDestroy(&petsc_solver.ksp);

  VecDestroy(&x);
  VecDestroy(&b);
  MatDestroy(&A);

  chi::log.Log() << "Done cleanup";

  return 0;
}

}//namespace chi_unit_tests
