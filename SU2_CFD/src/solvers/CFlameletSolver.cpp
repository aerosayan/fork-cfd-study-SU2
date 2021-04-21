/*!
 * \file CFlameletSolver.cpp
 * \brief Main subroutines for solving transported scalar class
 * \author T. Economon, N. Beishuizen, D. Mayer
 * \version 7.1.1 "Blackbird"
 *
 * SU2 Project Website: https://su2code.github.io
 *
 * The SU2 Project is maintained by the SU2 Foundation
 * (http://su2foundation.org)
 *
 * Copyright 2012-2020, SU2 Contributors (cf. AUTHORS.md)
 *
 * SU2 is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * SU2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with SU2. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../../include/solvers/CFlameletSolver.hpp"
#include "../../include/variables/CFlameletVariable.hpp"
#include "../../include/fluid/CFluidFlamelet.hpp"
#include "../../../Common/include/parallelization/omp_structure.hpp"
#include "../../../Common/include/toolboxes/geometry_toolbox.hpp"

CFlameletSolver::CFlameletSolver(void) : CScalarSolver() {}

CFlameletSolver::CFlameletSolver(CGeometry *geometry,
                                 CConfig *config,
                                 unsigned short iMesh)
: CScalarSolver(geometry, config) {
  unsigned short nLineLets;
  bool turbulent = ((config->GetKind_Solver() == RANS) ||
                    (config->GetKind_Solver() == DISC_ADJ_RANS));
  bool turb_SST  = ((turbulent) && (config->GetKind_Turb_Model() == SST));
  bool turb_SA   = ((turbulent) && (config->GetKind_Turb_Model() == SA));
  bool multizone = config->GetMultizone_Problem();

  nVar     = config->GetNScalars();
  nPrimVar = config->GetNScalars();

  nPoint       = geometry->GetnPoint();
  nPointDomain = geometry->GetnPointDomain();

  /*--- Initialize nVarGrad for deallocation ---*/

  nVarGrad = nVar;

  /*--- Define geometry constants in the solver structure ---*/

  nDim = geometry->GetnDim();
  
  /*--- Fluid model pointer initialization ---*/
  
  FluidModel = nullptr;
  
  /*--- Single grid simulation ---*/

  /*--- Define some auxiliary vector related with the solution ---*/
  Solution   = new su2double[nVar];
  Solution_i = new su2double[nVar];
  Solution_j = new su2double[nVar];

  /*--- do not see a reason to use only single grid ---*/
  //if (iMesh == MESH_0) {

    /*--- Define some auxiliary vector related with the residual ---*/

    Residual_RMS.resize(nVar,0.0);
    Residual_Max.resize(nVar,0.0);
    Point_Max.resize(nVar,0);
    Point_Max_Coord.resize(nVar,nDim) = su2double(0.0);

    /*--- Initialization of the structure of the whole Jacobian ---*/

    if (rank == MASTER_NODE) cout << "Initialize Jacobian structure (Flamelet model)." << endl;
    Jacobian.Initialize(nPoint, nPointDomain, nVar, nVar, true, geometry, config, ReducerStrategy);

    if (config->GetKind_Linear_Solver_Prec() == LINELET) {
      nLineLets = Jacobian.BuildLineletPreconditioner(geometry, config);
      if (rank == MASTER_NODE) cout << "Compute linelet structure. " << nLineLets << " elements in each line (average)." << endl;
    }

    LinSysSol.Initialize(nPoint, nPointDomain, nVar, 0.0);
    LinSysRes.Initialize(nPoint, nPointDomain, nVar, 0.0);
    System.SetxIsZero(true);

    if (ReducerStrategy)
      EdgeFluxes.Initialize(geometry->GetnEdge(), geometry->GetnEdge(), nVar, nullptr);

    /*--- Initialize the BGS residuals in multizone problems. ---*/
    if (multizone){
      Residual_BGS.resize(nVar,0.0);
      Residual_Max_BGS.resize(nVar,0.0);
      Point_Max_BGS.resize(nVar,0);
      Point_Max_Coord_BGS.resize(nVar,nDim) = su2double(0.0);
    }

  //} //iMESH_0

  /*--- Initialize lower and upper limits---*/

  lowerlimit = new su2double[nVar];
  upperlimit = new su2double[nVar];
  if (config->GetScalar_Clipping()){
    for (auto iVar=0u;iVar<nVar;iVar++){
      lowerlimit[iVar] = config->GetScalar_Clipping_Min(iVar);
      upperlimit[iVar] = config->GetScalar_Clipping_Max(iVar);
    }
  }
  else {
    for (auto iVar=0u;iVar<nVar;iVar++){
      lowerlimit[iVar] = -1.0e15;
      upperlimit[iVar] =  1.0e15;
    }
  }
  /*--- Far-field flow state quantities and initialization. ---*/
  //su2double Density_Inf, Viscosity_Inf;
  //Density_Inf   = config->GetDensity_FreeStreamND();
  //Viscosity_Inf = config->GetViscosity_FreeStreamND();

  /*--- Set up fluid model for the diffusivity ---*/

  su2double Diffusivity_Ref = 1.0;
  su2double DiffusivityND = config->GetDiffusivity_Constant()/Diffusivity_Ref;
  config->SetDiffusivity_ConstantND(DiffusivityND);

  //nijso: we probably do not need this for cflamelet
  //FluidModel = new CFluidModel();
  //FluidModel->SetMassDiffusivityModel(config);

  /*--- Scalar variable state at the far-field. ---*/

  Scalar_Inf = new su2double[nVar];
  for (auto iVar = 0u; iVar < nVar; iVar++){
    Scalar_Inf[iVar] = config->GetScalar_Init(iVar);
  }

  /*--- Initialize the solution to the far-field state everywhere. ---*/

  nodes = new CFlameletVariable(Scalar_Inf, nPoint, nDim, nVar, config);


  //nijso: we probably do not need this for flamelet model
  /*--- initialize the mass diffusivity ---*/
  // for (auto iVar = 0u; iVar < nVar; iVar++){
  //   auto mass_diff = FluidModel->GetMassDiffusivity(); // returns a su2double, note that for more species this should be a vector
  //   // loop over all points and set diffusivity
  //   // why construct the entire diffusivity matrix?
  //  for (unsigned long iPoint = 0; iPoint < nPoint; iPoint++)
  //    nodes->SetDiffusivity(iPoint, mass_diff, iVar);
  // }

  SetBaseClassPointerToNodes();

  /*--- MPI solution ---*/

  InitiateComms(geometry, config, SOLUTION);
  CompleteComms(geometry, config, SOLUTION);

  /*--- Initialize quantities for SlidingMesh Interface ---*/

  SlidingState.resize(nMarker);
  SlidingStateNodes.resize(nMarker);

  for (unsigned long iMarker = 0; iMarker < nMarker; iMarker++) {
    if (config->GetMarker_All_KindBC(iMarker) == FLUID_INTERFACE) {
      SlidingState[iMarker].resize(nVertex[iMarker], nPrimVar+1) = nullptr;
      SlidingStateNodes[iMarker].resize(nVertex[iMarker],0);
    }
  }


  /*-- Allocation of inlets has to happen in derived classes
   (not CScalarSolver), due to arbitrary number of scalar variables.
   First, we also set the column index for any inlet profiles. ---*/
  
  Inlet_Position = nDim*2+2;
  if (turbulent) {
    if (turb_SA) Inlet_Position += 1;
    else if (turb_SST) Inlet_Position += 2;
  }

  Inlet_ScalarVars.resize(nMarker);
  for (unsigned long iMarker = 0; iMarker < nMarker; iMarker++){
    Inlet_ScalarVars[iMarker].resize(nVertex[iMarker],nVar);
    for (unsigned long iVertex = 0; iVertex < nVertex[iMarker]; ++iVertex) {
      for (unsigned short iVar = 0; iVar < nVar; iVar++){
        Inlet_ScalarVars[iMarker](iVertex,iVar) = Scalar_Inf[iVar];
      }
    }
  }

  /*--- The turbulence models are always solved implicitly, so set the
  implicit flag in case we have periodic BCs. ---*/

  SetImplicitPeriodic(true);

  /*--- Store the initial CFL number for all grid points. ---*/
 
  const su2double CFL = config->GetCFL(MGLevel)*config->GetCFLRedCoeff_Scalar();
  for (auto i_point = 0u; i_point < nPoint; i_point++) {
    nodes->SetLocalCFL(i_point, CFL);
  }
  Min_CFL_Local = CFL;
  Max_CFL_Local = CFL;
  Avg_CFL_Local = CFL;

  /*--- Add the solver name (max 8 characters) ---*/
  SolverName = "FLAMELET";

}

