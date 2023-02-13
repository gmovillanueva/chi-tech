#include "LBSTransient/lbts_transient_solver.h"

#include "LinearBoltzmannSolvers/A_LBSSolver/Groupset/lbs_groupset.h"

#include "chi_log.h"
#include "ChiTimer/chi_timer.h"

//###################################################################
/**Sets the source moments for the groups_ in the current group set.
 *
 * \param groupset The groupset the under consideration.
 * \param destination_q A vector to contribute the source to.
 * \param source_flags Flags for adding specific terms into the
 *        destination vector. Available flags are for applying
 *        the material source, across/within-group scattering,
 *        and across/within-groups_ fission.
 *
 * */
void lbs::TransientSolver::
  SetTransientSource(LBSGroupset& groupset,
                     std::vector<double>& destination_q,
                     const std::vector<double>& phi,
                     SourceFlags source_flags)
{
  chi::log.LogEvent(source_event_tag_, chi_objects::ChiLog::EventType::EVENT_BEGIN);

  const auto& BackwardEuler = chi_math::SteppingMethod::BACKWARD_EULER;
  const auto& CrankNicolson = chi_math::SteppingMethod::CRANK_NICHOLSON;

  double theta;
  if      (method == BackwardEuler) theta = 1.0;
  else if (method == CrankNicolson) theta = 0.5;
  else                              theta = 0.7;

  const double eff_dt = theta * dt;

  const bool apply_fixed_src       = (source_flags & APPLY_FIXED_SOURCES);
  const bool apply_wgs_scatter_src = (source_flags & APPLY_WGS_SCATTER_SOURCES);
  const bool apply_ags_scatter_src = (source_flags & APPLY_AGS_SCATTER_SOURCES);
  const bool apply_wgs_fission_src = (source_flags & APPLY_WGS_FISSION_SOURCES);
  const bool apply_ags_fission_src = (source_flags & APPLY_AGS_FISSION_SOURCES);

  //================================================== Get group setup
  auto gs_i = static_cast<size_t>(groupset.groups[0].id);
  auto gs_f = static_cast<size_t>(groupset.groups.back().id);

  auto first_grp = static_cast<size_t>(groups_.front().id);
  auto last_grp = static_cast<size_t>(groups_.back().id);

  const auto& m_to_ell_em_map =
    groupset.quadrature->GetMomentToHarmonicsIndexMap();

  std::vector<double> default_zero_src(groups_.size(), 0.0);

  //================================================== Loop over local cells
  // Apply all nodal sources
  for (const auto& cell : grid_ptr_->local_cells)
  {
    const auto& fe_values = unit_cell_matrices_[cell.local_id];
    auto& transport_view = cell_transport_views_[cell.local_id];
    const double cell_volume = transport_view.Volume();

    //==================== Obtain xs
    auto xs = transport_view.XS();
    auto P0_src = matid_to_src_map_[cell.material_id];

    const auto& S = xs.transfer_matrices;

    //==================== Obtain src
    double* src = default_zero_src.data();
    if (P0_src and apply_fixed_src)
      src = P0_src->source_value_g.data();

    //======================================== Loop over nodes
    const int num_nodes = transport_view.NumNodes();
    for (int i = 0; i < num_nodes; ++i)
    {
      //======================================== Loop over moments
      for (int m = 0; m < static_cast<int>(num_moments_); ++m)
      {
        unsigned int ell = m_to_ell_em_map[m].ell;

        size_t uk_map = transport_view.MapDOF(i, m, 0); //unknown map

        //=================================== Loop over groupset groups_
        for (size_t g = gs_i; g <= gs_f; ++g)
        {
          double rhs = 0.0;

          //============================== Apply fixed sources
          if (not options_.use_src_moments) //using regular material src
            rhs += (apply_fixed_src and ell == 0)? src[g] : 0.0;
          else if (apply_fixed_src)  //using ext_src_moments
           rhs += ext_src_moments_local_[uk_map + g];

          //============================== Apply scattering sources
          const bool moment_avail = (ell < S.size());

          //==================== Across groupset
          if (moment_avail and apply_ags_scatter_src)
            for (const auto& [_, gp, sigma_sm] : S[ell].Row(g))
              if (gp < gs_i or gp > gs_f)
                rhs += sigma_sm * phi[uk_map + gp];

          //==================== Within groupset
          if (moment_avail and apply_wgs_scatter_src)
            for (const auto& [_, gp, sigma_sm] : S[ell].Row(g))
              if (gp >= gs_i and gp <= gs_f)
                rhs += sigma_sm * phi[uk_map + gp];

          //============================== Apply fission sources
          const bool fission_avail = xs.is_fissionable and ell == 0;

          //==================== Across groupset
          if (fission_avail and apply_ags_fission_src)
          {
            const auto& prod = xs.production_matrix[g];
            for (size_t gp = first_grp; gp <= last_grp; ++gp)
              if (gp < gs_i or gp > gs_f)
              {
                rhs += prod[gp] * phi[uk_map + gp];

                if (options_.use_precursors)
                  for (const auto& precursor : xs.precursors)
                  {
                    const double coeff =
                        precursor.emission_spectrum[g] *
                        precursor.decay_constant /
                        (1.0 + eff_dt * precursor.decay_constant);

                    rhs += coeff * eff_dt *
                           precursor.fractional_yield *
                           xs.nu_delayed_sigma_f[gp] *
                           phi[uk_map + gp] /
                           cell_volume;
                  }
              }
          }

          //==================== Within groupset
          if (fission_avail and apply_wgs_fission_src)
          {
            const auto& prod = xs.production_matrix[g];
            for (size_t gp = gs_i; gp <= gs_f; ++gp)
            {
              rhs += prod[gp] * phi[uk_map + gp];

              if (options_.use_precursors)
                for (const auto& precursor : xs.precursors)
                {
                  const double coeff =
                      precursor.emission_spectrum[g] *
                      precursor.decay_constant /
                      (1.0 + eff_dt * precursor.decay_constant);

                  rhs += coeff * eff_dt *
                         precursor.fractional_yield *
                         xs.nu_delayed_sigma_f[gp] *
                         phi[uk_map + gp] /
                         cell_volume;
                }
            }
          }

          //============================== Apply previous precursors
          if (fission_avail and apply_fixed_src and options_.use_precursors)
          {
            const auto& J = max_precursors_per_material_;
            const size_t dof_map = cell.local_id * J;
            for (unsigned int j = 0; j < xs.num_precursors; ++j)
            {
              const auto& precursor = xs.precursors[j];

              const double coeff =
                  precursor.emission_spectrum[g] *
                  precursor.decay_constant /
                  (1.0 + eff_dt * precursor.decay_constant);

              rhs += coeff * precursor_prev_local[dof_map + j];
            }
          }

          //============================== Add to destination vector
          destination_q[uk_map + g] += rhs;

        }//for g
      }//for m
    }//for dof i
  }//for cell

  //================================================== Apply point sources
  if (not options_.use_src_moments and apply_fixed_src)
    for (const auto& point_source : point_sources_)
    {
      const auto& info_list = point_source.ContainingCellsInfo();
      for (const auto& info : info_list)
      {
        auto& transport_view = cell_transport_views_[info.cell_local_id];

        const auto& strength = point_source.Strength();
        const auto& node_weights = info.node_weights;
        const double vol_w = info.volume_weight;

        const int num_nodes = transport_view.NumNodes();
        for (int i = 0; i < num_nodes; ++i)
        {
          const size_t uk_map = transport_view.MapDOF(i, /*moment=*/0, /*grp=*/0);
          for (size_t g = gs_i; g <= gs_f; ++g)
            destination_q[uk_map + g] += strength[g] * node_weights[i] * vol_w;
        }//for node i
      }//for cell
    }//for point source

  chi::log.LogEvent(source_event_tag_, chi_objects::ChiLog::EventType::EVENT_END);
}