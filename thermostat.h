// This file is part of the ESPResSo distribution (http://www.espresso.mpg.de).
// It is therefore subject to the ESPResSo license agreement which you accepted upon receiving the distribution
// and by which you are legally bound while utilizing this file in any form or way.
// There is NO WARRANTY, not even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// You should have received a copy of that license along with this program;
// if not, refer to http://www.espresso.mpg.de/license.html where its current version can be found, or
// write to Max-Planck-Institute for Polymer Research, Theory Group, PO Box 3148, 55021 Mainz, Germany.
// Copyright (c) 2002-2004; all rights reserved unless otherwise stated.
#ifndef THERMOSTAT_H
#define THERMOSTAT_H
/** \file thermostat.h 

    Contains all thermostats.
    
    <ul>
    <li> For NVT Simulations one can chose either a Langevin thermostat or
    a DPD (Dissipative Particle Dynamics) thermostat.
    \verbatim thermostat langevin <temperature> <gamma> \endverbatim
    \verbatim thermostat dpd <temperature> <gamma> <r_cut> \endverbatim

    <li> For NPT Simulations one can use a thermostat for the box (Based on
    an Anderson thermostat) and a Langevin thermostat.
    \verbatim thermostat langevin <temperature> <gamma> \endverbatim
    \verbatim thermostat npt_isotropic <temperature> <gamma0> <gammaV> \endverbatim

    <li> \verbatim thermostat off \endverbatim
    Turns off all thermostats and sets all thermostat variables to zero.

    <li> \verbatim thermostat \endverbatim
    Returns the current thermostat status.
    </ul>

    Further important  remarks:
    <ul>
    <li> If ROTATION is compiled in, the rotational degrees of freedom
    are coupled to a Langevin thermostat as well if you use a Langevin
    thermostat.

    <li> All thermostats use the same temperature (So far we did not
    see any physical sense to change that!).
    </ul>

    Thermostat description:

    <ul>

    <li> LANGEVIN THERMOSTAT:

    Consists of a friction and noise term coupled via the fluctuation
    dissipation theorem. The Friction term is a function of the
    particles velocity.

    <li> DPD THERMOSTT (Dissipative Particle Dynamics):

    Consists of a friction and noise term coupled via the fluctuation
    dissipation theorem. The Friction term is a function of the
    relative velocity of particle pairs. 

    <li> NPT ISOTROPIC THERMOSTAT:
    
    INSERT COMMENT

    </ul>

    <b>Responsible:</b>
    <a href="mailto:limbach@mpip-mainz.mpg.de">Hanjo</a>

*/
#include <tcl.h>
#include <math.h>
#include "particle_data.h"
#include "parser.h"
#include "random.h"
#include "global.h"
#include "particle_data.h"
#include "communication.h"
#include "integrate.h"
#include "cells.h"
#include "debug.h"
#include "pressure.h"

/** \name Thermostat switches*/
/************************************************************/
/*@{*/

#define THERMO_OFF      0
#define THERMO_LANGEVIN 1
#define THERMO_DPD      2
#define THERMO_NPT_ISO  4

/*@}*/

/************************************************
 * exported variables
 ************************************************/

/** Switch determining which thermostat to use. This is a or'd value
    of the different possible thermostats (defines: \ref THERMO_OFF,
    \ref THERMO_LANGEVIN, \ref THERMO_DPD \ref THERMO_NPT_ISO). If it
    is zero all thermostats are switched off and the temperature is
    set to zero. You may combine different thermostats at your own
    risk by turning them on one by one. Note that there is only one
    temperature for all thermostats so far. */
extern int thermo_switch;

/** Temperature. */
extern double temperature;

/** Langevin Friction coefficient gamma. */
extern double langevin_gamma;
/** langevin Friction coefficient gamma for rotation */
extern double langevin_gamma_rotation;

/** DPD Friction coefficient gamma. */
extern double dpd_gamma;
/** DPD thermostat cutoff */
extern double dpd_r_cut;
/** inverse off DPD thermostat cutoff */
extern double dpd_r_cut_inv;