CFlameletSolver::~CFlameletSolver(void) {
  
  unsigned long iMarker, iVertex;
  unsigned short iVar;
  
  if (FluidModel != nullptr) delete FluidModel;

}


void CFlameletSolver::Preprocessing(CGeometry *geometry, CSolver **solver_container, CConfig *config,
         unsigned short iMesh, unsigned short iRKStep, unsigned short RunTime_EqSystem, bool Output) {

  unsigned long n_not_in_domain     = 0;
  unsigned long global_table_misses = 0;

  const bool implicit = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  const bool muscl    =  config->GetMUSCL_Scalar();
  const bool limiter  = (config->GetKind_SlopeLimit_Scalar() != NO_LIMITER) &&
                        (config->GetInnerIter() <= config->GetLimiterIter());

  for (auto i_point = 0u; i_point < nPoint; i_point++) {

    CFluidModel * fluid_model_local = solver_container[FLOW_SOL]->GetFluidModel();
    su2double * scalars = nodes->GetSolution(i_point);

    n_not_in_domain += fluid_model_local->SetTDState_T(0,scalars); /*--- first arguemnt (temperature) is not used ---*/

    for(auto i_scalar = 0u; i_scalar < config->GetNScalars(); ++i_scalar){
      nodes->SetDiffusivity(i_point, fluid_model_local->GetMassDiffusivity(), i_scalar);
    }

    if (!Output) LinSysRes.SetBlock_Zero(i_point);

  }

  if (config->GetComm_Level() == COMM_FULL) {
    SU2_MPI::Reduce(&n_not_in_domain, &global_table_misses, 1, MPI_UNSIGNED_LONG, MPI_SUM, MASTER_NODE, SU2_MPI::GetComm());
    if (rank == MASTER_NODE) {
      SetNTableMisses(global_table_misses);
    } 
  }

  /*--- Clear residual and system matrix, not needed for
   * reducer strategy as we write over the entire matrix. ---*/
  // nijso: this makes the residuals unavailable for output
  if (!ReducerStrategy && !Output) {
    LinSysRes.SetValZero();
    if (implicit) Jacobian.SetValZero();
    else {SU2_OMP_BARRIER}
  }

  /*--- Upwind second order reconstruction and gradients ---*/

  if (config->GetReconstructionGradientRequired()) {
    if (config->GetKind_Gradient_Method_Recon() == GREEN_GAUSS)
      SetSolution_Gradient_GG(geometry, config, true);
    if (config->GetKind_Gradient_Method_Recon() == LEAST_SQUARES)
      SetSolution_Gradient_LS(geometry, config, true);
    if (config->GetKind_Gradient_Method_Recon() == WEIGHTED_LEAST_SQUARES)
      SetSolution_Gradient_LS(geometry, config, true);
  }

  if (config->GetKind_Gradient_Method() == GREEN_GAUSS)
    SetSolution_Gradient_GG(geometry, config);

  if (config->GetKind_Gradient_Method() == WEIGHTED_LEAST_SQUARES)
    SetSolution_Gradient_LS(geometry, config);

  if (limiter && muscl) SetSolution_Limiter(geometry, config);
}

