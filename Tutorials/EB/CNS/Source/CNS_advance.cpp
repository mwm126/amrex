
#include <CNS.H>
#include <CNS_F.H>

using namespace amrex;

Real
CNS::advance (Real time, Real dt, int iteration, int ncycle)
{
    BL_PROFILE("CNS::advance()");
        
    for (int i = 0; i < num_state_data_types; ++i) {
        state[i].allocOldData();
        state[i].swapTimeLevels(dt);
    }

    MultiFab& S_new = get_new_data(State_Type);
    MultiFab dSdt(grids,dmap,NUM_STATE,0,MFInfo(),Factory());
    MultiFab Sborder(grids,dmap,NUM_STATE,NUM_GROW,MFInfo(),Factory());
  
    if (CNS::do_load_balance) {
        MultiFab& C_new = get_new_data(Cost_Type);
        C_new.setVal(0.0);
    }

    EBFluxRegister* fr_as_crse = nullptr;
    if (level < parent->finestLevel()) {
        CNS& fine_level = getLevel(level+1);
        fr_as_crse = &fine_level.flux_reg;
    }

    EBFluxRegister* fr_as_fine = nullptr;
    if (level > 0) {
        fr_as_fine = &flux_reg;
    }

    if (fr_as_crse) {
        fr_as_crse->reset();
    }

    // RK2 stage 1
    FillPatch(*this, Sborder, NUM_GROW, time, State_Type, 0, NUM_STATE);
    compute_dSdt(Sborder, dSdt, 0.5*dt, fr_as_crse, fr_as_fine);
    // U^* = U^n + dt*dUdt^n
    MultiFab::LinComb(S_new, 1.0, Sborder, 0, dt, dSdt, 0, 0, NUM_STATE, 0);
    computeTemp(S_new,0);
    
    // RK2 stage 2
    // After fillpatch Sborder = U^n+0.5*dt*dUdt^n
    FillPatch(*this, Sborder, NUM_GROW, time+0.5*dt, State_Type, 0, NUM_STATE);
    compute_dSdt(Sborder, dSdt, 0.5*dt, fr_as_crse, fr_as_fine);
    // U^{n+1} = (U^n+0.5*dt*dUdt^n) + 0.5*dt*dUdt^*
    MultiFab::LinComb(S_new, 1.0, Sborder, 0, 0.5*dt, dSdt, 0, 0, NUM_STATE, 0);
    computeTemp(S_new,0);
    
    return dt;
}

