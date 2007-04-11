/* $Id$
 *
 * This file is part of the ESPResSo distribution (http://www.espresso.mpg.de).
 * It is therefore subject to the ESPResSo license agreement which you
 * accepted upon receiving the distribution and by which you are
 * legally bound while utilizing this file in any form or way.
 * There is NO WARRANTY, not even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 * You should have received a copy of that license along with this
 * program; if not, refer to http://www.espresso.mpg.de/license.html
 * where its current version can be found, or write to
 * Max-Planck-Institute for Polymer Research, Theory Group, 
 * PO Box 3148, 55021 Mainz, Germany. 
 * Copyright (c) 2002-2007; all rights reserved unless otherwise stated.
 */

/** \file lb.c
 *
 * Lattice Boltzmann algorithm for hydrodynamic degrees of freedom.
 *
 * Includes fluctuating LB and coupling to MD particles via frictional 
 * momentum transfer.
 *
 */

#include <mpi.h>
#include <tcl.h>
#include <stdio.h>
#include <fftw3.h>
#include "utils.h"
#include "parser.h"
#include "communication.h"
#include "grid.h"
#include "domain_decomposition.h"
#include "interaction_data.h"
#include "thermostat.h"
#include "lattice.h"
#include "halo.h"
#include "lb-d3q19.h"
#include "lb-boundaries.h"
#include "lb.h"

#ifdef LB

/** Flag indicating momentum exchange between particles and fluid */
int transfer_momentum = 0;

/** Struct holding the Lattice Boltzmann parameters */
LB_Parameters lbpar = { 0.0, 0.0, 0.0, -1.0, -1.0, 0.0, { 0.0, 0.0, 0.0} };

/** The DnQm model to be used. */
LB_Model lbmodel = { 19, d3q19_lattice, d3q19_coefficients, d3q19_w, 1./3. };

/** The underlying lattice structure */
Lattice lblattice = { {0,0,0}, {0,0,0}, 0, 0, 0, 0, -1.0, -1.0, NULL, NULL };

/** Pointer to the fluid nodes
 * This variable is used for convenience instead of having to type lattice.fields everywhere */
LB_FluidNode *lbfluid=NULL;

/** Communicator for halo exchange between processors */
HaloCommunicator update_halo_comm = { 0, NULL };

/** The number of field variables on a local lattice site (counted in doubles). */
static int n_fields;

/** \name Derived parameters */
/*@{*/
/** Flag indicating whether fluctuations are present. */
static int fluct;
/** eigenvalue of the collision operator for relaxation of shear modes */
static double lblambda = -1;
/** eigenvalue of the collision operator for relaxation of bulk modes */
static double lblambda_bulk = -1;
/** amplitude of the fluctuations in the fluid stress tensor */
static double lb_fluct_pref = 0.0;
/** amplitude of the bulk fluctuations of the stress tensor */
static double lb_fluct_pref_bulk = 0.0;
/** amplitude of the fluctuations in the viscous coupling */
static double lb_coupl_pref;
/*@}*/

/** The number of velocities of the LB model.
 * This variable is used for convenience instead of having to type lbmodel.n_veloc everywhere. */
static int n_veloc;

/** Lattice spacing.
 * This variable is used for convenience instead of having to type lbpar.agrid everywhere. */
static double agrid;

/** Lattice Boltzmann time step
 * This variable is used for convenience instead of having to type lbpar.tau everywhere. */
static double tau;

/** measures the MD time since the last fluid update */
static double fluidstep=0.0;

/***********************************************************************/

#ifdef ADDITIONAL_CHECKS
static int compare_buffers(double *buf1, double *buf2, int size) {
  int ret;
  if (memcmp(buf1,buf2,size)) {
    char *errtxt;
    errtxt = runtime_error(128);
    ERROR_SPRINTF(errtxt,"{102 Halo buffers are not identical} ");
    ret = 1;
  } else {
    ret = 0;
  }
  return ret;
}

/** Checks consistency of the halo regions (ADDITIONAL_CHECKS)
 * This function can be used as an additional check. It test whether the 
 * halo regions have been exchanged correctly. */
static void lb_check_halo_regions() {

  int x,y,z, index, s_node, r_node, count=n_veloc;
  double *s_buffer, *r_buffer;
  MPI_Status status[2];

  r_buffer = malloc(count*sizeof(double));

  if (PERIODIC(0)) {
    for (z=0;z<lblattice.halo_grid[2];++z) {
      for (y=0;y<lblattice.halo_grid[1];++y) {

	index  = get_linear_index(0,y,z,lblattice.halo_grid);
	s_buffer = lbfluid[index].n;
	s_node = node_neighbors[1];
	r_node = node_neighbors[0];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(lblattice.grid[0],y,z,lblattice.halo_grid);
	  compare_buffers(lbfluid[index].n,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(lblattice.grid[0],y,z,lblattice.halo_grid);
	  if (compare_buffers(lbfluid[index].n,s_buffer,count*sizeof(double)))
	    fprintf(stderr,"buffers differ in dir=%d at index=%d y=%d z=%d\n",0,index,y,z);
	}

	index = get_linear_index(lblattice.grid[0]+1,y,z,lblattice.halo_grid); 
	s_buffer = lbfluid[index].n;
	s_node = node_neighbors[0];
	r_node = node_neighbors[1];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(1,y,z,lblattice.halo_grid);
	  compare_buffers(lbfluid[index].n,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(1,y,z,lblattice.halo_grid);
	  if (compare_buffers(lbfluid[index].n,s_buffer,count*sizeof(double)))
	    fprintf(stderr,"buffers differ in dir=%d at index=%d y=%d z=%d\n",0,index,y,z);	  
	}

      }      
    }
  }

  if (PERIODIC(1)) {
    for (z=0;z<lblattice.halo_grid[2];++z) {
      for (x=0;x<lblattice.halo_grid[0];++x) {

	index = get_linear_index(x,0,z,lblattice.halo_grid);
	s_buffer = lbfluid[index].n;
	s_node = node_neighbors[3];
	r_node = node_neighbors[2];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(x,lblattice.grid[1],z,lblattice.halo_grid);
	  compare_buffers(lbfluid[index].n,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(x,lblattice.grid[1],z,lblattice.halo_grid);
	  if (compare_buffers(lbfluid[index].n,s_buffer,count*sizeof(double)))
	    fprintf(stderr,"buffers differ in dir=%d at index=%d x=%d z=%d\n",1,index,x,z);
	}

      }
      for (x=0;x<lblattice.halo_grid[0];++x) {

	index = get_linear_index(x,lblattice.grid[1]+1,z,lblattice.halo_grid);
	s_buffer = lbfluid[index].n;
	s_node = node_neighbors[2];
	r_node = node_neighbors[3];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(x,1,z,lblattice.halo_grid);
	  compare_buffers(lbfluid[index].n,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(x,1,z,lblattice.halo_grid);
	  if (compare_buffers(lbfluid[index].n,s_buffer,count*sizeof(double)))
	    fprintf(stderr,"buffers differ in dir=%d at index=%d x=%d z=%d\n",1,index,x,z);
	}

      }
    }
  }

  if (PERIODIC(2)) {
    for (y=0;y<lblattice.halo_grid[1];++y) {
      for (x=0;x<lblattice.halo_grid[0];++x) {

	index = get_linear_index(x,y,0,lblattice.halo_grid);
	s_buffer = lbfluid[index].n;
	s_node = node_neighbors[5];
	r_node = node_neighbors[4];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(x,y,lblattice.grid[2],lblattice.halo_grid);
	  compare_buffers(lbfluid[index].n,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(x,y,lblattice.grid[2],lblattice.halo_grid);
	  if (compare_buffers(lbfluid[index].n,s_buffer,count*sizeof(double)))
	    fprintf(stderr,"buffers differ in dir=%d at index=%d x=%d y=%d z=%d\n",2,index,x,y,lblattice.grid[2]);  
	}

      }
    }
    for (y=0;y<lblattice.halo_grid[1];++y) {
      for (x=0;x<lblattice.halo_grid[0];++x) {

	index = get_linear_index(x,y,lblattice.grid[2]+1,lblattice.halo_grid);
	s_buffer = lbfluid[index].n;
	s_node = node_neighbors[4];
	r_node = node_neighbors[5];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(x,y,1,lblattice.halo_grid);
	  compare_buffers(lbfluid[index].n,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(x,y,1,lblattice.halo_grid);
	  if(compare_buffers(lbfluid[index].n,s_buffer,count*sizeof(double)))
	    fprintf(stderr,"buffers differ in dir=%d at index=%d x=%d y=%d\n",2,index,x,y);
	}
      
      }
    }
  }

  free(r_buffer);

}
#endif /* ADDITIONAL_CHECKS */