void CFlameletSolver::Postprocessing(CGeometry *geometry, CSolver **solver_container,
                                    CConfig *config, unsigned short iMesh) {

/*--- your postprocessing goes here ---*/

}

void CFlameletSolver::SetInitialCondition(CGeometry **geometry,
                                          CSolver ***solver_container,
                                          CConfig *config,
                                          unsigned long ExtIter) {
  su2double *coords;
  bool Restart   = (config->GetRestart() || config->GetRestart_Flow());
  
  
  if ((!Restart) && ExtIter == 0) {
    if (rank == MASTER_NODE){
      cout << "Initializing progress variable and temperature (initial condition)." << endl;
    } 

    su2double *scalar_init  = new su2double[nVar];
    su2double *flame_offset = config->GetFlameOffset();
    su2double *flame_normal = config->GetFlameNormal();

    su2double prog_burnt;
    su2double prog_unburnt    = 0.0;
    su2double flame_thickness = config->GetFlameThickness();
    su2double burnt_thickness = config->GetBurntThickness();
    su2double flamenorm       = sqrt( flame_normal[0]*flame_normal[0]
                                     +flame_normal[1]*flame_normal[1]
                                     +flame_normal[2]*flame_normal[2]);

   
    su2double temp_inlet = config->GetInc_Temperature_Init(); // should do reverse lookup of enthalpy
    su2double prog_inlet = config->GetScalar_Init(I_PROG_VAR); 
    if (rank == MASTER_NODE){
      cout << "initial condition: T = "<<temp_inlet << endl; 
      cout << "initial condition: pv = "<<prog_inlet << endl; 
    }

    su2double enth_inlet;
    su2double point_loc;
    su2double dist;
    unsigned long n_not_iterated  = 0;
    unsigned long n_not_in_domain = 0;

    CFluidModel *fluid_model_local;

    vector<string>     look_up_tags;
    vector<su2double*> look_up_data;
    string name_enth = config->GetScalarName(I_ENTHALPY);
    string name_prog = config->GetScalarName(I_PROG_VAR);

    for (unsigned long i_mesh = 0; i_mesh <= config->GetnMGLevels(); i_mesh++) {

      fluid_model_local = solver_container[i_mesh][FLOW_SOL]->GetFluidModel();

      n_not_iterated         += fluid_model_local->GetEnthFromTemp(&enth_inlet, prog_inlet,temp_inlet);
      scalar_init[I_ENTHALPY] = enth_inlet;

      /*--- the burnt value of the progress variable is set to a value slightly below the maximum value ---*/

      prog_burnt = 0.95*fluid_model_local->GetLookUpTable()->GetTableLimitsProg().second;
      for (unsigned long i_point = 0; i_point < geometry[i_mesh]->GetnPoint(); i_point++) {
        
        for (unsigned long i_var = 0; i_var < nVar; i_var++)
          Solution[i_var] = 0.0;
        
                coords = geometry[i_mesh]->nodes->GetCoord(i_point);

        /* determine if our location is above or below the plane, assuming the normal 
           is pointing towards the burned region*/ 
        point_loc = flame_normal[0]*(coords[0]-flame_offset[0]) 
                  + flame_normal[1]*(coords[1]-flame_offset[1]) 
                  + flame_normal[2]*(coords[2]-flame_offset[2]);

        /* compute the exact distance from point to plane */          
        point_loc = point_loc/flamenorm;

        /* --- unburnt region upstream of the flame --- */
        if (point_loc <= 0){
          scalar_init[I_PROG_VAR] = prog_unburnt;

         /* --- flame zone --- */
        } else if ( (point_loc > 0) && (point_loc <= flame_thickness) ){
          scalar_init[I_PROG_VAR] = 0.5*(prog_unburnt + prog_burnt);

        /* --- burnt region --- */
        } else if ( (point_loc > flame_thickness) && (point_loc <= flame_thickness + burnt_thickness) ){
          scalar_init[I_PROG_VAR] = prog_burnt;

        /* --- unburnt region downstream of the flame --- */
        } else {
          scalar_init[I_PROG_VAR] = prog_unburnt;
        }

        n_not_in_domain        += fluid_model_local->GetLookUpTable()->LookUp_ProgEnth(look_up_tags, look_up_data, scalar_init[I_PROG_VAR], scalar_init[I_ENTHALPY],name_prog,name_enth);

        // skip progress variable and enthalpy
        // we can make an init based on the lookup table. 
        for(int i_scalar = 0; i_scalar < config->GetNScalars(); ++i_scalar){
          if ( (i_scalar != I_ENTHALPY) && (i_scalar != I_PROG_VAR) )
          scalar_init[i_scalar] = config->GetScalar_Init(i_scalar);
        }
        
        solver_container[i_mesh][SCALAR_SOL]->GetNodes()->SetSolution(i_point, scalar_init);

      }

      solver_container[i_mesh][SCALAR_SOL]->InitiateComms(geometry[i_mesh], config, SOLUTION);
      solver_container[i_mesh][SCALAR_SOL]->CompleteComms(geometry[i_mesh], config, SOLUTION);

      solver_container[i_mesh][FLOW_SOL]->InitiateComms(geometry[i_mesh], config, SOLUTION);
      solver_container[i_mesh][FLOW_SOL]->CompleteComms(geometry[i_mesh], config, SOLUTION);

      solver_container[i_mesh][FLOW_SOL]->Preprocessing( geometry[i_mesh], solver_container[i_mesh], config, i_mesh, NO_RK_ITER, RUNTIME_FLOW_SYS, false);
      
    }

  delete[] scalar_init;

    if (rank == MASTER_NODE && (n_not_in_domain > 0 || n_not_iterated > 0))
      cout << endl;
    
    if (rank == MASTER_NODE && n_not_in_domain > 0)
      cout << " !!! Initial condition: Number of points outside of table domain: " << n_not_in_domain << " !!!" << endl;

    if (rank == MASTER_NODE && n_not_iterated > 0)
      cout << " !!! Initial condition: Number of points in which enthalpy could not be iterated: " << n_not_iterated << " !!!" << endl;

    if (rank == MASTER_NODE && (n_not_in_domain > 0 || n_not_iterated > 0))
      cout << endl;
  }
}

