/********************************************************************************
 * HOMMEXX 1.0: Copyright of Sandia Corporation
 * This software is released under the BSD license
 * See the file 'COPYRIGHT' in the HOMMEXX/src/share/cxx directory
 *******************************************************************************/

#include "CaarFunctor.hpp"
#include "Context.hpp"
#include "Diagnostics.hpp"
#include "Elements.hpp"
#include "ErrorDefs.hpp"
#include "EulerStepFunctor.hpp"
#include "HommexxEnums.hpp"
#include "HybridVCoord.hpp"
#include "HyperviscosityFunctor.hpp"
#include "ReferenceElement.hpp"
#include "SimulationParams.hpp"
#include "SphereOperators.hpp"
#include "TimeLevel.hpp"
#include "Tracers.hpp"
#include "VerticalRemapManager.hpp"
#include "mpi/BoundaryExchange.hpp"
#include "mpi/BuffersManager.hpp"

#include "utilities/SyncUtils.hpp"

#include "profiling.hpp"

namespace Homme
{

extern "C"
{

void init_simulation_params_c (const int& remap_alg, const int& limiter_option, const int& rsplit, const int& qsplit,
                               const int& time_step_type, const int& qsize, const int& state_frequency,
                               const Real& nu, const Real& nu_p, const Real& nu_q, const Real& nu_s, const Real& nu_div, const Real& nu_top,
                               const int& hypervis_order, const int& hypervis_subcycle, const double& hypervis_scaling,
                               const int& ftype, const bool& prescribed_wind, const bool& moisture, const bool& disable_diagnostics,
                               const bool& use_cpstar, const bool& use_semi_lagrangian_transport)
{
  // Check that the simulation options are supported. This helps us in the future, since we
  // are currently 'assuming' some option have/not have certain values. As we support for more
  // options in the C++ build, we will remove some checks
  Errors::check_option("init_simulation_params_c","vert_remap_q_alg",remap_alg,{1,3});
  Errors::check_option("init_simulation_params_c","prescribed_wind",prescribed_wind,{false});
  Errors::check_option("init_simulation_params_c","hypervis_order",hypervis_order,{2});
  Errors::check_option("init_simulation_params_c","use_semi_lagrangian_transport",use_semi_lagrangian_transport,{false});
  Errors::check_option("init_simulation_params_c","time_step_type",time_step_type,{5});
  Errors::check_option("init_simulation_params_c","qsize",qsize,0,Errors::ComparisonOp::GE);
  Errors::check_option("init_simulation_params_c","qsize",qsize,QSIZE_D,Errors::ComparisonOp::LE);
  Errors::check_option("init_simulation_params_c","limiter_option",limiter_option,{8,9});
  Errors::check_option("init_simulation_params_c","ftype",ftype, {-1, 0, 2});
  Errors::check_option("init_simulation_params_c","nu_p",nu_p,0.0,Errors::ComparisonOp::GT);
  Errors::check_option("init_simulation_params_c","nu",nu,0.0,Errors::ComparisonOp::GT);
  Errors::check_option("init_simulation_params_c","nu_div",nu_div,0.0,Errors::ComparisonOp::GT);

  // Get the simulation params struct
  SimulationParams& params = Context::singleton().get<SimulationParams>();

  if (remap_alg==1) {
    params.remap_alg = RemapAlg::PPM_MIRRORED;
  } else if (remap_alg == 2) {
    params.remap_alg = RemapAlg::PPM_FIXED_PARABOLA;
  } else if (remap_alg == 3) {
    params.remap_alg = RemapAlg::PPM_FIXED_MEANS;
  }

  params.limiter_option                = limiter_option;
  params.rsplit                        = rsplit;
  params.qsplit                        = qsplit;
  params.time_step_type                = time_step_type;
  params.prescribed_wind               = prescribed_wind;
  params.state_frequency               = state_frequency;
  params.qsize                         = qsize;
  params.nu                            = nu;
  params.nu_p                          = nu_p;
  params.nu_q                          = nu_q;
  params.nu_s                          = nu_s;
  params.nu_div                        = nu_div;
  params.nu_top                        = nu_top;
  params.hypervis_order                = hypervis_order;
  params.hypervis_subcycle             = hypervis_subcycle;
  params.hypervis_scaling              = hypervis_scaling;
  params.disable_diagnostics           = disable_diagnostics;
  params.moisture                      = (moisture ? MoistDry::MOIST : MoistDry::DRY);
  params.use_cpstar                    = use_cpstar;
  params.use_semi_lagrangian_transport = use_semi_lagrangian_transport;

  //set nu_ratios values
  if (params.nu != params.nu_div) {
    Real ratio = params.nu_div / params.nu;
    if (params.hypervis_scaling != 0.0) {
      params.nu_ratio1 = ratio * ratio;
      params.nu_ratio2 = 1.0;
    }else{
      params.nu_ratio1 = ratio;
      params.nu_ratio2 = ratio;
    }
  }else{
    params.nu_ratio1 = 1.0;
    params.nu_ratio2 = 1.0;
  }

  if (ftype == -1) {
    params.ftype = ForcingAlg::FORCING_OFF;
  } else if (ftype == 0) {
    params.ftype = ForcingAlg::FORCING_DEBUG;
  } else if (ftype == 2) {
    params.ftype = ForcingAlg::FORCING_2;
  }

  // TODO Parse a fortran string and set this properly. For now, our code does
  // not depend on this except to throw an error in apply_test_forcing.
  params.test_case = TestCase::JW_BAROCLINIC;

  // Now this structure can be used safely
  params.params_set = true;

}

void init_hvcoord_c (const Real& ps0, CRCPtr& hybrid_am_ptr, CRCPtr& hybrid_ai_ptr,
                                      CRCPtr& hybrid_bm_ptr, CRCPtr& hybrid_bi_ptr)
{
  HybridVCoord& hvcoord = Context::singleton().get<HybridVCoord>();
  hvcoord.init(ps0,hybrid_am_ptr,hybrid_ai_ptr,hybrid_bm_ptr,hybrid_bi_ptr);
}

void cxx_push_results_to_f90(F90Ptr &elem_state_v_ptr, F90Ptr &elem_state_temp_ptr,
                             F90Ptr &elem_state_dp3d_ptr, F90Ptr &elem_state_Qdp_ptr,
                             F90Ptr &elem_Q_ptr, F90Ptr &elem_state_ps_v_ptr,
                             F90Ptr &elem_derived_omega_p_ptr) {
  Elements &elements = Context::singleton().get<Elements>();
  elements.m_state.push_to_f90_pointers(elem_state_v_ptr, elem_state_temp_ptr, elem_state_dp3d_ptr);

  Tracers &tracers = Context::singleton().get<Tracers>();
  tracers.push_qdp(elem_state_Qdp_ptr);

  // F90 ptrs to arrays (np,np,num_time_levels,nelemd) can be stuffed directly
  // in an unmanaged view
  // with scalar Real*[NUM_TIME_LEVELS][NP][NP] (with runtime dimension nelemd)
  HostViewUnmanaged<Real * [NUM_TIME_LEVELS][NP][NP]> ps_v_f90(
      elem_state_ps_v_ptr, elements.num_elems());

  auto ps_v_host = Kokkos::create_mirror_view(elements.m_state.m_ps_v);

  Kokkos::deep_copy(ps_v_host, elements.m_state.m_ps_v);
  Kokkos::deep_copy(ps_v_f90, ps_v_host);

  sync_to_host(elements.m_derived.m_omega_p,
               HostViewUnmanaged<Real * [NUM_PHYSICAL_LEV][NP][NP]>(
                   elem_derived_omega_p_ptr, elements.num_elems()));
  sync_to_host(tracers.Q,
               HostViewUnmanaged<Real * [QSIZE_D][NUM_PHYSICAL_LEV][NP][NP]>(
                   elem_Q_ptr, elements.num_elems()));
}

// Probably not needed
void cxx_push_forcing_to_f90(F90Ptr elem_derived_FM, F90Ptr elem_derived_FT,
                             F90Ptr elem_derived_FQ) {
  Elements &elements = Context::singleton().get<Elements>();
  Tracers &tracers = Context::singleton().get<Tracers>();

  HostViewUnmanaged<Real * [NUM_PHYSICAL_LEV][2][NP][NP]> fm_f90(
      elem_derived_FM, elements.num_elems());
  sync_to_host(elements.m_derived.m_fm, fm_f90);
  HostViewUnmanaged<Real * [NUM_PHYSICAL_LEV][NP][NP]> ft_f90(
      elem_derived_FT, elements.num_elems());
  sync_to_host(elements.m_derived.m_ft, ft_f90);

  const SimulationParams &params = Context::singleton().get<SimulationParams>();
  if (params.ftype == ForcingAlg::FORCING_DEBUG) {
    if (tracers.fq.data() == nullptr) {
      tracers.fq = decltype(tracers.fq)("fq", elements.num_elems());
    }
    HostViewUnmanaged<Real * [QSIZE_D][NUM_PHYSICAL_LEV][NP][NP]> fq_f90(
        elem_derived_FQ, elements.num_elems());
    sync_to_host(tracers.fq, fq_f90);
  }
}

void f90_push_forcing_to_cxx(F90Ptr elem_derived_FM, F90Ptr elem_derived_FT,
                             F90Ptr elem_derived_FQ,
                             F90Ptr elem_state_Qdp_ptr) {
  Elements &elements = Context::singleton().get<Elements>();

  HostViewUnmanaged<Real * [NUM_PHYSICAL_LEV][2][NP][NP]> fm_f90(
      elem_derived_FM, elements.num_elems());
  sync_to_device(fm_f90, elements.m_derived.m_fm);

  HostViewUnmanaged<Real * [NUM_PHYSICAL_LEV][NP][NP]> ft_f90(
      elem_derived_FT, elements.num_elems());
  sync_to_device(ft_f90, elements.m_derived.m_ft);

  const SimulationParams &params = Context::singleton().get<SimulationParams>();
  Tracers &tracers = Context::singleton().get<Tracers>();
  if (params.ftype == ForcingAlg::FORCING_DEBUG) {
    if (tracers.fq.data() == nullptr) {
      tracers.fq = decltype(tracers.fq)("fq", elements.num_elems());
    }
    HostViewUnmanaged<Real * [QSIZE_D][NUM_PHYSICAL_LEV][NP][NP]> fq_f90(
        elem_derived_FQ, elements.num_elems());
    sync_to_device(fq_f90, tracers.fq);
  }

  tracers.push_qdp(elem_state_Qdp_ptr);
}

void init_reference_element_c (CF90Ptr& deriv, CF90Ptr& mass)
{
  ReferenceElement& ref_FE = Context::singleton().get<ReferenceElement> ();
  ref_FE.init(deriv,mass);
}

void init_time_level_c (const int& nm1, const int& n0, const int& np1,
                        const int& nstep, const int& nstep0)
{
  TimeLevel& tl = Context::singleton().get<TimeLevel>();
  tl.nm1    = nm1-1;
  tl.n0     = n0-1;
  tl.np1    = np1-1;
  tl.nstep  = nstep;
  tl.nstep0 = nstep0;
}

void init_elements_c (const int& num_elems)
{
  Elements& r = Context::singleton().get<Elements> ();
  const SimulationParams& params = Context::singleton().get<SimulationParams>();

  const bool consthv = (params.hypervis_scaling==0.0);
  r.init (num_elems, consthv);

  // Init also the tracers structure
  Tracers& t = Context::singleton().get<Tracers> ();
  t.init(num_elems,params.qsize);

  // In the context, we register also ElementsGeometry, ElementsDerivedState, and ElementsBuffers,
  // making sure they store the same views as in the subobjects of Elements.
  // This allows objects that need only a piece of Elements, to grab it from the Context,
  // while still knowing that what they grab contains the same views as the object stored in the
  // Elements inside the Context
  ElementsGeometry& geometry = Context::singleton().get<ElementsGeometry>();
  geometry = r.m_geometry;
  ElementsDerivedState& derived = Context::singleton().get<ElementsDerivedState>();
  derived = r.m_derived;
  ElementsBuffers& buffers = Context::singleton().get<ElementsBuffers>();
  buffers = r.m_buffers;
}

void init_functors_c ()
{
  // We init all the functors in the Context, so that every call to
  // Context::singleton().get<[FunctorName]>()
  // will return a functor already initialized.
  // This avoids the risk of having a class doing
  //   FunctorName f = Context::singleton().get<FunctorName>();
  //   f.init(some_args);
  // and then, somewhere else, we find
  //   FunctorName f = Context::singleton().get<FunctorName>(some_args);
  // The problem is that the first call created an uninitialized functor,
  // *copied it*, and initialized the copy. The second call to get,
  // sees that there is *already* an object of type FunctorName in the
  // Context, and therefore does *not* create a FunctorName object.
  // A solution would be to make the second call could as the first one (with an
  // init call right after). However, we can also initialize all functors here
  // once and for all, so that the user does not have to initialize them when
  // calling them.

  auto& elems   = Context::singleton().get<Elements>();
  auto& tracers = Context::singleton().get<Tracers>();
  auto& ref_FE  = Context::singleton().get<ReferenceElement>();
  auto& hvcoord = Context::singleton().get<HybridVCoord>();
  auto& params  = Context::singleton().get<SimulationParams>();

  // Check that the above structures have been inited
  Errors::runtime_check(elems.inited(),    "Error! You must initialize the Elements structure before initializing the functors.\n", -1);
  Errors::runtime_check(tracers.inited(),  "Error! You must initialize the Tracers structure before initializing the functors.\n", -1);
  Errors::runtime_check(ref_FE.inited(),   "Error! You must initialize the ReferenceElement structure before initializing the functors.\n", -1);
  Errors::runtime_check(hvcoord.m_inited,  "Error! You must initialize the HybridVCoord structure before initializing the functors.\n", -1);
  Errors::runtime_check(params.params_set, "Error! You must initialize the SimulationParams structure before initializing the functors.\n", -1);

  // First, sphere operators
  auto& sph_op = Context::singleton().get<SphereOperators>(elems,ref_FE);
  auto& caar   = Context::singleton().get<CaarFunctor>(elems,tracers,ref_FE,hvcoord,sph_op,params.rsplit);
  auto& esf    = Context::singleton().get<EulerStepFunctor>();
  auto& hvf    = Context::singleton().get<HyperviscosityFunctor>();
  auto& vrm    = Context::singleton().get<VerticalRemapManager>();

  // Silence compiler warnings
  (void) caar;
  (void) esf;
  (void) hvf;
  (void) vrm;
}

void init_elements_2d_c (const int& ie, CF90Ptr& D, CF90Ptr& Dinv, CF90Ptr& fcor,
                         CF90Ptr& spheremp, CF90Ptr& rspheremp,
                         CF90Ptr& metdet, CF90Ptr& metinv, CF90Ptr& phis,
                         CF90Ptr &tensorvisc, CF90Ptr &vec_sph2cart)
{
  Elements& e = Context::singleton().get<Elements> ();
  const SimulationParams& params = Context::singleton().get<SimulationParams>();

  const bool consthv = (params.hypervis_scaling==0.0);
  e.m_geometry.init(ie,D,Dinv,fcor,spheremp,rspheremp,metdet,metinv,phis,tensorvisc,vec_sph2cart,consthv);
}

void init_elements_states_c (CF90Ptr& elem_state_v_ptr,   CF90Ptr& elem_state_temp_ptr, CF90Ptr& elem_state_dp3d_ptr,
                             CF90Ptr& elem_state_Qdp_ptr, CF90Ptr& elem_state_ps_v_ptr)
{
  Elements& elements = Context::singleton().get<Elements> ();
  elements.m_state.pull_from_f90_pointers(elem_state_v_ptr,elem_state_temp_ptr,elem_state_dp3d_ptr,elem_state_ps_v_ptr);
  Tracers &tracers = Context::singleton().get<Tracers>();
  tracers.pull_qdp(elem_state_Qdp_ptr);

}

void init_diagnostics_c (F90Ptr& elem_state_q_ptr, F90Ptr& elem_accum_qvar_ptr,  F90Ptr& elem_accum_qmass_ptr,
                         F90Ptr& elem_accum_q1mass_ptr, F90Ptr& elem_accum_iener_ptr, F90Ptr& elem_accum_iener_wet_ptr,
                         F90Ptr& elem_accum_kener_ptr, F90Ptr& elem_accum_pener_ptr)
{
  Elements& elements = Context::singleton().get<Elements> ();
  Diagnostics& diagnostics = Context::singleton().get<Diagnostics> ();

  diagnostics.init(elements.num_elems(), elem_state_q_ptr, elem_accum_qvar_ptr, elem_accum_qmass_ptr, elem_accum_q1mass_ptr,
                   elem_accum_iener_ptr, elem_accum_iener_wet_ptr, elem_accum_kener_ptr, elem_accum_pener_ptr);
}

void init_boundary_exchanges_c ()
{
  SimulationParams& params = Context::singleton().get<SimulationParams>();

  // Euler BEs
  auto& esf = Context::singleton().get<EulerStepFunctor>();
  esf.reset(params);
  esf.init_boundary_exchanges();

  // RK stages BE's
  auto& cf = Context::singleton().get<CaarFunctor>();
  auto connectivity = Context::singleton().get_ptr<Connectivity>();
  cf.init_boundary_exchanges(Context::singleton().get<BuffersManagerMap>(connectivity)[MPI_EXCHANGE]);

  // HyperviscosityFunctor's BE's
  auto& hvf = Context::singleton().get<HyperviscosityFunctor>();
  hvf.init_boundary_exchanges();
}

} // extern "C"

} // namespace Homme