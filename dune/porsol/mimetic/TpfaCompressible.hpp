/*
  Copyright 2010 SINTEF ICT, Applied Mathematics.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_TPFACOMPRESSIBLE_HEADER_INCLUDED
#define OPM_TPFACOMPRESSIBLE_HEADER_INCLUDED


#include "../opmpressure/src/TPFACompressiblePressureSolver.hpp"

#include <dune/common/ErrorMacros.hpp>
#include <dune/common/SparseTable.hpp>
#include <dune/porsol/common/LinearSolverISTL.hpp>

#include <tr1/array>

namespace Dune
{


    template <class GridInterface,
              class RockInterface,
              class FluidInterface,
              class WellsInterface,
              class BCInterface>
    class TpfaCompressible
    {
    public:
        typedef TPFACompressiblePressureSolver PressureSolver;

        /// @brief
        ///    Default constructor. Does nothing.
        TpfaCompressible()
            : pgrid_(0), prock_(0), pfluid_(0)
        {
        }


        /// @brief
        ///    Initializes run-time parameters of the solver.
        void init(const parameter::ParameterGroup& param)
        {
            // Initialize inflow mixture to a fixed, user-provided mix.
            typename FluidInterface::CompVec mix(0.0);
            const int nc = FluidInterface::numComponents;
            double inflow_mixture_gas = param.getDefault("inflow_mixture_gas", 1.0);
            double inflow_mixture_oil = param.getDefault("inflow_mixture_oil", 0.0);
            switch (nc) {
            case 2:
                mix[FluidInterface::Gas] = inflow_mixture_gas;
                mix[FluidInterface::Oil] = inflow_mixture_oil;
                break;
            case 3: {
                double inflow_mixture_water = param.getDefault("inflow_mixture_water", 0.0);
                mix[FluidInterface::Water] = inflow_mixture_water;
                mix[FluidInterface::Gas] = inflow_mixture_gas;
                mix[FluidInterface::Oil] = inflow_mixture_oil;
                break;
            }
            default:
                THROW("Unhandled number of components: " << nc);
            }
            inflow_mixture_ = mix;
            linsolver_.init(param);
            flux_rel_tol_ = param.getDefault("flux_rel_tol", 1e-5);
            press_rel_tol_ = param.getDefault("press_rel_tol", 1e-5);
            max_num_iter_ = param.getDefault("max_num_iter", 15);
            max_relative_voldiscr_ = param.getDefault("max_relative_voldiscr", 0.15);
            relax_time_voldiscr_ = param.getDefault("relax_time_voldiscr", 0.0);
            relax_weight_pressure_iteration_ = param.getDefault("relax_weight_pressure_iteration", 1.0);
            experimental_jacobian_ = param.getDefault("experimental_jacobian", false);
            output_residual_ = param.getDefault("output_residual", false);
        }




        /// @brief
        ///    Accessor for the inflow mixture.
        typename FluidInterface::CompVec inflowMixture() const
        {
            return inflow_mixture_;
        }




        /// @brief
        ///    Setup routine, does grid/rock-dependent initialization.
        ///
        /// @param [in] grid
        ///    The grid.
        ///
        /// @param [in] rock
        ///    The cell-wise permeabilities and porosities.
        ///
        /// @param [in] fluid
        ///    Fluid properties.
        ///
        /// @param [in] wells
        ///    Well specifications.
        ///
        /// @param [in] grav
        ///    Gravity vector.  Its Euclidian two-norm value
        ///    represents the strength of the gravity field (in units
        ///    of m/s^2) while its direction is the direction of
        ///    gravity in the current model.
        ///
        /// @param [in] bc
        ///    Boundary conditions.
        ///
        void setup(const GridInterface&         grid,
                   const RockInterface&         rock,
                   const FluidInterface&        fluid,
                   const WellsInterface&        wells,
                   const typename GridInterface::Vector& grav,
                   const BCInterface& bc)
        {
            pgrid_ = &grid;
            prock_ = &rock;
            pfluid_ = &fluid;
            pwells_ = &wells;
            gravity_ = grav;

            // Extract perm tensors.
            const double* perm = &(rock.permeability(0)(0,0));
            poro_.clear();
            poro_.resize(grid.numCells(), 1.0);
            for (int i = 0; i < grid.numCells(); ++i) {
                poro_[i] = rock.porosity(i);
            }
            // Initialize 
            psolver_.init(grid, wells, perm, &poro_[0], grav);

            // Build bctypes_ and bcvalues_.
            int num_faces = grid.numFaces();
            bctypes_.clear();
            bctypes_.resize(num_faces, PressureSolver::FBC_UNSET);
            bcvalues_.clear();
            bcvalues_.resize(num_faces, 0.0);
            for (int face = 0; face < num_faces; ++face) {
                int bid = pgrid_->boundaryId(face);
                if (bid == 0) {
                    bctypes_[face] = PressureSolver::FBC_UNSET;
                    continue;
                }
                FlowBC face_bc = bc.flowCond(bid);
                if (face_bc.isDirichlet()) {
                    bctypes_[face] = PressureSolver::FBC_PRESSURE;
                    bcvalues_[face] = face_bc.pressure();
                } else if (face_bc.isNeumann()) {
                    bctypes_[face] = PressureSolver::FBC_FLUX;
                    bcvalues_[face] = face_bc.outflux(); // TODO: may have to switch sign here depending on orientation.
                    if (bcvalues_[face] != 0.0) {
                        THROW("Nonzero Neumann conditions not yet properly implemented "
                              "(signs must be fixed, also face pressures are not correctly computed for this case)");
                    }
                } else {
                    THROW("Unhandled boundary condition type.");
                }
            }

            // Setup unchanging well data structures.
            perf_wells_.clear();
            perf_cells_.clear();
            perf_pressure_.clear();
            perf_A_.clear();
            perf_mob_.clear();
            perf_sat_.clear();
            int num_wells = pwells_->numWells();
            for (int well = 0; well < num_wells; ++well) {
                int num_perf = pwells_->numPerforations(well);
                for (int perf = 0; perf < num_perf; ++perf) {
                    int cell = pwells_->wellCell(well, perf);
                    perf_wells_.push_back(well);
                    perf_cells_.push_back(cell);
                    perf_pressure_.push_back(pwells_->perforationPressure(cell));
                }
            }
            int num_perf = perf_wells_.size();
            perf_A_.resize(num_perf*numPhases*numComponents);
            perf_mob_.resize(num_perf*numPhases);
            perf_sat_.resize(num_perf);
        }




        double volumeDiscrepancyLimit() const
        {
            return max_relative_voldiscr_;
        }




        const std::vector<double>& faceTransmissibilities()
        {
            return psolver_.faceTransmissibilities();
        }




        bool volumeDiscrepancyAcceptable(const std::vector<typename FluidInterface::PhaseVec>& cell_pressure,
                                         const std::vector<typename FluidInterface::PhaseVec>& face_pressure,
                                         const std::vector<typename FluidInterface::CompVec>& cell_z,
                                         const double dt)
        {
            computeFluidProps(cell_pressure, face_pressure, cell_z, dt);
            double rel_voldiscr = *std::max_element(fp_.relvoldiscr.begin(), fp_.relvoldiscr.end());
            if (rel_voldiscr > max_relative_voldiscr_) {
                std::cout << "    Relative volume discrepancy too large: " << rel_voldiscr << std::endl;
                return false;
            } else {
                std::cout << "    Relative volume discrepancy ok: " << rel_voldiscr << std::endl;
                return true;
            }
        }

        enum ReturnCode { SolveOk, VolumeDiscrepancyTooLarge, FailedToConverge };


        /// @brief
        ///    Construct and solve system of linear equations for the
        ///    phase pressure values on cells and faces, also compute
        ///    total face fluxes.
        ///
        /// @param [inout] cell_pressure
        ///    Phase pressures per cell.
        ///
        /// @param [inout] face_pressure
        ///    Phase pressures per face.
        ///
        /// @param [inout] cell_z
        ///    Surface volume per cell. Only changed if the @code
        ///    transport @endcode argument is true.
        ///
        /// @param [out] face_flux
        ///    Total (summed over all phases) volume flux (signed)
        ///    across each face.
        ///
        /// @param [out] well_perf_pressures
        ///    Pressure in each well perforation.
        ///
        /// @param [out] well_perf_fluxes
        ///    Total (summed over all phases) volume flux (signed,
        ///    positive meaning injection) from each well perforation.
        ///
        /// @param [in] src
        ///    Explicit source terms.  One scalar value for each grid
        ///    cell representing the rate (in units of m^3/s) of fluid
        ///    being injected into (>0) or extracted from (<0) a given
        ///    grid cell.
        ///
        /// @param [in] dt
        ///    Timestep for pressure solver.
        ///
        /// @param [in] transport
        ///    If true, modify @code cell_z @endcode by IMPES scheme.
        ///
        ReturnCode solve(std::vector<typename FluidInterface::PhaseVec>& cell_pressure,
                         std::vector<typename FluidInterface::PhaseVec>& face_pressure,
                         std::vector<typename FluidInterface::CompVec>& cell_z,
                         std::vector<double>& face_flux,
                         std::vector<double>& well_perf_pressures,
                         std::vector<double>& well_perf_fluxes,
                         const std::vector<double>& src,
                         const double dt)
        {
            perf_pressure_ = well_perf_pressures;

            int num_cells = cell_z.size();
            std::vector<double> cell_pressure_scalar_initial(num_cells);
            // Set initial pressure to Liquid phase pressure. \TODO what is correct with capillary pressure?
            for (int cell = 0; cell < num_cells; ++cell) {
                cell_pressure_scalar_initial[cell] = cell_pressure[cell][FluidInterface::Liquid];
            }
            std::vector<double> cell_pressure_scalar = cell_pressure_scalar_initial;
            std::vector<double> initial_voldiscr;
            std::vector<double> face_pressure_scalar;
            std::vector<double> start_face_flux;
            std::vector<double> start_face_pressure;
            std::vector<double> start_cell_press;
            std::vector<double> well_bhp(pwells_->numWells(), 0.0);
            std::vector<double> start_perf_flux;
            int num_faces = pgrid_->numFaces();
            face_flux.clear();
            face_flux.resize(num_faces, 0.0);
            face_pressure_scalar.clear();
            face_pressure_scalar.resize(num_faces, 0.0);

            // ------------  Main iteration loop -------------
            for (int iter = 0; iter < max_num_iter_; ++iter) {
                start_face_flux = face_flux;
                start_face_pressure = face_pressure_scalar;
                start_cell_press = cell_pressure_scalar;
                start_perf_flux = well_perf_fluxes;
                // (Re-)compute fluid properties.
                computeFluidProps(cell_pressure, face_pressure, cell_z, dt);

                // Initialization for the first iteration only.
                if (iter == 0) {
                    initial_voldiscr = fp_.voldiscr;
                    double rel_voldiscr = *std::max_element(fp_.relvoldiscr.begin(), fp_.relvoldiscr.end());
                    if (rel_voldiscr > max_relative_voldiscr_) {
                        std::cout << "    Relative volume discrepancy too large: " << rel_voldiscr << std::endl;
                        return VolumeDiscrepancyTooLarge;
                    }
                    if (relax_time_voldiscr_ > 0.0) {
                        double relax = std::min(1.0,dt/relax_time_voldiscr_);
                        std::transform(initial_voldiscr.begin(), initial_voldiscr.end(), initial_voldiscr.begin(),
                                       std::binder1st<std::multiplies<double> >(std::multiplies<double>() , relax));
                    }

                    // well_gpot is computed once per pressure solve,
                    // while perf_A_, perf_mob_ are recoomputed
                    // for every iteration.
                    computeWellPotentials(perf_gpot_);
                }

                if (experimental_jacobian_) {
                    // Compute residual and jacobian.
                    PressureSolver::LinearSystem s;
                    std::vector<double> res;
                    computeResidualJacobian(initial_voldiscr, cell_pressure_scalar, cell_pressure_scalar_initial,
                                            well_bhp, src, dt, s, res);

                    if (output_residual_) {
                        // Temporary hack to get output of residual.
                        static int psolve_iter = -1;
                        if (iter == 0) {
                            ++psolve_iter;
                        }
                        std::ostringstream oss;
                        oss << "residual-" << psolve_iter << '-' << iter << ".dat";
                        std::ofstream outres(oss.str().c_str());
                        std::copy(res.begin(), res.end(), std::ostream_iterator<double>(outres, "\n"));
                    }

                    // Solve system for dp, that is, we use res as the rhs.
                    LinearSolverISTL::LinearSolverResults result
                        = linsolver_.solve(s.n, s.nnz, s.ia, s.ja, s.sa, &res[0], s.x);
                    if (!result.converged) {
                        THROW("Linear solver failed to converge in " << result.iterations << " iterations.\n"
                              << "Residual reduction achieved is " << result.reduction << '\n');
                    }
                    // Set x so that the call to computePressuresAndFluxes() will work.
                    // Recall that x now contains dp, and we want it to contain p - dp
                    for (int cell = 0; cell < num_cells; ++cell) {
                        s.x[cell] = cell_pressure_scalar[cell] - s.x[cell];
                    }
                    for (int well = 0; well < pwells_->numWells(); ++well) {
                        s.x[num_cells + well] = well_bhp[well] - s.x[num_cells + well]; 
                    }
                } else {
                    // Assemble system matrix and rhs.
                    psolver_.assemble(src, bctypes_, bcvalues_, dt,
                                      fp_.totcompr, initial_voldiscr, fp_.cellA, fp_.faceA,
                                      perf_A_, fp_.phasemobf, perf_mob_,
                                      cell_pressure_scalar_initial, fp_.gravcapf,
                                      perf_gpot_, &(pfluid_->surfaceDensities()[0]));
                    PressureSolver::LinearSystem s;
                    psolver_.linearSystem(s);
                    // Solve system.
                    LinearSolverISTL::LinearSolverResults res = linsolver_.solve(s.n, s.nnz, s.ia, s.ja, s.sa, s.b, s.x);
                    if (!res.converged) {
                        THROW("Linear solver failed to converge in " << res.iterations << " iterations.\n"
                              << "Residual reduction achieved is " << res.reduction << '\n');
                    }
                }

                // Get pressures and face fluxes.
                psolver_.computePressuresAndFluxes(cell_pressure_scalar, face_pressure_scalar, face_flux,
                                                   well_bhp, well_perf_fluxes);

                // Relaxation
                if (relax_weight_pressure_iteration_ != 1.0) {
                    double ww = relax_weight_pressure_iteration_;
                    for (int cell = 0; cell < num_cells; ++cell) {
                        cell_pressure_scalar[cell] = ww*cell_pressure_scalar[cell] + (1.0-ww)*start_cell_press[cell];
                    }
                    if (iter > 0) {
                        for (int face = 0; face < num_faces; ++face) {
                            face_pressure_scalar[face] = ww*face_pressure_scalar[face] + (1.0-ww)*start_face_pressure[face];
                            face_flux[face] = ww*face_flux[face] + (1.0-ww)*start_face_flux[face];
                        }
                    }
                }
                             
                // Copy to phase pressures. \TODO handle capillary pressure.
                for (int cell = 0; cell < num_cells; ++cell) {
                    cell_pressure[cell] = cell_pressure_scalar[cell];
                }
                for (int face = 0; face < num_faces; ++face) {
                    face_pressure[face] = face_pressure_scalar[face];
                }

                // Compute well_perf_pressures
                computeWellPerfPressures(well_perf_fluxes, well_bhp, perf_gpot_, well_perf_pressures);

                // Update internal well pressure vector.
                perf_pressure_ = well_perf_pressures;

                // Compute relative changes for pressure and flux.
                std::pair<double, double> rel_changes
                    = computeFluxPressChanges(face_flux, well_perf_fluxes, cell_pressure_scalar,
                                              start_face_flux, start_perf_flux, start_cell_press);
                double flux_rel_difference = rel_changes.first;
                double press_rel_difference = rel_changes.second;

                // Test for convergence.
                if (iter == 0) {
                    std::cout << "Iteration      Rel. flux change     Rel. pressure change\n";
                }
                std::cout.precision(5);
                std::cout << std::setw(6) << iter
                          << std::setw(24) << flux_rel_difference
                          << std::setw(24) << press_rel_difference << std::endl;
                std::cout.precision(16);

                if (flux_rel_difference < flux_rel_tol_ || press_rel_difference < press_rel_tol_) {
                    std::cout << "Pressure solver converged. Number of iterations: " << iter + 1 << '\n' << std::endl;
                    return SolveOk;
                }
            }

            return FailedToConverge;
        }




        /// Call this function after solve().
        double stableStepIMPES()
        {
            return psolver_.explicitTimestepLimit(fp_.faceA,
                                                  fp_.phasemobf,
                                                  fp_.phasemobf_deriv,
                                                  &(pfluid_->surfaceDensities()[0]));
        }




        void doStepIMPES(std::vector<typename FluidInterface::CompVec>& cell_z,
                         const double dt)
        {
            psolver_.explicitTransport(dt, &(cell_z[0][0]));
        }




    private:
        const GridInterface* pgrid_;
        const RockInterface* prock_;
        const FluidInterface* pfluid_;
        const WellsInterface* pwells_;
        typename GridInterface::Vector gravity_;
        typename FluidInterface::FluidData fp_;
        std::vector<double> poro_;
        PressureSolver psolver_;
        LinearSolverISTL linsolver_;
        std::vector<PressureSolver::FlowBCTypes> bctypes_;
        std::vector<double> bcvalues_;

        typename FluidInterface::CompVec inflow_mixture_;
        double flux_rel_tol_;
        double press_rel_tol_;
        int max_num_iter_;
        double max_relative_voldiscr_;
        double relax_time_voldiscr_;
        double relax_weight_pressure_iteration_;
        bool experimental_jacobian_;
        bool output_residual_;

        typedef typename FluidInterface::PhaseVec PhaseVec;
        typedef typename FluidInterface::CompVec CompVec;
        enum { numPhases = FluidInterface::numPhases,
               numComponents = FluidInterface::numComponents };

        struct PrivateFluidData
        {
            PhaseVec saturation;
            PhaseVec mobility;
            std::tr1::array<double, numPhases*numComponents> A;
        };
        std::vector<int> perf_wells_;
        std::vector<int> perf_cells_;
        std::vector<double> perf_pressure_;
        std::vector<double> perf_A_;   // Flat storage.
        std::vector<double> perf_mob_; // Flat storage.
        std::vector<PhaseVec> perf_sat_;
        std::vector<double> perf_gpot_; // Flat storage.



        PrivateFluidData computeProps(const PhaseVec& pressure,
                                      const CompVec& composition)
        {
            typename FluidInterface::FluidState state = pfluid_->computeState(pressure, composition);
            PrivateFluidData data;
            data.saturation = state.saturation_;
            data.mobility = state.mobility_;
            std::copy(state.phase_to_comp_, state.phase_to_comp_ + numComponents*numPhases, &data.A[0]);
            return data;
        }



        void computeFluidProps(const std::vector<typename FluidInterface::PhaseVec>& phase_pressure,
                               const std::vector<typename FluidInterface::PhaseVec>& phase_pressure_face,
                               const std::vector<typename FluidInterface::CompVec>& cell_z,
                               const double dt)
        {
            fp_.compute(*pgrid_, *prock_, *pfluid_, gravity_, phase_pressure, phase_pressure_face, cell_z, inflow_mixture_, dt);
            // Properties at well perforations.
            // \TODO only need to recompute this once per pressure update.
            // No, that is false, at production perforations the cell z is
            // used, which may change every step.
            unsigned int perfcount = 0;
            int num_wells = pwells_->numWells();
            for (int well = 0; well < num_wells; ++well) {
                bool inj = pwells_->type(well) == WellsInterface::Injector;
                int num_perf = pwells_->numPerforations(well);
                for (int perf = 0; perf < num_perf; ++perf) {
                    int cell = pwells_->wellCell(well, perf);
                    // \TODO handle capillary in perforation pressure below?
                    PhaseVec well_pressure = inj ? PhaseVec(perf_pressure_[perf]) : phase_pressure[cell];
                    CompVec well_mixture = inj ? pwells_->injectionMixture(cell) : cell_z[cell];
                    typename FluidInterface::FluidState state = pfluid_->computeState(well_pressure, well_mixture);
                    std::copy(state.phase_to_comp_, state.phase_to_comp_ + numComponents*numPhases,
                              &perf_A_[perfcount*numPhases*numComponents]);
                    std::copy(state.mobility_.begin(), state.mobility_.end(),
                              &perf_mob_[perfcount*numPhases]);
                    perf_sat_[perfcount] = state.saturation_;
                    ++perfcount;
                }
            }
            ASSERT(perfcount == perf_wells_.size());
        }



        // Compute res = Ax - b.
        void computeLinearResidual(const PressureSolver::LinearSystem& s, std::vector<double>& res)
        {
            res.resize(s.n);
            for (int row = 0; row < s.n; ++row) {
                res[row] = -s.b[row];
                for (int i = s.ia[row]; i < s.ia[row + 1]; ++i) {
                    res[row] += s.sa[i]*s.x[s.ja[i]];
                }
            }
        }



        // Compute residual and Jacobian of the new formulation.
        void computeResidualJacobian(const std::vector<double>& initial_voldiscr,
                                     const std::vector<double>& cell_pressure_scalar,
                                     const std::vector<double>& cell_pressure_scalar_initial,
                                     const std::vector<double>& well_bhp,
                                     const std::vector<double>& src,
                                     const double dt,
                                     PressureSolver::LinearSystem& linsys,
                                     std::vector<double>& res)
        {
            // Assemble system matrix and rhs.
            psolver_.assemble(src, bctypes_, bcvalues_, dt,
                              fp_.totcompr, initial_voldiscr, fp_.cellA, fp_.faceA,
                              perf_A_, fp_.phasemobf, perf_mob_,
                              cell_pressure_scalar_initial, fp_.gravcapf,
                              perf_gpot_, &(pfluid_->surfaceDensities()[0]));
            psolver_.linearSystem(linsys);
            // The linear system is for direct evaluation, we want a residual based approach.
            // First we compute the residual for the original code.
            int num_cells = pgrid_->numCells();
            std::copy(cell_pressure_scalar.begin(), cell_pressure_scalar.end(), linsys.x);
            std::copy(well_bhp.begin(), well_bhp.end(), linsys.x + num_cells);
            computeLinearResidual(linsys, res);
            // Then we compute the residual we actually want by subtracting terms that do not
            // appear in the new formulation and adding the new terms.
            for (int cell = 0; cell < num_cells; ++cell) {
                double dres = fp_.totcompr[cell]*(cell_pressure_scalar[cell] - cell_pressure_scalar_initial[cell]);
                dres -= 1.0 - fp_.totphasevol_density[cell];
                dres *= pgrid_->cellVolume(cell)*prock_->porosity(cell)/dt;
                res[cell] -= dres;
            }
            // Change the jacobian by adding/subtracting the necessary terms.
            for (int cell = 0; cell < num_cells; ++cell) {
                for (int i = linsys.ia[cell]; i < linsys.ia[cell + 1]; ++i) {
                    if  (linsys.ja[i] == cell) {
                        linsys.sa[i] -= fp_.totcompr[cell]*pgrid_->cellVolume(cell)*prock_->porosity(cell)/dt;
                        linsys.sa[i] += fp_.expjacterm[cell]*pgrid_->cellVolume(cell)*prock_->porosity(cell)/dt;
                    }
                }
            }
        }



        // Compute the well potentials. Assumes that the perforation variables
        // have been set properly: perf_[wells_|cells_|pressure_|props_].
        void computeWellPotentials(std::vector<double>& wellperf_gpot) const
        {
            int num_perf = perf_cells_.size();
            wellperf_gpot.resize(num_perf*numPhases);
            for (int perf = 0; perf < num_perf; ++perf) {
                int well = perf_wells_[perf];
                int cell = perf_cells_[perf];
                typename GridInterface::Vector pos = pgrid_->cellCentroid(cell);
                // With wells, we assume that gravity is in the z-direction.
                ASSERT(gravity_[0] == 0.0 && gravity_[1] == 0.0);
                double depth_delta = pos[2] - pwells_->referenceDepth(well);
                double gh = gravity_[2]*depth_delta;
                // At is already transposed since in Fortran order.
                const double* At = &perf_A_[perf*numPhases*numComponents];
                PhaseVec rho = pfluid_->phaseDensities(At);
                for (int phase = 0; phase < numPhases; ++phase) {
                    // Gravity potential is (by phase) \rho_\alpha g h
                    wellperf_gpot[numPhases*perf + phase] = rho[phase]*gh;
                }
            }
        }



        // Compute the relative changes in fluxes and pressures.
        static std::pair<double, double>
        computeFluxPressChanges(const std::vector<double>& face_flux,
                                const std::vector<double>& well_perf_fluxes,
                                const std::vector<double>& cell_pressure_scalar,
                                const std::vector<double>& start_face_flux,
                                const std::vector<double>& start_perf_flux,
                                const std::vector<double>& start_cell_press)
        {
            int num_faces = face_flux.size();
            int num_perf = well_perf_fluxes.size();
            int num_cells = cell_pressure_scalar.size();
            double max_flux_face = std::max(std::fabs(*std::min_element(face_flux.begin(), face_flux.end())),
                                            std::fabs(*std::max_element(face_flux.begin(), face_flux.end())));
            double max_flux_perf = num_perf == 0 ? 0.0
                : std::max(std::fabs(*std::min_element(well_perf_fluxes.begin(), well_perf_fluxes.end())),
                           std::fabs(*std::max_element(well_perf_fluxes.begin(), well_perf_fluxes.end())));
            double max_flux = std::max(max_flux_face, max_flux_perf);
            double max_press = std::max(std::fabs(*std::min_element(cell_pressure_scalar.begin(),
                                                                    cell_pressure_scalar.end())),
                                        std::fabs(*std::max_element(cell_pressure_scalar.begin(),
                                                                    cell_pressure_scalar.end())));
            double flux_change_infnorm = 0.0;
            double press_change_infnorm = 0.0;
            for (int face = 0; face < num_faces; ++face) {
                flux_change_infnorm = std::max(flux_change_infnorm,
                                               std::fabs(face_flux[face] - start_face_flux[face]));
            }
            for (int perf = 0; perf < num_perf; ++perf) {
                flux_change_infnorm = std::max(flux_change_infnorm,
                                               std::fabs(well_perf_fluxes[perf] - start_perf_flux[perf]));
            }
            for (int cell = 0; cell < num_cells; ++cell) {
                press_change_infnorm = std::max(press_change_infnorm,
                                                std::fabs(cell_pressure_scalar[cell] - start_cell_press[cell]));
            }
            double flux_rel_difference = flux_change_infnorm/max_flux;
            double press_rel_difference = press_change_infnorm/max_press;
            return std::make_pair(flux_rel_difference, press_rel_difference);
        }



        // Compute well perforation pressures.
        void computeWellPerfPressures(const std::vector<double>& well_perf_fluxes,
                                      const std::vector<double>& well_bhp,
                                      const std::vector<double>& wellperf_gpot,
                                      std::vector<double>& well_perf_pressures) const
        {
            // Compute averaged saturations for each well. This code
            // assumes that flow is either in or out of any single
            // well, not both.
            int num_perf = well_perf_fluxes.size();
            std::vector<PhaseVec> well_sat(pwells_->numWells(), PhaseVec(0.0));
            std::vector<double> well_flux(pwells_->numWells(), 0.0);
            for (int perf = 0; perf < num_perf; ++perf) {
                int well = perf_wells_[perf];
                double flux = well_perf_fluxes[perf];
                well_flux[well] += flux;
                PhaseVec tmp = perf_sat_[perf];
                tmp *= flux;
                well_sat[well] += tmp;
            }
            for (int well = 0; well < pwells_->numWells(); ++well) {
                well_sat[well] *= 1.0/well_flux[well];
            }

            // Compute well_perf_pressures
            for (int perf = 0; perf < num_perf; ++perf) {
                well_perf_pressures[perf] = well_bhp[perf_wells_[perf]];
                PhaseVec sat = well_sat[perf_wells_[perf]];
                for (int phase = 0; phase < numPhases; ++phase) {
                    well_perf_pressures[perf]
                        += sat[phase]*wellperf_gpot[numPhases*perf + phase];
                }
            }
        }

    };


} // namespace Dune



#endif // OPM_TPFACOMPRESSIBLE_HEADER_INCLUDED