void CFlameletSolver::SetPreconditioner(CGeometry *geometry, CSolver **solver_container,  CConfig *config) {
  
  unsigned short iVar;
  unsigned long iPoint, total_index;
  
  su2double  BetaInc2, Density, dRhodT, dRhodC, Temperature, Cp, Delta;
  
  bool variable_density = (config->GetKind_DensityModel() == VARIABLE);
  bool implicit         = (config->GetKind_TimeIntScheme_Flow() == EULER_IMPLICIT);
  
  for (iPoint = 0; iPoint < nPointDomain; iPoint++) {
    
    /*--- Access the primitive variables at this node. ---*/
    
    Density     = solver_container[FLOW_SOL]->GetNodes()->GetDensity(iPoint);
    BetaInc2    = solver_container[FLOW_SOL]->GetNodes()->GetBetaInc2(iPoint);
    Cp          = solver_container[FLOW_SOL]->GetNodes()->GetSpecificHeatCp(iPoint);
    Temperature = solver_container[FLOW_SOL]->GetNodes()->GetTemperature(iPoint);
    
    unsigned short nVar_Flow = solver_container[FLOW_SOL]->GetnVar();
    
    su2double SolP = solver_container[FLOW_SOL]->LinSysSol[iPoint*nVar_Flow+0];
    su2double SolT = solver_container[FLOW_SOL]->LinSysSol[iPoint*nVar_Flow+nDim+1];
    
    /*--- We need the derivative of the equation of state to build the
     preconditioning matrix. For now, the only option is the ideal gas
     law, but in the future, dRhodT should be in the fluid model. ---*/
    
    if (variable_density) {
      dRhodT = -Density/Temperature;
    } else {
      dRhodT = 0.0;
    }
    
    /*--- Passive scalars have no impact on the density. ---*/
    
    dRhodC = 0.0;
    
    /*--- Modify matrix diagonal with term including volume and time step. ---*/
    
    su2double Vol = geometry->nodes->GetVolume(iPoint);
    Delta = Vol / (config->GetCFLRedCoeff_Scalar()*
                   solver_container[FLOW_SOL]->GetNodes()->GetDelta_Time(iPoint));
    
    /*--- Calculating the inverse of the preconditioning matrix
     that multiplies the time derivative during time integration. ---*/
    
    if (implicit) {
      // nijso: do we need to wipe the entire jacobian for preconditioning?
      //for (int i_var = 0; i_var < nVar; i_var++) {
      //  for (int j_var = 0; j_var < nVar; j_var++) {
      //    Jacobian_i[i_var][j_var] = 0.0;
      //  }
      //}
      
      for (iVar = 0; iVar < nVar; iVar++) {
        
        total_index = iPoint*nVar+iVar;
        
        su2double scalar = nodes->GetSolution(iPoint,iVar);
        
        /*--- Compute the lag terms for the decoupled linear system from
         the mean flow equations and add to the residual for the scalar.
         In short, we are effectively making these terms explicit. ---*/
        
        su2double artcompc1 = SolP * scalar/(Density*BetaInc2);
        su2double artcompc2 = SolT * dRhodT * scalar/(Density);
        
        LinSysRes[total_index] += artcompc1 + artcompc2;
        
        /*--- Add the extra Jacobian term to the scalar system. ---*/
        
        su2double Jaccomp = scalar * dRhodC + Density; //This is Gamma
        su2double JacTerm = Jaccomp*Delta;
        
        Jacobian.AddVal2Diag(iPoint, JacTerm);
        
      }
      
    }
    
  }

}

