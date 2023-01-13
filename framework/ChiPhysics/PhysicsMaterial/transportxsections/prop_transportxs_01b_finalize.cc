#include "material_property_transportxsections.h"

#include "chi_runtime.h"
#include "chi_log.h"

#include <numeric>
#include <algorithm>
#include <string>


void chi_physics::TransportCrossSections::Finalize()
{

  //============================================================
  // Set the absorption cross section, if unset
  //============================================================

  // The logic here is that if absorption is empty, it was not
  // specified, therefore, it should be computed. If a uniformly
  // zero absorption cross section was provided, assume that
  // was intentional.

  if (sigma_a.empty())
    ComputeAbsorption();

  //============================================================
  // Define utility functions
  //============================================================


  auto is_valid = [](const std::vector<double>& vec)
  {
    return !vec.empty() &&
            std::all_of(vec.begin(), vec.end(),
                       [](double x) { return x >= 0.0; });
  };

  //============================================================
  // Determine if fissionable or not
  //============================================================

  is_fissionable = is_valid(sigma_f) || is_valid(nu_sigma_f);

  //============================================================
  // Zero fission data if not fissionable
  //============================================================

  // Use a little bit of overkill and clear all fission-related
  // properties if the fission cross section was not specified.

  if (!is_fissionable)
  {
    chi::log.Log0Verbose1()
        << "No fission cross sections specified... "
        << "Clearing all fission properties.";


    num_precursors = 0;
    sigma_f.clear();
    nu_sigma_f.clear();
    nu_prompt_sigma_f.clear();
    nu_delayed_sigma_f.clear();

    nu.clear();
    nu_prompt.clear();
    nu_delayed.clear();
    beta.clear();

    chi.clear();
    chi_prompt.clear();

    precursors.clear();
  }

  //============================================================
  // Check specified fission data
  //============================================================

  else
  {

    chi::log.Log0Verbose1()
        << "Fission cross sections found.\n"
        << "Checking fission data specification...";

    //==================================================
    // Check prompt/delayed specification
    //==================================================

    if (num_precursors > 0)
    {
      chi::log.Log0Verbose1()
          << "Prompt/delayed specification used.\n"
          << "Checking for prompt/delayed fission data...";

      //==================================================
      // Check fission yield data
      //==================================================

      if (is_valid(nu_prompt) && is_valid(nu_delayed))
      {
        if (!std::all_of(nu_prompt.begin(), nu_prompt.end(),
                         [](double x) { return x == 0.0 || x > 1.0; }) &&
            !std::all_of(nu_delayed.begin(), nu_delayed.end(),
                         [](double x) { return x >= 0.0; }))
          throw std::logic_error(
              "Invalid prompt and delayed fission neutron yields "
              "encountered.\nPrompt fission neutron yields must be either "
              "zero or greater than one.\nDelayed fission neutron yields "
              "must be zero or greater.");

        //compute other quantities
        nu.assign(num_groups, 0.0);
        beta.assign(num_groups, 0.0);
        for (unsigned int g = 0; g < num_groups; ++g)
        {
          nu[g] = nu_prompt[g] + nu_delayed[g];
          beta[g] = nu_delayed[g] / nu[g];
        }
      }//if prompt/delayed specified

      else if (is_valid(nu) && is_valid(beta))
      {
        if (!std::all_of(nu.begin(), nu.end(),
                         [](double x) { return x == 0.0 || x > 1.0; }))
          throw std::logic_error(
              "Invalid fission neutron yield data encountered.\n"
              "All values must be either zero or greater than one.");
        if (!std::all_of(beta.begin(), beta.end(),
                         [](double x) { return x >= 0.0 && x <= 1.0; }))
          throw std::logic_error(
              "Invalid delayed neutron fraction data encountered.\n"
              "All values must be in the range [0.0, 1.0].");

        //compute other quantities
        nu_prompt.assign(num_groups, 0.0);
        nu_delayed.assign(num_groups, 0.0);
        for (unsigned int g = 0; g < num_groups; ++g)
        {
          nu_prompt[g] = (1.0 - beta[g]) * nu[g];
          nu_delayed[g] = beta[g] * nu[g];
        }
      } //if delayed fraction specified

      else
        throw std::logic_error(
            "Invalid specification of prompt/delayed fission "
            "neutron yield data encountered.\nEither the prompt and "
            "delayed fission neutron yields or the total fission neutron"
            "yield and delayed neutron fraction must be provided.");

      //==================================================
      // Compute all production cross sections
      //==================================================

      //ensure the fission cross section is available
      if (!is_valid(sigma_f))
      {
        sigma_f.assign(num_groups, 0.0);
        for (unsigned int g = 0; g < num_groups; ++g)
          if (nu[g] != 0.0)
            sigma_f[g] = nu_sigma_f[g] / nu[g];
      }

      //compute other quantities
      nu_sigma_f.assign(num_groups, 0.0);
      nu_prompt_sigma_f.assign(num_groups, 0.0);
      nu_delayed_sigma_f.assign(num_groups, 0.0);
      for (unsigned int g = 0; g < num_groups; ++g)
      {
        nu_sigma_f[g] = nu[g] * sigma_f[g];
        nu_prompt_sigma_f[g] = nu_prompt[g] * sigma_f[g];
        nu_delayed_sigma_f[g] = nu_delayed[g] * sigma_f[g];
      }

      //==================================================
      // Check prompt fission spectrum
      //==================================================

      if (chi_prompt.empty())
        throw std::logic_error("Prompt fission spectrum not found.");

      if (std::all_of(chi_prompt.begin(), chi_prompt.end(),
                      [](double x) { return x == 0.0; }))
        throw std::logic_error(
            "Invalid prompt fission spectrum encountered.\n"
            "Spectra must have at least one nonzero value.");

      //normalize the spectrum to a unit sum
      double chi_prompt_sum = std::accumulate(
          chi_prompt.begin(), chi_prompt.end(), 0.0);

      for (unsigned int g = 0; g < num_groups; ++g)
        chi_prompt[g] /= chi_prompt_sum;

      //==================================================
      // Check precursor data
      //==================================================

      if (precursors.empty())
        throw std::logic_error("No precursors found.");

      // check decay constants
      if (!std::all_of(precursors.begin(), precursors.end(),
                       [](const Precursor& p)
                       { return p.decay_constant > 0.0; }))
        throw std::logic_error(
            "Invalid precursor decay constant encountered.\n"
            "Decay constants must be strictly positive.");

      // check fractional yields
      if (std::all_of(precursors.begin(), precursors.end(),
                      [](const Precursor& p)
                      { return p.fractional_yield == 0.0; }))
        throw std::logic_error(
            "Invalid precursor yield fractions encountered.\n"
            "There must be one or more nonzero yield fractions.");

      if (!std::all_of(precursors.begin(), precursors.end(),
                       [](const Precursor& p)
                       { return p.fractional_yield >= 0.0 &&
                                p.fractional_yield <= 1.0; }))
        throw std::logic_error(
            "Invalid delayed neutron precursor yield fraction "
            "encountered.\n"
            "Yield fractions must be in the range [0.0, 1.0]");

      //normalize the fractional yields
      double yield_sum = std::accumulate(
          precursors.begin(), precursors.end(), 0.0,
          [](double val, const Precursor& p)
          { return val + p.fractional_yield; });

      for (unsigned int j = 0; j < num_precursors; ++j)
        precursors[j].fractional_yield /= yield_sum;

      //check the emission spectra
      for (unsigned int j = 0; j < num_precursors; ++j)
      {
        auto& spectrum = precursors[j].emission_spectrum;

        if (std::all_of(spectrum.begin(), spectrum.end(),
                        [](double x) { return x == 0.0; }))
          throw std::logic_error(
              "Invalid delayed emission spectra encountered for "
              "precursor species " + std::to_string(j) + ".\n" +
              "Spectra must have at least one nonzero value.");

        //normalize the emission spectrum
        double spectrum_sum = std::accumulate(
            spectrum.begin(), spectrum.end(), 0.0);

        for (unsigned int g = 0; g < num_groups; ++g)
          spectrum[g] /= spectrum_sum;
      }

      //==================================================
      // Compute the steady-state fission spectrum
      //==================================================

      // NOTE: This is only exact when beta is energy-independent.
      //       When it is not, this estimation may be incorrect.
      //       The true definition of steady-state fission spectrum
      //       is the rate at which prompt and delayed fission
      //       yield neutrons within group `g` divided by the
      //       total fission rate. When beta is energy-independent,
      //       the fission rate can be eliminated . When it is not,
      //       it requires the computation of the fission rate, which
      //       requires a weight spectrum.

      chi.assign(num_groups, 0.0);
      for (unsigned int g = 0; g < num_groups; ++g)
      {
        //compute beta-averaged total fission spectrum
        chi[g] = (1.0 - beta[g]) * chi_prompt[g];
        for (unsigned int j = 0; j < num_precursors; ++j)
          chi[g] += beta[g] * precursors[j].fractional_yield *
                    precursors[j].emission_spectrum[g];
      }

      //normalize total chi just in case
      double chi_sum = std::accumulate(chi.begin(), chi.end(), 0.0);
      for (unsigned int g = 0; g < num_groups; ++g)
        chi[g] /= chi_sum;
    }//if prompt/delayed

    //==================================================
    // Check total fission specification
    //==================================================

    else
    {
      chi::log.Log0Verbose1()
          << "Total/steady-state specification used.\n"
          << "Checking total/steady-state fission data...";

      //check total nu
      {
        if (nu.empty())
          throw std::logic_error("Total neutrons per fission not found.");
        if (!std::all_of(nu.begin(), nu.end(),
                         [](double x) { return x == 0.0 || x > 1.0; }))
          throw std::logic_error(
              "Invalid total fission neutron yield encountered.\n"
              "All values must be either zero or greater than one.");

        //compute other quantities
        if (sigma_f.empty())
        {
          sigma_f.assign(num_groups, 0.0);
          for (unsigned int g = 0; g < num_groups; ++g)
            if (nu[g] != 0.0)
              sigma_f[g] = nu_sigma_f[g] / nu[g];
        }

        //compute the production cross section
        nu_sigma_f.assign(num_groups, 0.0);
        for (unsigned int g = 0; g < num_groups; ++g)
          nu_sigma_f[g] = nu[g] * sigma_f[g];
      }

      //check and normalize total fission spectrum
      {
        if (chi.empty())
          throw std::logic_error("Total fission spectrum not found.");
        if (!std::all_of(chi.begin(), chi.end(),
                         [](double x) { return x == 0.0; }))
          throw std::logic_error(
              "Invalid total fission spectrum encountered.\n"
              "Spectra must have at least one non-zero value.");

        //normalize the total fission spectrum
        double chi_sum = std::accumulate(chi.begin(), chi.end(), 0.0);
        for (unsigned int g = 0; g < num_groups; ++g)
          chi[g] /= chi_sum;
      }
    }//if total

    chi::log.Log0Verbose1() << "Fission data checks completed.";

  }//if fissionable

  //============================================================
  // Compute diffusion parameters
  //============================================================

  ComputeDiffusionParameters();
}
