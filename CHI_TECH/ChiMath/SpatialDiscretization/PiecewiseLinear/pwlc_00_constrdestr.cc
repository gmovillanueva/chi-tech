#include "pwlc.h"

//###################################################################
/**Constructor.*/
SpatialDiscretization_PWLC::
SpatialDiscretization_PWLC(chi_mesh::MeshContinuumPtr in_grid) :
  SpatialDiscretization(0, in_grid,
                        SDMType::PIECEWISE_LINEAR_CONTINUOUS),
  line_quad_order_second(chi_math::QuadratureOrder::SECOND),
  tri_quad_order_second(chi_math::QuadratureOrder::SECOND),
  tet_quad_order_second(chi_math::QuadratureOrder::SECOND)
{
  mapping_initialized = false;
  PreComputeCellSDValues(in_grid);
  OrderNodes(in_grid);
}