void CFlameletSolver::Source_Residual(CGeometry *geometry,
                                      CSolver **solver_container,
                                      CNumerics **numerics_container,
                                      CConfig *config,
                                      unsigned short iMesh) {

  bool implicit            = (config->GetKind_TimeIntScheme_Flow() == EULER_IMPLICIT);
  bool axisymmetric        = config->GetAxisymmetric();
  bool viscous             = config->GetViscous();
  unsigned short n_scalars = config->GetNScalars();
  unsigned short n_lookups = config->GetNLookups();

  su2double delta_enth_lut;
  su2double delta_temp_lut;
  su2double delta_source_prog_lut;
  su2double delta_source_energy_lut;

  CNumerics *second_numerics = numerics_container[SOURCE_SECOND_TERM + omp_get_thread_num()*MAX_TERMS];
  CNumerics *first_numerics  = numerics_container[SOURCE_FIRST_TERM  + omp_get_thread_num()*MAX_TERMS];

  CFluidModel *fluid_model_local = solver_container[FLOW_SOL]->GetFluidModel();
  
  su2double zero_sources[4] {0,0,0,0};
  
  SU2_OMP_FOR_DYN(omp_chunk_size)
  for (auto i_point = 0u; i_point < nPointDomain; i_point++) {

    /*--- Set primitive variables w/o reconstruction ---*/

    first_numerics->SetPrimitive(solver_container[FLOW_SOL]->GetNodes()->GetPrimitive(i_point), nullptr);

    /*--- Set scalar variables w/o reconstruction ---*/

    first_numerics->SetScalarVar(nodes->GetSolution(i_point), nullptr);

    first_numerics->SetDiffusionCoeff(nodes->GetDiffusivity(i_point), nodes->GetDiffusivity(i_point));

    /*--- Set volume of the dual cell. ---*/

    first_numerics->SetVolume(geometry->nodes->GetVolume(i_point));

    /*--- Update scalar sources in the fluidmodel ---*/

    fluid_model_local->SetScalarSources(nodes->GetSolution(i_point));

    /*--- Retrieve scalar sources from fluidmodel and update numerics class data. ---*/
    //first_numerics->SetSourcePV(fluid_model_local->GetScalarSources()[0]);
    first_numerics->SetScalarSources(fluid_model_local->GetScalarSources());
    //first_numerics->SetScalarSources(zero_sources);

    auto residual = first_numerics->ComputeResidual(config);

    /*--- Add Residual ---*/
    
    LinSysRes.SubtractBlock(i_point, residual);
    
    /*--- Implicit part ---*/
    
    if (implicit) Jacobian.SubtractBlock2Diag(i_point, residual.jacobian_i);
    
  }
  
  /*--- Axisymmetry source term for the scalar equation. ---*/
  
  if (axisymmetric) {
    
    /*--- Zero out Jacobian structure ---*/
    
    if (implicit) {
      for (auto i_var = 0u; i_var < nVar; i_var ++)
        for (auto j_var = 0u; j_var < nVar; j_var ++)
          Jacobian_i[i_var][j_var] = 0.0;
    }
    
    /*--- loop over points ---*/
    
    for (auto i_point = 0u; i_point < nPointDomain; i_point++) {
      
      /*--- Primitive variables w/o reconstruction ---*/
      
      second_numerics->SetPrimitive(solver_container[FLOW_SOL]->GetNodes()->GetPrimitive(i_point), nullptr);
      
      /*--- Scalar variables w/o reconstruction ---*/
      
      second_numerics->SetScalarVar(nodes->GetSolution(i_point), nullptr);
      
      /*--- Mass diffusivity coefficients. ---*/
      
      second_numerics->SetDiffusionCoeff(nodes->GetDiffusivity(i_point), nullptr);
      
      /*--- Set control volume ---*/
      
      second_numerics->SetVolume(geometry->nodes->GetVolume(i_point));
      
      /*--- Set y coordinate ---*/
      
      second_numerics->SetCoord(geometry->nodes->GetCoord(i_point), nullptr);
      
      /*--- If viscous, we need gradients for extra terms. ---*/
      
      if (viscous) {
        
        /*--- Gradient of the scalar variables ---*/
        
        second_numerics->SetScalarVarGradient(nodes->GetGradient(i_point), nullptr);
        
      }
      
      /*--- Compute Source term Residual ---*/
      
      second_numerics->ComputeResidual(Residual, Jacobian_i, config);
      
      /*--- Add Residual ---*/
      
      LinSysRes.AddBlock(i_point, Residual);
      
      /*--- Implicit part ---*/
      
      if (implicit) Jacobian.AddBlock(i_point, i_point, Jacobian_i);
      
    }
  }
}