void
CNS::compute_dSdt (const MultiFab& S, MultiFab& dSdt, Real dt,
                   EBFluxRegister* fr_as_crse, EBFluxRegister* fr_as_fine)
{
    BL_PROFILE("CNS::compute_dSdt()");

    const Real* dx = geom.CellSize();
    const int ncomp = dSdt.nComp();

    int as_crse = (fr_as_crse != nullptr);
    int as_fine = (fr_as_fine != nullptr);

    MultiFab* cost = (do_load_balance) ? &(get_new_data(Cost_Type)) : nullptr;

#ifdef _OPENMP
#pragma omp parallel
#endif
    {
        std::array<FArrayBox,AMREX_SPACEDIM> flux;
        FArrayBox dm_ftoc(Box::TheUnitBox(),ncomp);
        FArrayBox fab_dm_as_crse(Box::TheUnitBox(),ncomp);
        IArrayBox fab_rrflag_as_crse(Box::TheUnitBox());

        for (MFIter mfi(S, MFItInfo().EnableTiling(hydro_tile_size).SetDynamic(true));
                        mfi.isValid(); ++mfi)
        {
            Real wt = ParallelDescriptor::second();

            const Box& bx = mfi.tilebox();

            const auto& sfab = dynamic_cast<EBFArrayBox const&>(S[mfi]);
            const auto& flag = sfab.getEBCellFlagFab();

            if (flag.getType(bx) == FabType::covered) {
                dSdt[mfi].setVal(0.0, bx, 0, ncomp);
            } else {

                // flux is used to store centroid flux needed for reflux
                for (int idim=0; idim < AMREX_SPACEDIM; ++idim) {
                    flux[idim].resize(amrex::surroundingNodes(bx,idim),ncomp);
                }

                if (flag.getType(amrex::grow(bx,1)) == FabType::regular)
                {
                    cns_compute_dudt(BL_TO_FORTRAN_BOX(bx),
                                     BL_TO_FORTRAN_ANYD(dSdt[mfi]),
                                     BL_TO_FORTRAN_ANYD(S[mfi]),
                                     BL_TO_FORTRAN_ANYD(flux[0]),
                                     BL_TO_FORTRAN_ANYD(flux[1]),
                                     BL_TO_FORTRAN_ANYD(flux[2]),
                                     dx, &dt);

                    if (fr_as_crse) {
                        fr_as_crse->CrseAdd(mfi,{&flux[0],&flux[1],&flux[2]},dx,dt);
                    }

                    if (fr_as_fine) {
                        fr_as_fine->FineAdd(mfi,{&flux[0],&flux[1],&flux[2]},dx,dt);
                    }
                }
                else
                {
                    FArrayBox* p_dm_as_crse = (fr_as_crse) ?
                        fr_as_crse->getCrseData(mfi) : &fab_dm_as_crse;
                    const IArrayBox* p_rrflag_as_crse = (fr_as_crse) ?
                        fr_as_crse->getCrseFlag(mfi) : &fab_rrflag_as_crse;

                    if (fr_as_fine) {
                        dm_ftoc.resize(amrex::grow(bx,crse_ratio),ncomp);
                    }

                    cns_eb_compute_dudt(BL_TO_FORTRAN_BOX(bx),
                                        BL_TO_FORTRAN_ANYD(dSdt[mfi]),
                                        BL_TO_FORTRAN_ANYD(S[mfi]),
                                        BL_TO_FORTRAN_ANYD(flux[0]),
                                        BL_TO_FORTRAN_ANYD(flux[1]),
                                        BL_TO_FORTRAN_ANYD(flux[2]),
                                        BL_TO_FORTRAN_ANYD(flag),
                                        BL_TO_FORTRAN_ANYD(volfrac[mfi]),
                                        BL_TO_FORTRAN_ANYD(bndrycent[mfi]),
                                        BL_TO_FORTRAN_ANYD(areafrac[0][mfi]),
                                        BL_TO_FORTRAN_ANYD(areafrac[1][mfi]),
                                        BL_TO_FORTRAN_ANYD(areafrac[2][mfi]),
                                        BL_TO_FORTRAN_ANYD(facecent[0][mfi]),
                                        BL_TO_FORTRAN_ANYD(facecent[1][mfi]),
                                        BL_TO_FORTRAN_ANYD(facecent[2][mfi]),
                                        &as_crse,
                                        BL_TO_FORTRAN_ANYD(*p_dm_as_crse),
                                        BL_TO_FORTRAN_ANYD(*p_rrflag_as_crse),
                                        &as_fine,
                                        BL_TO_FORTRAN_ANYD(dm_ftoc),
                                        BL_TO_FORTRAN_ANYD(level_mask[mfi]),
                                        dx, &dt);

                    if (fr_as_crse) {
                        fr_as_crse->CrseAdd(mfi, {&flux[0],&flux[1],&flux[2]}, dx,dt,
                                            volfrac[mfi],
                                            {&areafrac[0][mfi],
                                             &areafrac[1][mfi],
                                             &areafrac[2][mfi]});
                    }

                    if (fr_as_fine) {
                        fr_as_fine->FineAdd(mfi, {&flux[0],&flux[1],&flux[2]}, dx,dt,
                                            volfrac[mfi],
                                            {&areafrac[0][mfi],
                                             &areafrac[1][mfi],
                                             &areafrac[2][mfi]},
                                            dm_ftoc);
                    }
                }
            }

            if (do_load_balance) {
                wt = (ParallelDescriptor::second() - wt) / bx.d_numPts();
                (*cost)[mfi].plus(wt, bx);
            }
        }
    }
}