/***********************************************************************/

/** Performs basic sanity checks. */
static int lb_sanity_checks() {

  char *errtxt;
  int ret = 0;

    if (cell_structure.type != CELL_STRUCTURE_DOMDEC) {
      errtxt = runtime_error(128);
      ERROR_SPRINTF(errtxt, "{103 LB requires domain-decomposition cellsystem} ");
      ret = -1;
    } 
    else if (dd.use_vList) {
      errtxt = runtime_error(128);
      ERROR_SPRINTF(errtxt, "{104 LB requires no Verlet Lists} ");
      ret = -1;
    }    

    return ret;

}

/***********************************************************************/

/** (Re-)allocate memory for the fluid and initialize pointers. */
static void lb_create_fluid() {

  int index;

  lblattice.fields = realloc(lblattice.fields,lblattice.halo_grid_volume*sizeof(LB_FluidNode));
  lblattice.data = realloc(lblattice.data,lblattice.halo_grid_volume*n_fields*sizeof(double));

  lbfluid = lblattice.fields;
  lbfluid[0].n = lblattice.data;
#ifndef D3Q19
  lbfluid[0].n_new = lbfluid[0].n + lblattice.halo_grid_volume*n_veloc;
#endif

  for (index=0; index<lblattice.halo_grid_volume; index++) {
    lbfluid[index].n   = lbfluid[0].n + index*n_veloc;
#ifndef D3Q19
    lbfluid[index].n_new = lbfluid[0].n_new + index*n_veloc;
#endif
  }

  int lens[2] = { n_veloc, 1 };
  MPI_Aint disps[2] = { 0, n_veloc*sizeof(double) };
  MPI_Datatype types[2] = { MPI_DOUBLE, MPI_UB };
  MPI_Type_free(&lblattice.datatype);
  MPI_Type_struct(2, lens, disps, types, &lblattice.datatype);
  MPI_Type_commit(&lblattice.datatype);
  LB_TRACE(fprintf(stderr,"Potential memory hole!\n"));

}

/** Sets up the structures for exchange of the halo regions.
 *  See also \ref halo.c */
static void lb_prepare_communication() {

    /* create types for lattice data layout */
    int lens[2] = { n_veloc*sizeof(double), 1 };
    int disps[2] = { 0, n_veloc*sizeof(double) };
    Fieldtype fieldtype;
    halo_create_fieldtype(1, lens, disps, disps[1], &fieldtype);

    /* setup the halo communication */
    prepare_halo_communication(&update_halo_comm,&lblattice,fieldtype,lblattice.datatype);
 
    halo_free_fieldtype(&fieldtype);

}

/** Release the fluid. */
static void lb_release_fluid() {
  MPI_Type_free(&lblattice.datatype);
  free(lbfluid[0].n);
  free(lbfluid);
}

/** (Re-)initializes the fluid. */
void lb_reinit_parameters() {

  agrid   = lbpar.agrid;
  tau     = lbpar.tau;

  n_veloc = lbmodel.n_veloc;

  /* number of double entries in the data fields
   * velocity populations, density, momentum, stress tensor */
  n_fields = n_veloc;
#ifndef D3Q19
  n_fields += n_veloc; /* temporary velocity populations */
#endif

  /* Eq. (3) Ahlrichs and Duenweg, JCP 111(17):8225 (1999). */
  lblambda = -2./(6.*lbpar.viscosity*tau/(agrid*agrid)+1.) ;
    
  if (temperature > 0.0) {  /* fluctuating hydrodynamics ? */

    fluct = 1 ;

    /* lb_fluct_pref is stored in lattice units (pressure)
     * Eq. (7) Ahlrichs and Duenweg, JCP 111(17):8225 (1999).
     * The factor 12 comes from the fact that we use random numbers
     * from -0.5 to 0.5 (equally distributed) which have variance 1/12.
     */
    lb_fluct_pref = sqrt(12.*2.*lbpar.viscosity*lbpar.rho*temperature*SQR(lblambda)*tau*tau*tau/agrid);
    lb_fluct_pref_bulk = sqrt(12.*2.*lbpar.bulk_viscosity*lbpar.rho*temperature*SQR(lblambda_bulk)*tau*tau*tau/agrid);

    LB_TRACE(fprintf(stderr,"%d: lb_fluct_pref=%f (temp=%f, lambda=%f, tau=%f, agrid=%f)\n",this_node,lb_fluct_pref,temperature,lblambda,tau,agrid));

    /* lb_coupl_pref is stored in MD units (force)
     * Eq. (16) Ahlrichs and Duenweg, JCP 111(17):8225 (1999).
     * The factor 12 comes from the fact that we use random numbers
     * from -0.5 to 0.5 (equally distributed) which have variance 1/12.
     * time_step comes from the discretization.
     */
    lb_coupl_pref = sqrt(12.*2.*lbpar.friction*temperature/time_step); 

    LB_TRACE(fprintf(stderr,"%d: lb_coupl_pref=%f (temp=%f, friction=%f, time_step=%f)\n",this_node,lb_coupl_pref,temperature,lbpar.friction,time_step));

  } else {
    /* no fluctuations at zero temperature */
    fluct = 0 ;
    lb_fluct_pref = 0.0;
    lb_coupl_pref = 0.0;
  }

}

/** (Re-)initializes the fluid according to the given value of rho. */
void lb_reinit_fluid() {

    int k ;

    /* default values for fields in lattice units */
    double rho = lbpar.rho*agrid*agrid*agrid ;
    double v[3] = { 0., 0., 0. };
    double pi[6] = { rho*lbmodel.c_sound_sq, 0., rho*lbmodel.c_sound_sq, 0., 0., rho*lbmodel.c_sound_sq };

    for (k=0;k<lblattice.halo_grid_volume;k++) {

      if (lbfluid[k].boundary==0) {
	lb_set_local_fields(k,rho,v,pi) ;
      } else {
	lb_set_local_fields(k,0.0,v,pi);
      }

    }

}

/** Performs a full initialization of
 *  the Lattice Boltzmann system. All derived parameters
 *  and the fluid are reset to their default values. */
void lb_init() {

  if (lb_sanity_checks()) return;

  /* initialize derived parameters */
  lb_reinit_parameters();

  /* initialize the local lattice domain */
  init_lattice(&lblattice,agrid,tau);  

  if (check_runtime_errors()) return;

  /* allocate memory for data structures */
  lb_create_fluid();

#ifdef CONSTRAINTS
  /* setup boundaries of constraints */
  lb_init_constraints();
#endif

  /* setup the initial particle velocity distribution */
  lb_reinit_fluid();

  /* prepare the halo communication */
  lb_prepare_communication();

}

/** Release fluid and communication. */
void lb_release() {
  release_halo_communication(&update_halo_comm);
  lb_release_fluid();
}

/***********************************************************************/
/** \name Mapping between hydrodynamic fields and particle populations */
/***********************************************************************/
/*@{*/

/** Calculate local populations from hydrodynamic fields.
 *
 * The mapping is given in terms of the equilibrium distribution.
 *
 * Eq. (2.15) Ladd, J. Fluid Mech. 271, 295-309 (1994)
 * Eq. (4) in Berk Usta, Ladd and Butler, JCP 122, 094902 (2005)
 *
 * @param local_node Pointer to the local lattice site (Input).
 * @param trace      Trace of the local stress tensor (Input).
 * @param trace_eq   Trace of equilibriumd part of local stress tensor (Input).
 */