void CFlameletSolver::BC_Inlet(CGeometry *geometry,
                               CSolver **solver_container,
                               CNumerics *conv_numerics,
                               CNumerics *visc_numerics,
                               CConfig *config,
                               unsigned short val_marker) {
  
  unsigned short iDim, iVar;
  unsigned long iVertex, iPoint, total_index, not_used;
  su2double *Coords;
  su2double enth_inlet;

  bool        grid_movement = config->GetGrid_Movement      (          );
  string      Marker_Tag    = config->GetMarker_All_TagBound(val_marker);
  su2double   temp_inlet    = config->GetInlet_Ttotal       (Marker_Tag);
  su2double  *inlet_scalar  = config->GetInlet_ScalarVal    (Marker_Tag);
 
  CFluidModel  *fluid_model_local = solver_container[FLOW_SOL]->GetFluidModel();

  not_used                 = fluid_model_local->GetEnthFromTemp(&enth_inlet, inlet_scalar[I_PROG_VAR], temp_inlet);
  inlet_scalar[I_ENTHALPY] = enth_inlet;

  /*--- Loop over all the vertices on this boundary marker ---*/

  for (iVertex = 0; iVertex < geometry->nVertex[val_marker]; iVertex++) {

   /* Dirichlet boundary condition at the inlet for scalars */

    iPoint = geometry->vertex[val_marker][iVertex]->GetNode();
    
    /*--- Check if the node belongs to the domain (i.e., not a halo node) ---*/

       if (geometry->nodes->GetDomain(iPoint)) {
    
        nodes->SetSolution_Old(iPoint, inlet_scalar);

        LinSysRes.SetBlock_Zero(iPoint);

        for (iVar = 0; iVar < nVar; iVar++) {
          nodes->SetVal_ResTruncError_Zero(iPoint, iVar);
        }

        /*--- Includes 1 in the diagonal ---*/
         for (iVar = 0; iVar < nVar; iVar++) {
          total_index = iPoint*nVar+iVar;
          Jacobian.DeleteValsRowi(total_index);
        }      
    }
  }

}