/** Friction coefficient for nptiso-thermostat's inline-functions \ref friction_therm0_nptiso */
extern double nptiso_gamma0;
/** Friction coefficient for nptiso-thermostat's inline-functions \ref friction_thermV_nptiso */
extern double nptiso_gammav;
#ifdef NPT
/** Prefactors for nptiso-thermostat's inline-functions \ref friction_therm0_nptiso */
extern double nptiso_pref1;
extern double nptiso_pref2;
/** Prefactors for nptiso-thermostat's inline-functions \ref friction_thermV_nptiso */
extern double nptiso_pref3;
extern double nptiso_pref4;
#endif

#ifdef DPD
/** DPD thermostat: prefactor friction force. */
extern double dpd_pref1;
/** DPD thermostat: prefactor random force */
extern double dpd_pref2;
#endif

/************************************************
 * functions
 ************************************************/

/** Implementation of the tcl command \ref tcl_thermostat. This function
    allows to change the used thermostat and to set its parameters.
 */
int thermostat(ClientData data, Tcl_Interp *interp, int argc, char **argv);

/** initialize constants of the thermostat on
    start of integration */
void thermo_init();

/** overwrite the forces of a particle with
    the friction term, i.e. \f$ F_i= -\gamma v_i + \xi_i\f$.
*/
void friction_thermo_langevin(Particle *p);

/** very nasty: if we recalculate force when leaving/reentering the integrator,
    a(t) and a((t-dt)+dt) are NOT equal in the vv algorithm. The random
    numbers are drawn twice, resulting in a different variance of the random force.
    This is corrected by additional heat when restarting the integrator here.
    Currently only works for the Langevin thermostat, although probably also others
    are affected.
*/
void thermo_heat_up();

/** pendant to \ref thermo_heat_up */
void thermo_cool_down();

#ifdef NPT
/** add velocity-dependend noise and friction for NpT-sims to the particle's velocity 
    @param dt_vj  j-component of the velocity scaled by time_step dt 
    @return       j-component of the noise added to the velocity, also scaled by dt (contained in prefactors) */
MDINLINE double friction_therm0_nptiso(double dt_vj) {
  if(thermo_switch & THERMO_NPT_ISO)   
    return ( nptiso_pref1*dt_vj + nptiso_pref2*(d_random()-0.5) );
  else
    return 0.0;
}

/** add p_diff-dependend noise and friction for NpT-sims to \ref nptiso_struct::p_diff */
MDINLINE double friction_thermV_nptiso(double p_diff) {
  if(thermo_switch & THERMO_NPT_ISO)   
    return ( nptiso_pref3*p_diff + nptiso_pref4*(d_random()-0.5) );
  else
    return 0.0;
}
#endif

/** set the particle torques to the friction term, i.e. \f$\tau_i=-\gamma w_i + \xi_i\f$.
The same friction coefficient \f$\gamma\f$ is used as that for translation.
*/
void friction_thermo_langevin_rotation(Particle *p);

/** Callback for setting \ref #temperature */
int temp_callback(Tcl_Interp *interp, void *_data);
/** Callback for setting \ref langevin_gamma */
int langevin_gamma_callback(Tcl_Interp *interp, void *_data);

#ifdef DPD
/** Calculate Random Force and Friction Force acting between particle
    p1 and p2 and add them to their forces. */
MDINLINE void add_dpd_thermo_pair_force(Particle *p1, Particle *p2, double d[3], double dist)
{
  int j;
  /* velocity difference between p1 and p2 */
  double vel12_dot_d12=0.0;
  /* inverse distance */
  double dist_inv;
  /* temporary storage variable */
  double tmp;
  /* weighting functions for friction and random force */
  double omega, friction, noise;
  if(dist < dpd_r_cut) {
    /* random force prefactor */
    dist_inv = 1.0/dist;
    omega    = dist_inv - dpd_r_cut_inv;
    /* friction force prefactor */
    for(j=0; j<3; j++)  vel12_dot_d12 += (p1->m.v[j] - p2->m.v[j]) * d[j];
    friction = dpd_pref1 * SQR(omega) * vel12_dot_d12;
    /* random force prefactor */
    noise    = dpd_pref2 * omega      * (d_random()-0.5);

    for(j=0; j<3; j++) {
      p1->f.f[j] += ( tmp = (noise - friction)*d[j] );
      p2->f.f[j] -= tmp;
    }
  }
}
#endif

#endif