MDINLINE void lb_calc_local_n(LB_FluidNode *local_node) {

  double *local_n   = local_node->n;
  double *local_rho = local_node->rho;
  double *local_j   = local_node->j;
  double *local_pi  = local_node->pi;
  double trace;

#ifdef D3Q19
  const double rhoc_sq = *local_rho/3.0;
  const double avg_rho = lbpar.rho*agrid*agrid*agrid;
  double rho_times_coeff;
  double tmp1,tmp2;

  /* see Eq. (4) in Berk Usta, Ladd and Butler, JCP 122, 094902 (2005) */
  
  /* reduce the pressure tensor to the part needed here */
  local_pi[0] -= rhoc_sq;
  local_pi[2] -= rhoc_sq;
  local_pi[5] -= rhoc_sq;

  trace = local_pi[0] + local_pi[2] + local_pi[5];

  /* update the q=0 sublattice */
  local_n[0] = 1./3. * (*local_rho-avg_rho) - 1./2.*trace;

  /* update the q=1 sublattice */
  rho_times_coeff = 1./18. * (*local_rho-avg_rho);

  local_n[1] = rho_times_coeff + 1./6.*local_j[0] + 1./4.*local_pi[0] - 1./12.*trace;
  local_n[2] = rho_times_coeff - 1./6.*local_j[0] + 1./4.*local_pi[0] - 1./12.*trace;
  local_n[3] = rho_times_coeff + 1./6.*local_j[1] + 1./4.*local_pi[2] - 1./12.*trace;
  local_n[4] = rho_times_coeff - 1./6.*local_j[1] + 1./4.*local_pi[2] - 1./12.*trace;
  local_n[5] = rho_times_coeff + 1./6.*local_j[2] + 1./4.*local_pi[5] - 1./12.*trace;
  local_n[6] = rho_times_coeff - 1./6.*local_j[2] + 1./4.*local_pi[5] - 1./12.*trace;

  /* update the q=2 sublattice */
  rho_times_coeff = 1./36. * (*local_rho-avg_rho);

  tmp1 = local_pi[0] + local_pi[2];
  tmp2 = 2.0*local_pi[1];

  local_n[7]  = rho_times_coeff + 1./12.*(local_j[0]+local_j[1]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  local_n[8]  = rho_times_coeff - 1./12.*(local_j[0]+local_j[1]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  local_n[9]  = rho_times_coeff + 1./12.*(local_j[0]-local_j[1]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;
  local_n[10] = rho_times_coeff - 1./12.*(local_j[0]-local_j[1]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;

  tmp1 = local_pi[0] + local_pi[5];
  tmp2 = 2.0*local_pi[3];

  local_n[11] = rho_times_coeff + 1./12.*(local_j[0]+local_j[2]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  local_n[12] = rho_times_coeff - 1./12.*(local_j[0]+local_j[2]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  local_n[13] = rho_times_coeff + 1./12.*(local_j[0]-local_j[2]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;
  local_n[14] = rho_times_coeff - 1./12.*(local_j[0]-local_j[2]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;

  tmp1 = local_pi[2] + local_pi[5];
  tmp2 = 2.0*local_pi[4];

  local_n[15] = rho_times_coeff + 1./12.*(local_j[1]+local_j[2]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  local_n[16] = rho_times_coeff - 1./12.*(local_j[1]+local_j[2]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  local_n[17] = rho_times_coeff + 1./12.*(local_j[1]-local_j[2]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;
  local_n[18] = rho_times_coeff - 1./12.*(local_j[1]-local_j[2]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;

  /* restore the pressure tensor to the full part */
  local_pi[0] += rhoc_sq;
  local_pi[2] += rhoc_sq;
  local_pi[5] += rhoc_sq;

#else
  int i;
  const double rhoc_sq = *local_rho*lbmodel.c_sound_sq;

  /* see Eq. (4) in Berk Usta, Ladd and Butler, JCP 122, 094902 (2005) */

  /* reduce the pressure tensor to the part needed here */
  local_pi[0] -= rhoc_sq;
  local_pi[2] -= rhoc_sq;
  local_pi[5] -= rhoc_sq;

  trace = local_pi[0] + local_pi[2] + local_pi[5];

  for (i=0;i<n_veloc;i++) {

    tmp = 0.0;
    tmp += local_pi[0]*c[i][0]*c[i][0];
    tmp += (2.0*local_pi[1]*c[i][0]+local_pi[2]*c[i][1])*c[i][1];
    tmp += (2.0*(local_pi[3]*c[i][0]+local_pi[4]*c[i][1])+local_pi[5]*c[i][2])*c[i][2];

    local_n[i] =  coeff[i][0] * *local_rho;
    local_n[i] += coeff[i][1] * scalar(local_j,c[i]);
    local_n[i] += coeff[i][2] * tmp;
    local_n[i] += coeff[i][3] * trace;

    /* restore the pressure tensor to the full part */
    local_pi[0] += rhoc_sq;
    local_pi[2] += rhoc_sq;
    local_pi[5] += rhoc_sq;
#endif

#ifdef ADDITIONAL_CHECKS
    int i;
    for (i=0;i<n_veloc;i++) {
	if (lbmodel.coeff[i][0]*lbpar.rho+local_n[i] < 0.0) {
	    char *errtxt;
	    errtxt = runtime_error(128);
	    ERROR_SPRINTF(errtxt,"{105 Unexpected negative population} ");
	}
    }
#endif
	    
}
  
MDINLINE void lb_map_fields_to_populations() {
    int k;

    for (k=0; k<lblattice.halo_grid_volume; k++) {
	lb_calc_local_fields(&lbfluid[k],1);
    }

}

MDINLINE void lb_map_populations_to_fields() {
    int k;

    for (k=0; k<lblattice.halo_grid_volume; k++) {
	lb_calc_local_n(&lbfluid[k]);
    }

}

/*@}*/

/***********************************************************************/
/** \name Collision step */
/***********************************************************************/
/*@{*/

/** Collision update of the stress tensor.
 * The stress tensor is relaxed towards the equilibrium.
 *
 * See Eq. (5) in Berk Usta, Ladd and Butler, JCP 122, 094902 (2005)
 *
 * @param local_node Pointer to the local lattice site (Input).
 * @param trace      Trace of local stress tensor (Output).
 * @param trace_eq   Trace of equilibrium part of local stress tensor (Output).
 */
MDINLINE void lb_update_local_pi(LB_FluidNode *local_node) {

  const double local_rho = *(local_node->rho);
  double *local_j  = local_node->j;
  double *local_pi = local_node->pi;
  double local_pi_eq[6];
  double trace, trace_eq;
  double tmp;

  const double rhoc_sq = local_rho*lbmodel.c_sound_sq;
  const double onepluslambda = 1.0 + lblambda;

  /* calculate the equilibrium part of the pressure tensor */
  local_pi_eq[0] = rhoc_sq + local_j[0]*local_j[0]/local_rho;
  tmp = local_j[1]/local_rho;
  local_pi_eq[1] = local_j[0]*tmp;
  local_pi_eq[2] = rhoc_sq + local_j[1]*tmp;
  tmp = local_j[2]/local_rho;
  local_pi_eq[3] = local_j[0]*tmp;
  local_pi_eq[4] = local_j[1]*tmp;
  local_pi_eq[5] = rhoc_sq + local_j[2]*tmp;

  /* calculate the traces */
  trace_eq = local_pi_eq[0] + local_pi_eq[2] + local_pi_eq[5];
  trace = local_pi[0] + local_pi[2] + local_pi[5];
    
  /* relax the local pressure tensor */
  local_pi[0] = local_pi_eq[0] + onepluslambda*(local_pi[0] - local_pi_eq[0]);
  local_pi[1] = local_pi_eq[1] + onepluslambda*(local_pi[1] - local_pi_eq[1]);
  local_pi[2] = local_pi_eq[2] + onepluslambda*(local_pi[2] - local_pi_eq[2]);
  local_pi[3] = local_pi_eq[3] + onepluslambda*(local_pi[3] - local_pi_eq[3]);
  local_pi[4] = local_pi_eq[4] + onepluslambda*(local_pi[4] - local_pi_eq[4]);
  local_pi[5] = local_pi_eq[5] + onepluslambda*(local_pi[5] - local_pi_eq[5]);  
  tmp = 1./3.*(lblambda_bulk-lblambda)*(trace - trace_eq);
  local_pi[0] += tmp;
  local_pi[2] += tmp;
  local_pi[5] += tmp;

}

/** Add fluctuating part to the stress tensor and update the populations.
 *
 * Ladd, J. Fluid Mech. 271, 285-309 (1994).<br>
 * Berk Usta, Ladd and Butler, JCP 122, 094902 (2005).<br>
 * Ahlrichs, PhD-Thesis (2000).
 *   
 * @param local_node Pointer to the local lattice site.
 * @param trace      Trace of local stress tensor (Input).
 * @param trace_eq   Trace of equilibrium part of the stress tensor (Input).
 * @param badrandoms Flag/Counter for the occurence of negative populations (Output).
 */
MDINLINE void lb_add_fluct_pi(LB_FluidNode *local_node) {

  double *local_pi = local_node->pi;
  double tmp, sum=0.0;

  const double pref1 = sqrt(2) * lb_fluct_pref;

  /* off-diagonal components */
  local_pi[1] += lb_fluct_pref * (d_random()-0.5);
  local_pi[3] += lb_fluct_pref * (d_random()-0.5);
  local_pi[4] += lb_fluct_pref * (d_random()-0.5);

  /* diagonal components */
  tmp = pref1 * (d_random()-0.5);
  sum += tmp;
  local_pi[0] += tmp;
  tmp = pref1 * (d_random()-0.5);
  sum += tmp;
  local_pi[2] += tmp;
  tmp = pref1 * (d_random()-0.5);
  sum += tmp;
  local_pi[5] += tmp;

  /* make fluctuations traceless */
  sum /= 3.0;
  local_pi[0] -= sum;
  local_pi[2] -= sum;
  local_pi[5] -= sum;

  /* additional bulk fluctuations */
  local_pi[0] += lb_fluct_pref_bulk * (d_random()-0.5);
  local_pi[2] += lb_fluct_pref_bulk * (d_random()-0.5);
  local_pi[5] += lb_fluct_pref_bulk * (d_random()-0.5);

}

/** Check for negative populations.  
 *
 * Looks for negative populations and increases badrandoms for each
 * occurence.
 *
 * @param local_node Pointer to the local lattice site (Input).
 * @param badrandoms Counts occurences of negative populations (Output).
 */
MDINLINE void lb_check_negative_n(LB_FluidNode *local_node, int *badrandoms) { 

  int i;
  const double *local_n = local_node->n;

  for (i=0; i<n_veloc; i++) {
    if (lbmodel.coeff[i][0]*lbpar.rho+local_n[i]<0.0) {
      *badrandoms++;
      fprintf(stderr,"%d: Negative population. Check your parameters if this happens too often! (badrandoms=%d)\n",this_node,*badrandoms);
   }
  }

}

/** The Lattice Boltzmann collision step.
 * Loop over all lattice sites and perform the collision update.
 * If fluctuations are present, the fluctuating part of the stress tensor
 * is added. The update is only accepted then, if no negative populations
 * occur.
 */
MDINLINE void lb_calc_collisions() {

  int i, index, x, y, z;
  int badrandoms;
  LB_FluidNode *local_node;
  double *local_pi, save_local_pi[6] ; 
  
  /* loop over all nodes (halo excluded) */
  index = lblattice.halo_offset;
  for (z=1;z<=lblattice.grid[2];z++) {
    for (y=1;y<=lblattice.grid[1];y++) {
      for (x=1;x<=lblattice.grid[0];x++) {

	local_node = &lbfluid[index];

	lb_calc_local_fields(local_node,1);

#ifdef ADDITIONAL_CHECKS
	double old_rho = *(local_node->rho);
#endif

	lb_update_local_pi(local_node) ;
	
	if (fluct) {
	  
	  local_pi  = local_node->pi;
	  
	  /* save the pressure tensor */
	  for (i=0;i<6;i++) {
	    save_local_pi[i] = local_pi[i];
	  }
	  
	  do { /* try random numbers until no negative populations occur */
	    
	    badrandoms = 0 ;
	    
	    lb_add_fluct_pi(local_node);
	    lb_calc_local_n(local_node);
	    lb_check_negative_n(local_node,&badrandoms);
	    
	    if (badrandoms>0) {
	      fprintf(stderr,"%d: Negative population (badrandoms=%d). Check your parameters if this happens too often!\n",this_node,badrandoms);
	      
	      /* restore the local pressure tensor */
	      for (i=0;i<6;i++) {
		local_pi[i] = save_local_pi[i];
	      }
	    }
	    
	  } while (badrandoms>0);

	} else {
	  
	  lb_calc_local_n(local_node);
	  
	}
	  
#ifdef ADDITIONAL_CHECKS
	  double *local_rho = local_node->rho;
	  lb_calc_local_rho(local_node);
	  if (fabs(*local_rho-old_rho) > ROUND_ERROR_PREC) {
	    char *errtxt = runtime_error(128 + TCL_DOUBLE_SPACE + 3*TCL_INTEGER_SPACE);
	    ERROR_SPRINTF(errtxt,"{106 Mass loss/gain %le in lb_calc_collisions on site (%d,%d,%d)} ",*local_rho-old_rho,x,y,z);
	    fprintf(stderr,"Mass loss\n");
	    errexit();
	  }
#endif
	
	++index;
      }
      index += 2;
    }
    index += 2*lblattice.halo_grid[0];
  }

}

/*@}*/

/***********************************************************************/
/** \name Streaming step */
/***********************************************************************/
/*@{*/

/** The Lattice Boltzmann streaming step.
 * The populations are moved to the neighbouring lattice sites
 * according to the velocity sublattice. This can be done in two ways:
 * First, one can use a temporary field to store the updated configuration.
 * Second, one can order the updates such that only populations are
 * overwritten which have already been propagated. The halo region
 * serves as a buffer. This requires two sweeps through the lattice,
 * one bottom up and one bottom down. One has to be careful if the
 * velocities are upgoing or downgoing. This can be a real bugfest!
 */
MDINLINE void lb_propagate_n() {

#ifdef D3Q19
  int k, index;
  int yperiod = lblattice.halo_grid[0];
  int zperiod = lblattice.halo_grid[0]*lblattice.halo_grid[1];
  int next[19];
  next[0]  =   n_veloc * 0 + 0;                  // ( 0, 0, 0) =
  next[1]  =   n_veloc * 1 + 1;                  // ( 1, 0, 0) +
  next[2]  = - n_veloc * 1 + 2;                  // (-1, 0, 0)
  next[3]  =   n_veloc * yperiod + 3;            // ( 0, 1, 0) +
  next[4]  = - n_veloc * yperiod + 4;            // ( 0,-1, 0)
  next[5]  =   n_veloc * zperiod + 5;            // ( 0, 0, 1) +
  next[6]  = - n_veloc * zperiod + 6;            // ( 0, 0,-1)
  next[7]  =   n_veloc * (1+yperiod) + 7;        // ( 1, 1, 0) +
  next[8]  = - n_veloc * (1+yperiod) + 8;        // (-1,-1, 0)
  next[9]  =   n_veloc * (1-yperiod) + 9;        // ( 1,-1, 0)
  next[10] = - n_veloc * (1-yperiod) + 10;       // (-1, 1, 0) +
  next[11] =   n_veloc * (1+zperiod) + 11;       // ( 1, 0, 1) +
  next[12] = - n_veloc * (1+zperiod) + 12;       // (-1, 0,-1)
  next[13] =   n_veloc * (1-zperiod) + 13;       // ( 1, 0,-1)
  next[14] = - n_veloc * (1-zperiod) + 14;       // (-1, 0, 1) +
  next[15] =   n_veloc * (yperiod+zperiod) + 15; // ( 0, 1, 1) +
  next[16] = - n_veloc * (yperiod+zperiod) + 16; // ( 0,-1,-1)
  next[17] =   n_veloc * (yperiod-zperiod) + 17; // ( 0, 1,-1)
  next[18] = - n_veloc * (yperiod-zperiod) + 18; // ( 0,-1, 1) +

  double *n = lbfluid[0].n;

  /* top down sweep */
  index = (lblattice.halo_grid_volume-lblattice.halo_offset-1)*n_veloc;
  for (k=lblattice.halo_grid_volume-lblattice.halo_offset-1;k>=0;k--) {
      
    /* propagation to higher indices */
    n[index+next[1]]  = n[index+1];
    n[index+next[3]]  = n[index+3];
    n[index+next[5]]  = n[index+5];
    n[index+next[7]]  = n[index+7];
    n[index+next[10]] = n[index+10];
    n[index+next[11]] = n[index+11];
    n[index+next[14]] = n[index+14];
    n[index+next[15]] = n[index+15];
    n[index+next[18]] = n[index+18];

    index -= n_veloc;
  }

  /* bottom up sweep */
  index = lblattice.halo_offset*n_veloc;
  for (k=lblattice.halo_offset;k<lblattice.halo_grid_volume;k++) {

    /* propagation to lower indices */
    n[index+next[2]]  = n[index+2];
    n[index+next[4]]  = n[index+4];
    n[index+next[6]]  = n[index+6];
    n[index+next[8]]  = n[index+8];
    n[index+next[9]]  = n[index+9];
    n[index+next[12]] = n[index+12];
    n[index+next[13]] = n[index+13];
    n[index+next[16]] = n[index+16];
    n[index+next[17]] = n[index+17];

    index += n_veloc;
  }

#else
  double next[n_veloc], *tmp ;

  /* In the general case, we don't know a priori which 
   * velocities propagate to higher or lower indices.
   * So we use a complete new array as buffer and
   * swap the pointers afterwards.
   */ 

  double n_new = lbfluid[0].n_new;

  /* calculate the index shift for all velocities */
  for (i=0;i<n_veloc;i++) {
    next[i] = n_veloc*(c[i][0]+yperiod*c[i][1]+zperiod*c[i][2]) + i;
  }

  /* propagate the populations */
  /* on the surface we have to check that shifts 
   * don't lead out of the cell's node range */
  index = 0;
  for (k=0; k<gridbegin; k+=1) {
    for (i=0;i<n_veloc;i++) {
      next = index+next[i];
      if (next>=0) {
	  n_new[next] = n[index+i];
      }
    }
    tmp = lbfluid[k].n;
    lbfluid[k].n = lbfluid[k].n_new;
    lbfluid[k].n_new = tmp;
    index += n_veloc;
  }
  index = gridbegin*n_veloc;
  for (k=gridbegin;k<(xyzcube+gridsurface-gridbegin);k+=1) {
    for (i=0;i<n_veloc;i++) {
	n_new[next] = n[index+i];
    }
    tmp = lbfluid[k].n;
    lbfluid[k].n = lbfluid[k].n_new;
    lbfluid[k].n_new = tmp;
    index += n_veloc;
  }
  index = (xyzcube+gridsurface-gridbegin)*n_veloc;
  for (k=(xyzcube+gridsurface-gridbegin);k<(xyzcube+gridsurface);k+=1) {
    for (i=0;i<n_veloc;i++) {
      next = index+next[i];
      if (next<(xyzcube+gridsurface)*n_veloc) {
	  n_new[next] = n[index+i];
      }
    }
    tmp = lbfluid[k].n;
    lbfluid[k].n = lbfluid[k].n_new;
    lbfluid[k].n_new = tmp;
    index += n_veloc;
  }
  
#endif

}

/*@}*/

/***********************************************************************/
/** \name External forces */
/***********************************************************************/
/*@{*/

/** Apply external forces to the fluid.
 *
 * Eq. (28) Ladd and Verberg, J. Stat. Phys. 104(5/6):1191 (2001).
 * Note that the second moment of the force is neglected.
 */
MDINLINE void lb_external_forces() {

  int x, y, z, index;
  double *local_n, *local_j, delta_j[3] = { 0.0, 0.0, 0.0 };

  index = lblattice.halo_offset;
  for (z=1; z<=lblattice.grid[2]; z++) {
    for (y=1; y<=lblattice.grid[1]; y++) {
      for (x=1; x<=lblattice.grid[0]; x++) {

	if (lbfluid[index].boundary==0) 
	{

	  local_n   = lbfluid[index].n;
	  local_j   = lbfluid[index].j;

	  /* calculate momentum due to ext_force in lattice units */
	  /* ext_force is the force per volume in LJ units */
	  delta_j[0] = lbpar.ext_force[0]*tau*tau*agrid*agrid;
	  delta_j[1] = lbpar.ext_force[1]*tau*tau*agrid*agrid;
	  delta_j[2] = lbpar.ext_force[2]*tau*tau*agrid*agrid;
	  
	  local_j[0] += delta_j[0];
	  local_j[1] += delta_j[1];
	  local_j[2] += delta_j[2];

#ifdef D3Q19
	  local_n[1]  +=   1./6. * delta_j[0];
	  local_n[2]  += - 1./6. * delta_j[0];
	  local_n[3]  +=   1./6. * delta_j[1];
	  local_n[4]  += - 1./6. * delta_j[1];
	  local_n[5]  +=   1./6. * delta_j[2];
	  local_n[6]  += - 1./6. * delta_j[2];
	  local_n[7]  +=   1./12. * (delta_j[0]+delta_j[1]);
	  local_n[8]  += - 1./12. * (delta_j[0]+delta_j[1]);
	  local_n[9]  +=   1./12. * (delta_j[0]-delta_j[1]);
	  local_n[10] += - 1./12. * (delta_j[0]-delta_j[1]);
	  local_n[11] +=   1./12. * (delta_j[0]+delta_j[2]);
	  local_n[12] += - 1./12. * (delta_j[0]+delta_j[2]);
	  local_n[13] +=   1./12. * (delta_j[0]-delta_j[1]);
	  local_n[14] += - 1./12. * (delta_j[0]-delta_j[1]);
	  local_n[15] +=   1./12. * (delta_j[1]+delta_j[2]);
	  local_n[16] += - 1./12. * (delta_j[1]+delta_j[2]);
	  local_n[17] +=   1./12. * (delta_j[1]-delta_j[2]);
	  local_n[18] += - 1./12. * (delta_j[1]-delta_j[2]);
#else
	  for (i=0; i<n_veloc; i++) {
	      local_n[i] += lbcoeff[i][2]*scalar(c[i],delta_j);
	  }
#endif

	}
	++index;
      }
      index += 2;
    }
    index += 2*lblattice.halo_grid[0];
  }

}

/*@}*/

/***********************************************************************/
/** \name Integration step for the lattice Boltzmann fluid             */
/***********************************************************************/
/*@{*/

/** Propagate the Lattice Boltzmann dynamics.
 * This function is called from the integrator. Since the time step
 * for the lattice dynamics can be coarser than the MD time step,
 * we monitor the time since the last lattice update.
 */
void lb_propagate() {

  fluidstep+=time_step ;

  if (fluidstep>=tau) {

    fluidstep=0.0 ;

    /* collision step */
    lb_calc_collisions();

#ifdef EXTERNAL_FORCES
    /* apply external forces */
    lb_external_forces();
#endif

    /* exchange halo regions */
    halo_communication(&update_halo_comm);
#ifdef ADDITIONAL_CHECKS
    lb_check_halo_regions();
#endif

#ifdef CONSTRAINTS
    /* boundary conditions */
    lb_boundary_conditions();
#endif

    /* streaming step */
    lb_propagate_n();

  }

}

/***********************************************************************/
/** \name Coupling part */
/***********************************************************************/
/*@{*/

/** Transfer a certain amount of momentum to a elementray cell of fluid.
 * 
 * Eq. (14) Ahlrichs and Duenweg, JCP 111(17):8225 (1999).
 *
 * @param momentum   Momentum to be transfered to the fluid (lattice
 *                   units) (Input).
 * @param node_index Indices of the sites of the elementary lattice
 *                   cell (Input).
 * @param delta      Weights for the assignment to the single lattice
 *                   sites (Input).
 * @param badrandoms Flag/Counter for the occurrence negative
 *                   populations (Output).
 */
MDINLINE void lb_transfer_momentum(const double momentum[3], const int node_index[8], const double delta[6], int *badrandoms) {

  int i, x, y, z;
  double *local_j, delta_j[3];
  double *local_n, *n_new ;

  /* We don't need to save the local populations because 
   * we use a trick for their restoration:
   * We substract the old force from the new one,
   * hence the previous change in the local populations
   * is automatically revoked during the recalculation.
   * Note that this makes it necessary to actually apply 
   * all changes and forbids to return immediately when negative
   * populations occur.
   */

  for (z=0;z<2;z++) {
    for (y=0;y<2;y++) {
      for (x=0;x<2;x++) {

	n_new = local_n = lbfluid[node_index[(z*2+y)*2+x]].n;
	local_j = lbfluid[node_index[(z*2+y)*2+x]].j;

	delta_j[0] = delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*momentum[0];
	delta_j[1] = delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*momentum[1];
	delta_j[2] = delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*momentum[2];

#ifdef D3Q19
	n_new[1]  = local_n[1]  + 1./6.*delta_j[0];
	n_new[2]  = local_n[2]  - 1./6.*delta_j[0];
	n_new[3]  = local_n[3]  + 1./6.*delta_j[1];
	n_new[4]  = local_n[4]  - 1./6.*delta_j[1];
	n_new[5]  = local_n[5]  + 1./6.*delta_j[2];
	n_new[6]  = local_n[6]  - 1./6.*delta_j[2];
	n_new[7]  = local_n[7]  + 1./12.*(delta_j[0]+delta_j[1]);
	n_new[8]  = local_n[8]  - 1./12.*(delta_j[0]+delta_j[1]);
	n_new[9]  = local_n[9]  + 1./12.*(delta_j[0]-delta_j[1]);
	n_new[10] = local_n[10] - 1./12.*(delta_j[0]-delta_j[1]);
	n_new[11] = local_n[11] + 1./12.*(delta_j[0]+delta_j[2]);
	n_new[12] = local_n[12] - 1./12.*(delta_j[0]+delta_j[2]);
	n_new[13] = local_n[13] + 1./12.*(delta_j[0]-delta_j[2]);
	n_new[14] = local_n[14] - 1./12.*(delta_j[0]-delta_j[2]);
	n_new[15] = local_n[15] + 1./12.*(delta_j[1]+delta_j[2]);
	n_new[16] = local_n[16] - 1./12.*(delta_j[1]+delta_j[2]);
	n_new[17] = local_n[17] + 1./12.*(delta_j[1]-delta_j[2]);
	n_new[18] = local_n[18] - 1./12.*(delta_j[1]-delta_j[2]);

	for (i=0;i<19;i++) {
	  if (lbmodel.coeff[i][0]*lbpar.rho+n_new[i]<0.0) {
	    (*badrandoms)++;
	    LB_TRACE(fprintf(stderr,"%d: (%d,%d,%d) negative population [%d]=%f (badrandoms=%d)\n",this_node,x,y,z,i,n_new[i],*badrandoms));
	    /* DO NOT break and return immediately here! */
	  }
	}
#else

	for (i=0;i<n_veloc;i++) {
	  tmp = 0.0 ;
	  for (k=0;k<3;k++) {
	    tmp += delta_j[k] * c_g_d[i][k] ;
	  }
	  n_new[i] = local_n[i] + coeff[i][1] * tmp ;
	}

	for (i=0;i<n_veloc;i++) {
	  if (n_new[i]<0.0) {
	    (*badrandoms)++ ;
	    LB_TRACE(fprintf(stderr,"%d: (%d,%d,%d) negative population %d (badrandoms=%d)\n",this_node,x,y,z,i,*badrandoms));
	    /* DO NOT break and return immediately here! */
	  }
	}

#endif

      }
    }
  }

}

/** Coupling of a particle to viscous fluid with Stokesian friction.
 * 
 * Section II.C. Ahlrichs and Duenweg, JCP 111(17):8225 (1999)
 *
 * @param p          The coupled particle (Input).
 * @param badrandoms Flag/Counter for occurence of negative
 *                   populations (Output).
 * @param p_is_ghost Flag indicating whether the particle is a ghost
 *                   particle. Ghost particles must not have the force
 *                   added since it is already included in the real
 *                   image. However, ghosts must be treated to
 *                   transfer momentum to sites on different processors.
 */
MDINLINE void lb_viscous_momentum_exchange(Particle *p, int *badrandoms, int p_is_ghost) {

  int x,y,z,dir ;
  int node_index[8] ;
  double delta[6] ;
  double *local_rho, *local_j, interpolated_u[3], force[3], delta_j[3];
#ifdef ADDITIONAL_CHECKS
  double old_rho[8];
#endif

  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: f_random = (%.3e,%.3e,%.3e)\n",this_node,p->lc.f_random[0],p->lc.f_random[1],p->lc.f_random[2]));
  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: f = (%.3e,%.3e,%.3e)\n",this_node,p->f.f[0],p->f.f[1],p->f.f[2]));

  /* determine elementary lattice cell surrounding the particle 
     and the relative position of the particle in this cell */ 
  map_position_to_lattice(&lblattice,p->r.p,node_index,delta) ;

  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB delta=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f) pos=(%.3f,%.3f,%.3f)\n",this_node,delta[0],delta[1],delta[2],delta[3],delta[4],delta[5],p->r.p[0],p->r.p[1],p->r.p[2]));

  /* calculate fluid velocity at particle's position
     this is done by linear interpolation
     (Eq. (11) Ahlrichs and Duenweg, JCP 111(17):8225 (1999)) */
  interpolated_u[0] = interpolated_u[1] = interpolated_u[2] = 0.0 ;
  for (z=0;z<2;z++) {
    for (y=0;y<2;y++) {
      for (x=0;x<2;x++) {

	local_rho = lbfluid[node_index[z*4+y*2+x]].rho;
#ifdef ADDITIONAL_CHECKS
	old_rho[z*4+y*2+x] = *local_rho;
#endif
	local_j = lbfluid[node_index[z*4+y*2+x]].j ;
	ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB fluid (%d,%d,%d) local_rho=%.3f local_j=(%.16e,%.3f,%.3f)\n",this_node,x,y,z,*local_rho,local_j[0],local_j[1],local_j[2]));

	for (dir=0;dir<3;dir++) {
	  interpolated_u[dir] += delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*local_j[dir]/(*local_rho) ;
	}

      }
    }
  }
  
  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB u = (%.16e,%.3e,%.3e) v = (%.16e,%.3e,%.3e)\n",this_node,interpolated_u[0],interpolated_u[1],interpolated_u[2],p->m.v[0],p->m.v[1],p->m.v[2]));

   /* calculate viscous force and add to random force
     (take care to rescale the velocities with the time_step
     and transform fluid velocity to MD units) 
     (Eq. (9) Ahlrichs and Duenweg, JCP 111(17):8225 (1999)) */
  for (dir=0;dir<3;dir++) {
    force[dir] = - lbpar.friction * (p->m.v[dir]/time_step - interpolated_u[dir]*agrid/tau);    
    p->lc.f_random[dir] += - lbpar.friction * (p->m.v[dir]/time_step - interpolated_u[dir]*agrid/tau);
  }

  /* exchange momentum */
  if (transfer_momentum) {

    /* add force to particle if not ghost */
    if (!p_is_ghost) {
      p->f.f[0] += p->lc.f_random[0];
      p->f.f[1] += p->lc.f_random[1];
      p->f.f[2] += p->lc.f_random[2];

      ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f_drag = (%.6e,%.3e,%.3e)\n",this_node,force[0],force[1],force[2]));
      ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f_tot = (%.6e,%.3e,%.3e)\n",this_node,p->lc.f_random[0],p->lc.f_random[1],p->lc.f_random[2]));
      ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f = (%.6e,%.3e,%.3e)\n",this_node,p->f.f[0],p->f.f[1],p->f.f[2]));
    }

    /* transform momentum transfer to lattice units
       (Eq. (12) Ahlrichs and Duenweg, JCP 111(17):8225 (1999)) */
    for (dir=0;dir<3;dir++) {
      delta_j[dir] = - p->lc.f_random[dir]*time_step*tau/agrid;
    }

    int save_bad = *badrandoms;
    lb_transfer_momentum(delta_j,node_index,delta,badrandoms);

    if (*badrandoms > save_bad) {
      fprintf(stderr,"%d: badrandoms=%d for particle %d!\n",this_node,*badrandoms,p->p.identity);
      if (*badrandoms > 1000) errexit(1);
    }

  }

#ifdef ADDITIONAL_CHECKS
  int i;
  for (i=0;i<8;i++) {
    lb_calc_local_rho(&lbfluid[node_index[i]]);
    local_rho = lbfluid[node_index[i]].rho;
    if (fabs(*local_rho-old_rho[i]) > ROUND_ERROR_PREC) {
      char *errtxt = runtime_error(128);
      ERROR_SPRINTF(errtxt,"{108 Mass loss/gain %le in lb_viscous_momentum_exchange} ",*local_rho-old_rho[i]);
    }
  }
#endif

}

/** Calculate particle lattice interactions.
 * So far, only viscous coupling with Stokesian friction is
 * implemented.
 * Include all particle-lattice forces in this function.
 * The function is called from \ref force_calc.
 *
 * Parallelizing the fluid particle coupling is not straightforward
 * because drawing of random numbers makes the whole thing nonlocal.
 * One way to do it is to treat every particle only on one node, i.e.
 * the random numbers need not be communicated. The particles that are 
 * not fully inside the local lattice are taken into account via their
 * ghost images on the neighbouring nodes. But this requires that the 
 * correct values of the surrounding lattice nodes are available on 
 * the respective node, which means that we have to communicate the 
 * halo regions before treating the ghost particles. Moreover, after 
 * determining the ghost couplings, we have to communicate back the 
 * halo region such that all local lattice nodes have the correct values.
 * Thus two communication phases are involved which will most likely be 
 * the bottleneck of the computation.
 *
 * Another way of dealing with the particle lattice coupling is to 
 * treat a particle and all of it's images explicitly. This requires the
 * communication of the random numbers used in the calculation of the 
 * coupling force. The problem is now that, if random numbers have to 
 * be redrawn, we cannot efficiently determine which particles and which 
 * images have to be re-calculated. We therefore go back to the outset
 * and go through the whole system again until no failure occurs during
 * such a sweep. In the worst case, this is very inefficient because
 * many things are recalculated although they actually don't need.
 * But we can assume that this happens extremely rarely and then we have
 * on average only one communication phase for the random numbers, which
 * probably makes this method preferable compared to the above one.
 */
void calc_particle_lattice_ia() {

  int i, k, dir, c, np, badrandoms, allbadrandoms ;
  Cell *cell ;
  Particle *p ;

  /* exchange halo regions */
  halo_communication(&update_halo_comm) ;
#ifdef ADDITIONAL_CHECKS
  lb_check_halo_regions();
#endif
    
  for (k=0;k<lblattice.halo_grid_volume;k++) {
    lb_calc_local_fields(&lbfluid[k],0);
  }

   /* draw random numbers for local particles */
  for (c=0;c<local_cells.n;c++) {
    cell = local_cells.cell[c] ;
    p = cell->part ;
    np = cell->n ;
    for (i=0;i<np;i++) {
      double x[3];
      for (dir=0;dir<3;dir++) {
	x[dir] = d_random()-0.5;
	p[i].lc.f_random[dir] = -lb_coupl_pref*x[dir] ;
      }
      ONEPART_TRACE(if (p[i].p.identity==check_id) fprintf(stderr, "%d: OPT: LB f_random = (%.6e,%.3e,%.3e)\n",this_node,p[i].lc.f_random[0],p[i].lc.f_random[1],p[i].lc.f_random[2]));
    }
  }

  /* try random numbers until no failure occurs during a whole sweep */
  int try = 0;
  do {

    allbadrandoms = 0;
    badrandoms = 0;
    try++;

    /* communicate the random numbers */
    ghost_communicator(&cell_structure.ghost_lbcoupling_comm) ;
    
    /* local cells */
    for (c=0;c<local_cells.n;c++) {
      cell = local_cells.cell[c] ;
      p = cell->part ;
      np = cell->n ;

      for (i=0;i<np;i++) {

	lb_viscous_momentum_exchange(&p[i],&badrandoms,0) ;

      }

    }

    /* ghost cells */
    for (c=0;c<ghost_cells.n;c++) {
      cell = ghost_cells.cell[c] ;
      p = cell->part ;
      np = cell->n ;

      for (i=0;i<np;i++) {

	  /* for ghost particles we have to check if they lie
	   * in the range of the local lattice nodes */
	if (p[i].r.p[0] >= my_left[0]-lblattice.agrid && p[i].r.p[0] < my_right[0]
	    && p[i].r.p[1] >= my_left[1]-lblattice.agrid && p[i].r.p[1] < my_right[1]
	    && p[i].r.p[2] >= my_left[2]-lblattice.agrid && p[i].r.p[2] < my_right[2]) {

	  ONEPART_TRACE(if(p[i].p.identity==check_id) fprintf(stderr,"%d: OPT: coupling of ghost\n",this_node));
	  lb_viscous_momentum_exchange(&p[i],&badrandoms,1) ;

	}
      }
    }

    MPI_Allreduce(&badrandoms, &allbadrandoms, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD) ;

    if (allbadrandoms>0) {

        fprintf(stderr, "%d: Momentum error (badrandoms=%d, allbadrandoms=%d). Check your parameters if this happens too often!\n",this_node,badrandoms,allbadrandoms);

	if (try > 100) {
	  fprintf(stderr, "too many tries %d\n",try);
	  errexit(1);
	}

	for (c=0;c<local_cells.n;c++) {
	  cell = local_cells.cell[c] ;
	  p = cell->part ;
	  np = cell->n ;
	  for (i=0;i<np;i++) {
	    double x[3];
	    for (dir=0;dir<3;dir++) {
	      x[dir] = lb_coupl_pref*(d_random()-0.5);
	      p[i].lc.f_random[dir] = x[dir] - p[i].lc.f_random[dir] ;
	    }
	    ONEPART_TRACE(if (p[i].p.identity==check_id) fprintf(stderr, "%d: OPT: LB f_newran = (%.6e,%.3e,%.3e)\n",this_node,x[0],x[1],x[2]));
	  }
	}
    }

  } while (allbadrandoms>0) ;

}

/***********************************************************************/

/** Calculate the average density of the fluid in the system.
 * This function has to be called after changing the density of
 * a local lattice site in order to set lbpar.rho consistently. */
void lb_calc_average_rho() {

  int x, y, z, index;
  double rho, sum_rho;

  rho = 0.0;
  index = 0;
  for (z=1; z<=lblattice.grid[2]; z++) {
    for (y=1; y<=lblattice.grid[1]; y++) {
      for (x=1; x<=lblattice.grid[0]; x++) {
	
	lb_calc_local_rho(&lbfluid[index]);
	rho += *lbfluid[index].rho;

	index++;
      }
      index += 2;
    }
    index += 2*lblattice.halo_grid[0];
  }

  MPI_Allreduce(&rho, &sum_rho, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

  /* calculate average density in MD units */
  lbpar.rho = sum_rho / (box_l[0]*box_l[1]*box_l[2]);

}

/** Returns the mass, momentum and stress of a local lattice site.
 * @param index The index of the lattice site within the local domain (Input)
 * @param rho   Local density of the fluid (Output)
 * @param j     Local momentum of the fluid (Output)
 * @param pi    Local stress tensor of the fluid (Output)
 */
void lb_get_local_fields(int index, double *rho, double *j, double *pi) {

  int i,k,m;

  double *local_rho = lbfluid[index].rho;
  double *local_j   = lbfluid[index].j;
  double *local_pi  = lbfluid[index].pi;

  lb_calc_local_fields(&lbfluid[index],1);

  *rho = *local_rho;
  m = 0;
  for (i=0;i<3;i++) {
    j[i] = local_j[i];
    for (k=0;k<i;k++) {
      pi[m] = local_pi[m];
      m++;
    }
  }

}

/** Sets the density and momentum on a local lattice site.
 * @param index The index of the lattice site within the local domain (Input)
 * @param rho   Local density of the fluid (Input)
 * @param v     Local velocity of the fluid (Input)
 */
void lb_set_local_fields(int index, const double rho, const double *v, const double *pi) {

  LB_FluidNode *local_node = &lbfluid[index];
  double *local_rho = local_node->rho;
  double *local_j = local_node->j;
  double *local_pi = local_node->pi;

  *local_rho = rho;

  local_j[0] = rho * v[0];
  local_j[1] = rho * v[1];
  local_j[2] = rho * v[2];
  
  local_pi[0] = pi[0];
  local_pi[1] = pi[1];
  local_pi[2] = pi[2];
  local_pi[3] = pi[3];
  local_pi[4] = pi[4];
  local_pi[5] = pi[5];

  /* calculate populations according to equilibrium distribution */
  lb_calc_local_n(local_node);

}

/*@}*/

/***********************************************************************/
/** \name TCL stuff */
/***********************************************************************/

static int lb_parse_set_fields(Tcl_Interp *interp, int argc, char **argv, int *change, int *ind) {

  int k, index, node, grid[3];
  double rho, j[3], pi[6];

  *change = 4 ;
  if (argc < 4) return TCL_ERROR ;
  if (!ARG0_IS_D(rho)) return TCL_ERROR ;
  for (k=0;k<3;k++) {
    if (!ARG_IS_D(k+1,j[k])) return TCL_ERROR ;
  }
    
  node = map_lattice_to_node(&lblattice,ind,grid);
  index = get_linear_index(ind[0],ind[1],ind[2],lblattice.halo_grid);

  /* transform to lattice units */
  rho  *= agrid*agrid*agrid;
  j[0] *= tau/agrid;
  j[1] *= tau/agrid;
  j[2] *= tau/agrid;

  pi[0] = rho*lbmodel.c_sound_sq + j[0]*j[0]/rho;
  pi[2] = rho*lbmodel.c_sound_sq + j[1]*j[1]/rho;
  pi[5] = rho*lbmodel.c_sound_sq + j[2]*j[2]/rho;
  pi[1] = j[0]*j[1]/rho;
  pi[3] = j[0]*j[2]/rho;
  pi[4] = j[1]*j[2]/rho;

  mpi_send_fluid(node,index,rho,j,pi) ;

  lb_calc_average_rho();
  lb_reinit_parameters();

  return TCL_OK ;

}

static int lb_print_local_fields(Tcl_Interp *interp, int argc, char **argv, int *change, int *ind) {

  char buffer[256+4*TCL_DOUBLE_SPACE+3*TCL_INTEGER_SPACE];
  int index, node, grid[3];
  double rho, j[3], pi[6];

  *change = 0;

  sprintf(buffer, "%d", ind[0]) ;
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  sprintf(buffer, "%d", ind[1]) ;
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  sprintf(buffer, "%d", ind[2]) ;
  Tcl_AppendResult(interp, buffer, (char *)NULL);

  node = map_lattice_to_node(&lblattice,ind,grid);
  index = get_linear_index(ind[0],ind[1],ind[2],lblattice.halo_grid);
  
  mpi_recv_fluid(node,index,&rho,j,pi) ;

  /* transform to MD units */
  rho  *= 1./(agrid*agrid*agrid);
  j[0] *= agrid/tau;
  j[1] *= agrid/tau;
  j[2] *= agrid/tau;

  Tcl_PrintDouble(interp, rho, buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  Tcl_PrintDouble(interp, j[0], buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  Tcl_PrintDouble(interp, j[1], buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  Tcl_PrintDouble(interp, j[2], buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
    
  return TCL_OK ;

}

static int lbfluid_parse_tau(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double tau;

    if (argc < 1) {
	Tcl_AppendResult(interp, "tau requires 1 argument", NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(tau)) {
	Tcl_AppendResult(interp, "wrong  argument for tau", (char *)NULL);
	return TCL_ERROR;
    }
    if (tau < 0.0) {
	Tcl_AppendResult(interp, "tau must be positive", (char *)NULL);
	return TCL_ERROR;
    }
    else if ((time_step >= 0.0) && (tau < time_step)) {
      Tcl_AppendResult(interp, "tau must be larger than MD time_step", (char *)NULL);
      return TCL_ERROR;
    }

    *change = 1;
    lbpar.tau = tau;

    mpi_bcast_lb_params(LBPAR_TAU);

    return TCL_OK;
}

static int lbfluid_parse_agrid(Tcl_Interp *interp, int argc, char *argv[], int *change) {

    if (argc < 1) {
	Tcl_AppendResult(interp, "agrid requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(agrid)) {
	Tcl_AppendResult(interp, "wrong argument for agrid", (char *)NULL);
	return TCL_ERROR;
    }
    if (agrid <= 0.0) {
	Tcl_AppendResult(interp, "agrid must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lbpar.agrid = agrid;

    mpi_bcast_lb_params(LBPAR_AGRID);
 
    return TCL_OK;
}

static int lbfluid_parse_density(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double density;

    if (argc < 1) {
	Tcl_AppendResult(interp, "density requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(density)) {
	Tcl_AppendResult(interp, "wrong argument for density", (char *)NULL);
	return TCL_ERROR;
    }
    if (density <= 0.0) {
	Tcl_AppendResult(interp, "density must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lbpar.rho = density;

    mpi_bcast_lb_params(LBPAR_DENSITY);
 
    return TCL_OK;
}

static int lbfluid_parse_viscosity(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double viscosity;

    if (argc < 1) {
	Tcl_AppendResult(interp, "viscosity requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(viscosity)) {
	Tcl_AppendResult(interp, "wrong argument for viscosity", (char *)NULL);
	return TCL_ERROR;
    }
    if (viscosity <= 0.0) {
	Tcl_AppendResult(interp, "viscosity must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lbpar.viscosity = viscosity;

    mpi_bcast_lb_params(LBPAR_VISCOSITY);
 
    return TCL_OK;
}

static int lbfluid_parse_friction(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double friction;

    if (argc < 1) {
	Tcl_AppendResult(interp, "friction requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(friction)) {
	Tcl_AppendResult(interp, "wrong argument for friction", (char *)NULL);
	return TCL_ERROR;
    }
    if (friction <= 0.0) {
	Tcl_AppendResult(interp, "friction must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lbpar.friction = friction;

    mpi_bcast_lb_params(LBPAR_FRICTION);
 
    return TCL_OK;
}

static int lbfluid_parse_ext_force(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double ext_f[3];
    if (argc < 3) {
	Tcl_AppendResult(interp, "ext_force requires 3 arguments", (char *)NULL);
	return TCL_ERROR;
    }
    else {
 	if (!ARG_IS_D(0, ext_f[0])) return TCL_ERROR;
	if (!ARG_IS_D(1, ext_f[1])) return TCL_ERROR;
	if (!ARG_IS_D(2, ext_f[2])) return TCL_ERROR;
    }
    
    *change = 3;
    lbpar.ext_force[0] = ext_f[0];
    lbpar.ext_force[1] = ext_f[1];
    lbpar.ext_force[2] = ext_f[2];
    
    mpi_bcast_lb_params(LBPAR_EXTFORCE);
 
    return TCL_OK;
}
#endif /* LB */

/** Parser for the \ref lbfluid command. */
int lbfluid_cmd(ClientData data, Tcl_Interp *interp, int argc, char **argv) {
#ifdef LB
  int err = TCL_OK;
  int change = 0;
  
  argc--; argv++;

  if (argc < 1) {
      Tcl_AppendResult(interp, "too few arguments to \"lbfluid\"", (char *)NULL);
      err = TCL_ERROR;
  }
  else if (ARG0_IS_S("off")) {
    err = TCL_ERROR;
  }
  else if (ARG0_IS_S("init")) {
    err = TCL_ERROR;
  }
  else while (argc > 0) {
      if (ARG0_IS_S("grid") || ARG0_IS_S("agrid"))
	  err = lbfluid_parse_agrid(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("tau"))
	  err = lbfluid_parse_tau(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("density"))
	  err = lbfluid_parse_density(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("viscosity"))
	  err = lbfluid_parse_viscosity(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("friction") || ARG0_IS_S("coupling"))
	  err = lbfluid_parse_friction(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("ext_force"))
	  err = lbfluid_parse_ext_force(interp, argc-1, argv+1, &change);
      else {
	  Tcl_AppendResult(interp, "unknown feature \"", argv[0],"\" of lbfluid", (char *)NULL);
	  err = TCL_ERROR ;
      }

      if ((err = mpi_gather_runtime_errors(interp, err))) break;

      argc -= (change + 1);
      argv += (change + 1);
  }

  lattice_switch = (lattice_switch | LATTICE_LB) ;
  mpi_bcast_parameter(FIELD_LATTICE_SWITCH) ;

  /* thermo_switch is retained for backwards compatibility */
  thermo_switch = (thermo_switch | THERMO_LB);
  mpi_bcast_parameter(FIELD_THERMO_SWITCH);

  return err;    
#else /* !defined LB */
  Tcl_AppendResult(interp, "LB is not compiled in!", NULL);
  return TCL_ERROR;
#endif
}

/*@}*/