void CFlameletSolver::BC_Outlet(CGeometry *geometry, CSolver **solver_container, CNumerics *conv_numerics,
                               CNumerics *visc_numerics, CConfig *config, unsigned short val_marker) {

  const bool implicit = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);

  /*--- Loop over all the vertices on this boundary marker ---*/

  SU2_OMP_FOR_STAT(OMP_MIN_SIZE)
  for (auto iVertex = 0u; iVertex < geometry->nVertex[val_marker]; iVertex++) {

    /* strong zero flux Neumann boundary condition at the outlet */
    const auto iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    /*--- Check if the node belongs to the domain (i.e., not a halo node) ---*/

    if (geometry->nodes->GetDomain(iPoint)) {
      
        /*--- Allocate the value at the outlet ---*/
        auto Point_Normal = geometry->vertex[val_marker][iVertex]->GetNormal_Neighbor(); 
          
        for (auto iVar = 0u; iVar < nVar; iVar++)
          Solution[iVar] = nodes->GetSolution(Point_Normal, iVar);
        nodes->SetSolution_Old(iPoint, Solution);
    
        LinSysRes.SetBlock_Zero(iPoint);

        for (auto iVar = 0u; iVar < nVar; iVar++){
          nodes->SetVal_ResTruncError_Zero(iPoint, iVar);
        }

        /*--- Includes 1 in the diagonal ---*/
        for (auto iVar = 0u; iVar < nVar; iVar++) {
          auto total_index = iPoint*nVar+iVar;
          Jacobian.DeleteValsRowi(total_index);
        }
    }
  }

}

void CFlameletSolver::BC_Isothermal_Wall(CGeometry *geometry,
                                         CSolver **solver_container,
                                         CNumerics *conv_numerics,
                                         CNumerics *visc_numerics,
                                         CConfig *config,
                                         unsigned short val_marker) {

  unsigned short iVar, jVar, iDim;
  unsigned long iVertex, iPoint, total_index;

  bool implicit                   = config->GetKind_TimeIntScheme_Flow() == EULER_IMPLICIT;
  string Marker_Tag               = config->GetMarker_All_TagBound(val_marker);
  su2double temp_wall             = config->GetIsothermal_Temperature(Marker_Tag);
  CFluidModel *fluid_model_local  = solver_container[FLOW_SOL]->GetFluidModel();    
  su2double enth_wall, prog_wall;
  unsigned long n_not_iterated    = 0;

  bool use_weak_bc                = config->GetUseWeakScalarBC();
  su2double *normal;
  su2double *coord_i, *coord_j;
  unsigned long point_normal;
  su2double area;
  su2double dist_ij;
  su2double dEnth_dn;
  su2double dT_dn;
  su2double mass_diffusivity;

  /*--- Loop over all the vertices on this boundary marker ---*/
  
  for (iVertex = 0; iVertex < geometry->nVertex[val_marker]; iVertex++) {
    
    iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    /*--- Check if the node belongs to the domain (i.e., not a halo node) ---*/

    if (geometry->nodes->GetDomain(iPoint)) {
      /*--- Set enthalpy on the wall ---*/

      prog_wall = solver_container[SCALAR_SOL]->GetNodes()->GetSolution(iPoint)[I_PROG_VAR];
      n_not_iterated += fluid_model_local->GetEnthFromTemp(&enth_wall, prog_wall, temp_wall);

      /*--- Impose the value of the enthalpy as a strong boundary
      condition (Dirichlet) and remove any
      contribution to the residual at this node. ---*/

      nodes->SetSolution(iPoint, I_ENTHALPY, enth_wall);
      nodes->SetSolution_Old(iPoint, I_ENTHALPY, enth_wall);

      //LinSysRes(iPoint, I_ENTHALPY) = 0.0;
      LinSysRes.SetBlock_Zero(iPoint, I_ENTHALPY);

      nodes->SetVal_ResTruncError_Zero(iPoint, I_ENTHALPY);

      if (implicit) {
        total_index = iPoint * nVar + I_ENTHALPY;

        Jacobian.DeleteValsRowi(total_index);
      }
    }
  }
  if (rank == MASTER_NODE && n_not_iterated > 0){
    cout << " !!! Isothermal wall bc ("  << Marker_Tag << "): Number of points in which enthalpy could not be iterated: " << n_not_iterated << " !!!" << endl;
  }

}


