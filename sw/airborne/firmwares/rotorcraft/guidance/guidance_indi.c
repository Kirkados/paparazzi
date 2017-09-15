/*
 * Copyright (C) 2015 Ewoud Smeur <ewoud.smeur@gmail.com>
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * @file firmwares/rotorcraft/guidance/guidance_indi.c
 *
 * A guidance mode based on Incremental Nonlinear Dynamic Inversion
 * Come to IROS2016 to learn more!
 *
 */

#include "generated/airframe.h"
#include "firmwares/rotorcraft/guidance/guidance_indi.h"
#include "subsystems/ins/ins_int.h"
#include "subsystems/radio_control.h"
#include "state.h"
#include "subsystems/imu.h"
#include "firmwares/rotorcraft/guidance/guidance_h.h"
#include "firmwares/rotorcraft/guidance/guidance_v.h"
#include "firmwares/rotorcraft/stabilization/stabilization_attitude.h"
#include "firmwares/rotorcraft/autopilot_rc_helpers.h"
#include "mcu_periph/sys_time.h"
#include "autopilot.h"
#include "stabilization/stabilization_attitude_ref_quat_int.h"
#include "firmwares/rotorcraft/stabilization.h"
#include "stdio.h"
#include "filters/low_pass_filter.h"
#include "filters/high_pass_filter.h"
#include "subsystems/abi.h"
#include "firmwares/rotorcraft/stabilization/stabilization_attitude_rc_setpoint.h"

// The acceleration reference is calculated with these gains. If you use GPS,
// they are probably limited by the update rate of your GPS. The default
// values are tuned for 4 Hz GPS updates. If you have high speed position updates, the
// gains can be higher, depending on the speed of the inner loop.
#ifdef GUIDANCE_INDI_POS_GAIN
float guidance_indi_pos_gain = GUIDANCE_INDI_POS_GAIN;
float guidance_indi_pos_gainz = GUIDANCE_INDI_POS_GAINZ;
#else
float guidance_indi_pos_gain = 0.5;
float guidance_indi_pos_gainz = 0.5;
#endif

#ifdef GUIDANCE_INDI_SPEED_GAIN
float guidance_indi_speed_gain = GUIDANCE_INDI_SPEED_GAIN;
float guidance_indi_speed_gainz = GUIDANCE_INDI_SPEED_GAINZ;
#else
float guidance_indi_speed_gain = 1.8;
float guidance_indi_speed_gainz = 1.8;
#endif

struct FloatVect3 sp_accel = {0.0,0.0,0.0};
#ifdef GUIDANCE_INDI_SPECIFIC_FORCE_GAIN
float thrust_in_specific_force_gain = GUIDANCE_INDI_SPECIFIC_FORCE_GAIN;
static void guidance_indi_filter_thrust(void);

#ifndef GUIDANCE_INDI_THRUST_DYNAMICS
#ifndef STABILIZATION_INDI_ACT_DYN_P
#error "You need to define GUIDANCE_INDI_THRUST_DYNAMICS to be able to use indi vertical control"
#else // assume that the same actuators are used for thrust as for roll (e.g. quadrotor)
#define GUIDANCE_INDI_THRUST_DYNAMICS STABILIZATION_INDI_ACT_DYN_P
#endif
#endif //GUIDANCE_INDI_THRUST_DYNAMICS

#endif //GUIDANCE_INDI_SPECIFIC_FORCE_GAIN

#ifndef GUIDANCE_INDI_FILTER_CUTOFF
#ifdef STABILIZATION_INDI_FILT_CUTOFF
#define GUIDANCE_INDI_FILTER_CUTOFF STABILIZATION_INDI_FILT_CUTOFF
#else
#define GUIDANCE_INDI_FILTER_CUTOFF 3.0
#endif
#endif

#ifndef GUIDANCE_INDI_MAX_AIRSPEED
#error "You must have an airspeed sensor to use this guidance"
#endif
float guidance_indi_max_airspeed = GUIDANCE_INDI_MAX_AIRSPEED;

float inv_eff[4];

float lift_pitch_eff = GUIDANCE_INDI_PITCH_LIFT_EFF;

/** state eulers in zxy order */
struct FloatEulers eulers_zxy;

float thrust_act = 0;
Butterworth2LowPass filt_accel_ned[3];
Butterworth2LowPass roll_filt;
Butterworth2LowPass pitch_filt;
Butterworth2LowPass thrust_filt;

struct FloatVect2 desired_airspeed;

struct FloatMat33 Ga;
struct FloatMat33 Ga_inv;
struct FloatVect3 euler_cmd;

float filter_cutoff = GUIDANCE_INDI_FILTER_CUTOFF;

struct FloatEulers guidance_euler_cmd;
float thrust_in;

struct FloatVect3 speed_sp = {0.0, 0.0, 0.0};

static void guidance_indi_propagate_filters(void);
static void guidance_indi_calcg_wing(struct FloatMat33 *Gmat);
static float guidance_indi_get_liftd(float pitch, float theta);

int16_t update_hp_freq_and_reset = 0;

struct FourthOrderHighPass flap_accel_hp;
double coef_b1[4] = {0.995201365263607,         -3.98080546105443,          5.97120819158164,         -3.98080546105443}; //0.3 Hz
double coef_a1[4] = {-3.99037963870238,          5.97118516477772,         -3.97123128331507,         0.990425757422548};
double coef_b2[4] = {0.992015065636079,         -3.96806026254432,          5.95209039381647,         -3.96806026254432}; //0.5 Hz
double coef_a2[4] = {-3.98396607231580,          5.95202663534277,         -3.95215445206974,         0.984093890448954};
double coef_b3[4] = {0.984093803447988,         -3.93637521379195,          5.90456282068793,         -3.93637521379195}; //1 Hz
double coef_a3[4] = {-3.96793221786345,          5.90430982475928,         -3.90481819856036,         0.968440613984728};
double coef_b4[4] = {0.968439929009413,         -3.87375971603765,          5.81063957405648,         -3.87375971603765}; //2 Hz
double coef_a4[4] = {-3.93586502144546,          5.80964371172319,          -3.8116542348822,         0.937875896099758};

/**
 *
 * Call upon entering indi guidance
 */
void guidance_indi_enter(void) {
  thrust_in = 0.0;
  thrust_act = 0;

  float tau = 1.0/(2.0*M_PI*filter_cutoff);
  float sample_time = 1.0/PERIODIC_FREQUENCY;
  for(int8_t i=0; i<3; i++) {
    init_butterworth_2_low_pass(&filt_accel_ned[i], tau, sample_time, 0.0);
  }
  init_butterworth_2_low_pass(&roll_filt, tau, sample_time, 0.0);
  init_butterworth_2_low_pass(&pitch_filt, tau, sample_time, 0.0);
  init_butterworth_2_low_pass(&thrust_filt, tau, sample_time, 0.0);

  init_fourth_order_high_pass(&flap_accel_hp, coef_a2, coef_b2, 0);
}

#include "firmwares/rotorcraft/navigation.h"
/**
 * @param in_flight in flight boolean
 * @param heading_sp the desired heading [rad]
 *
 * main indi guidance function
 */
void guidance_indi_run(bool UNUSED in_flight, float *heading_sp) {

  /*Obtain eulers with zxy rotation order*/
  float_eulers_of_quat_zxy(&eulers_zxy, stateGetNedToBodyQuat_f());

  /*Calculate the transition percentage so that the ctrl_effecitveness scheduling works*/
  transition_percentage = BFP_OF_REAL((eulers_zxy.theta/RadOfDeg(-75.0))*100,INT32_PERCENTAGE_FRAC);
  Bound(transition_percentage,0,BFP_OF_REAL(100.0,INT32_PERCENTAGE_FRAC));
  const int32_t max_offset = ANGLE_BFP_OF_REAL(TRANSITION_MAX_OFFSET);
  transition_theta_offset = INT_MULT_RSHIFT((transition_percentage <<
        (INT32_ANGLE_FRAC - INT32_PERCENTAGE_FRAC)) / 100, max_offset, INT32_ANGLE_FRAC);

  //filter accel to get rid of noise and filter attitude to synchronize with accel
  guidance_indi_propagate_filters();

  //Linear controller to find the acceleration setpoint from position and velocity
  float pos_x_err = POS_FLOAT_OF_BFP(guidance_h.ref.pos.x) - stateGetPositionNed_f()->x;
  float pos_y_err = POS_FLOAT_OF_BFP(guidance_h.ref.pos.y) - stateGetPositionNed_f()->y;
  float pos_z_err = POS_FLOAT_OF_BFP(guidance_v_z_ref - stateGetPositionNed_i()->z);

  if(autopilot.mode == AP_MODE_NAV) {
    speed_sp = nav_get_speed_setpoint();
  } else{
    speed_sp.x = pos_x_err * guidance_indi_pos_gain;
    speed_sp.y = pos_y_err * guidance_indi_pos_gain;
    speed_sp.z = pos_z_err * guidance_indi_pos_gainz;
  }

  //for rc control horizontal, rotate from body axes to NED
  float psi = eulers_zxy.psi;
  /*NAV mode*/
  float speed_sp_b_x = cosf(psi) * speed_sp.x + sinf(psi) * speed_sp.y;
  float speed_sp_b_y =-sinf(psi) * speed_sp.x + cosf(psi) * speed_sp.y;

  float airspeed = stateGetAirspeed_f();

  struct NedCoor_f *groundspeed = stateGetSpeedNed_f();
  struct FloatVect2 airspeed_v = {cos(psi)*airspeed, sin(psi)*airspeed};
  struct FloatVect2 windspeed;
  VECT2_DIFF(windspeed, *groundspeed, airspeed_v);

  VECT2_DIFF(desired_airspeed, speed_sp, windspeed); // Use 2d part of speed_sp
  float norm_des_as = FLOAT_VECT2_NORM(desired_airspeed);

  // Make turn instead of straight line
  if((airspeed > 10.0) && (norm_des_as > 12.0)) {

  // Give the wind cancellation priority.
    if (norm_des_as > guidance_indi_max_airspeed) {
      float groundspeed_factor = 0.0;

      // if the wind is faster than we can fly, just fly in the wind direction
      if(FLOAT_VECT2_NORM(windspeed) < guidance_indi_max_airspeed) {
        float av = speed_sp.x * speed_sp.x + speed_sp.y * speed_sp.y;
        float bv = -2 * (windspeed.x * speed_sp.x + windspeed.y * speed_sp.y);
        float cv = windspeed.x * windspeed.x + windspeed.y * windspeed.y - guidance_indi_max_airspeed * guidance_indi_max_airspeed;

        float dv = bv * bv - 4.0 * av * cv;

        // dv can only be positive, but just in case
        if(dv < 0) {
          dv = fabs(dv);
        }
        float d_sqrt = sqrtf(dv);

        groundspeed_factor = (-bv + d_sqrt)  / (2 * av);
      }

      desired_airspeed.x = groundspeed_factor * speed_sp.x - windspeed.x;
      desired_airspeed.y = groundspeed_factor * speed_sp.y - windspeed.y;

      speed_sp_b_x = guidance_indi_max_airspeed;
    }

    // desired airspeed can not be larger than max airspeed
    speed_sp_b_x = Min(norm_des_as,guidance_indi_max_airspeed);

    // Calculate accel sp in body axes, because we need to regulate airspeed
    struct FloatVect2 sp_accel_b;
    // In turn acceleration proportional to heading diff
    sp_accel_b.y = atan2(desired_airspeed.y, desired_airspeed.x) - psi;
    FLOAT_ANGLE_NORMALIZE(sp_accel_b.y);
    sp_accel_b.y *= 15.0;

    // Control the airspeed
    sp_accel_b.x = (speed_sp_b_x - airspeed) * guidance_indi_speed_gain;

    sp_accel.x = cosf(psi) * sp_accel_b.x - sinf(psi) * sp_accel_b.y;
    sp_accel.y = sinf(psi) * sp_accel_b.x + cosf(psi) * sp_accel_b.y;

    sp_accel.z = (speed_sp.z - stateGetSpeedNed_f()->z) * guidance_indi_speed_gainz;
  } else { // Go somewhere in the shortest way

    if(airspeed > 10.0) {
      // Groundspeed vector in body frame
      float groundspeed_x = cosf(psi) * stateGetSpeedNed_f()->x + sinf(psi) * stateGetSpeedNed_f()->y;
      float speed_increment = speed_sp_b_x - groundspeed_x;

      // limit groundspeed setpoint to max_airspeed + (diff gs and airspeed)
      if((speed_increment + airspeed) > guidance_indi_max_airspeed) {
        speed_sp_b_x = guidance_indi_max_airspeed + groundspeed_x - airspeed;
      }
    }
    speed_sp.x = cosf(psi) * speed_sp_b_x - sinf(psi) * speed_sp_b_y;
    speed_sp.y = sinf(psi) * speed_sp_b_x + cosf(psi) * speed_sp_b_y;

    sp_accel.x = (speed_sp.x - stateGetSpeedNed_f()->x) * guidance_indi_speed_gain;
    sp_accel.y = (speed_sp.y - stateGetSpeedNed_f()->y) * guidance_indi_speed_gain;
    sp_accel.z = (speed_sp.z - stateGetSpeedNed_f()->z) * guidance_indi_speed_gainz;
  }

  // Bound the acceleration setpoint
  float accelbound = 3.0 + airspeed/guidance_indi_max_airspeed*5.0;
  scale_two_d(&sp_accel, accelbound);
  /*BoundAbs(sp_accel.x, 3.0 + airspeed/guidance_indi_max_airspeed*6.0);*/
  /*BoundAbs(sp_accel.y, 3.0 + airspeed/guidance_indi_max_airspeed*6.0);*/
  BoundAbs(sp_accel.z, 3.0);

#if GUIDANCE_INDI_RC_DEBUG
#warning "GUIDANCE_INDI_RC_DEBUG lets you control the accelerations via RC, but disables autonomous flight!"
  //for rc control horizontal, rotate from body axes to NED
  float psi = eulers_zxy.psi;
  float rc_x = -(radio_control.values[RADIO_PITCH]/9600.0)*8.0;
  float rc_y = (radio_control.values[RADIO_ROLL]/9600.0)*8.0;
  sp_accel.x = cosf(psi) * rc_x - sinf(psi) * rc_y;
  sp_accel.y = sinf(psi) * rc_x + cosf(psi) * rc_y;

  //for rc vertical control
  sp_accel.z = -(radio_control.values[RADIO_THROTTLE]-4500)*8.0/9600.0;
#endif

  //Calculate matrix of partial derivatives
  guidance_indi_calcg_wing(&Ga);
  //Invert this matrix
  MAT33_INV(Ga_inv, Ga);

  // Calculate acceleration compensated for flap-lift effect
  /*accel_ned_comp = filt_accel_ned - rot_to_ned*flap_lift_eff*flap_deflection;*/
  float flap_effectiveness;
  if(airspeed < 8) {
    float pitch_interp = DegOfRad(eulers_zxy.theta);
    Bound(pitch_interp, -70.0, -20.0);
    float ratio = (pitch_interp + 20.0)/(-50.);
    flap_effectiveness = FE_LIFT_A_PITCH + FE_LIFT_B_PITCH*ratio;
  } else {
    flap_effectiveness = FE_LIFT_A_AS + (airspeed - 8.0)*FE_LIFT_B_AS;
  }
  double flap_deflection = -actuator_state_filt_vect[0] + actuator_state_filt_vect[1];

  if(update_hp_freq_and_reset > 0) {
    double *coef_a;
    double *coef_b;
    switch(update_hp_freq_and_reset) {
      case 1:
        coef_b = coef_b1;
        coef_a = coef_a1;
        break;
      case 2:
        coef_b = coef_b2;
        coef_a = coef_a2;
        break;
      case 3:
        coef_b = coef_b3;
        coef_a = coef_a3;
        break;
      case 4:
        coef_b = coef_b4;
        coef_a = coef_a4;
        break;
      default:
        coef_b = coef_b2;
        coef_a = coef_a2;
        break;
    }
    init_fourth_order_high_pass(&flap_accel_hp, coef_a, coef_b, 0);
    update_hp_freq_and_reset = 0;
  }

  // propagate high pass filter, because we don't want steady state offsets in the acceleration
  update_fourth_order_high_pass(&flap_accel_hp, flap_deflection);

  float flap_accel_body_x = (float) flap_accel_hp.o[0] * flap_effectiveness;

  float accel_x, accel_y, accel_z;
  if(radio_control.values[5] > 0) {
    struct FloatRMat rot_mat;
    float_rmat_of_quat(&rot_mat, stateGetNedToBodyQuat_f());
    accel_x = filt_accel_ned[0].o[0] - RMAT_ELMT(rot_mat, 0,0) * flap_accel_body_x;
    accel_y = filt_accel_ned[1].o[0] - RMAT_ELMT(rot_mat, 0,1) * flap_accel_body_x;
    accel_z = filt_accel_ned[2].o[0] - RMAT_ELMT(rot_mat, 0,2) * flap_accel_body_x;

/*or: cosf(psi)cosf(theta)-sinf(theta)sinf(psi)sinf(phi), sinf(psi)cosf(theta)+sinf(theta)sinf(phi)cosf(psi), -sinf(theta)cosf(phi)*/
    /*accel_x = filt_accel_ned[0].o[0] - (cosf(eulers_zxy.psi) - sinf(eulers_zxy.psi)) * (float) flap_accel_hp.o[0];*/
    /*accel_y = filt_accel_ned[1].o[0] - (sinf(eulers_zxy.psi) + cosf(eulers_zxy.psi)) * (float) flap_accel_hp.o[0];*/
  } else {
    accel_x = filt_accel_ned[0].o[0];
    accel_y = filt_accel_ned[1].o[0];
    accel_z = filt_accel_ned[2].o[0];
  }

  struct FloatVect3 a_diff = { sp_accel.x - accel_x, sp_accel.y - accel_y, sp_accel.z - accel_z};

  //Bound the acceleration error so that the linearization still holds
  Bound(a_diff.x, -6.0, 6.0);
  Bound(a_diff.y, -6.0, 6.0);
  Bound(a_diff.z, -9.0, 9.0);

  //If the thrust to specific force ratio has been defined, include vertical control
  //else ignore the vertical acceleration error
#ifndef GUIDANCE_INDI_SPECIFIC_FORCE_GAIN
#ifndef STABILIZATION_ATTITUDE_INDI_FULL
  a_diff.z = 0.0;
#endif
#endif

  //Calculate roll,pitch and thrust command
  MAT33_VECT3_MUL(euler_cmd, Ga_inv, a_diff);

  AbiSendMsgTHRUST(THRUST_INCREMENT_ID, euler_cmd.z);

  //Coordinated turn
  //feedforward estimate angular rotation omega = g*tan(phi)/v
  float omega;
  const float max_phi = RadOfDeg(60.0);
  float airspeed_turn = airspeed;
  //We are dividing by the airspeed, so a lower bound is important
  Bound(airspeed_turn,10.0,30.0);

  guidance_euler_cmd.phi = roll_filt.o[0] + euler_cmd.x;
  guidance_euler_cmd.theta = pitch_filt.o[0] + euler_cmd.y;

  //Bound euler angles to prevent flipping
  Bound(guidance_euler_cmd.phi, -GUIDANCE_H_MAX_BANK, GUIDANCE_H_MAX_BANK);
  Bound(guidance_euler_cmd.theta, -RadOfDeg(120.0), RadOfDeg(25.0));


  float coordinated_turn_roll = guidance_euler_cmd.phi;

  if( (guidance_euler_cmd.theta > 0.0) && ( fabs(guidance_euler_cmd.phi) < guidance_euler_cmd.theta)) {
    coordinated_turn_roll = ((guidance_euler_cmd.phi > 0.0) - (guidance_euler_cmd.phi < 0.0))*guidance_euler_cmd.theta;
  }

  if (fabs(coordinated_turn_roll) < max_phi) {
    omega = 9.81 / airspeed_turn * tanf(coordinated_turn_roll);
  } else { //max 60 degrees roll
    omega = 9.81 / airspeed_turn * 1.72305 * ((coordinated_turn_roll > 0.0) - (coordinated_turn_roll < 0.0));
  }

#ifdef FWD_SIDESLIP_GAIN
  // Add sideslip correction
  omega -= accely_filt.o[0]*FWD_SIDESLIP_GAIN;
#endif

#ifndef KNIFE_EDGE_TEST
  *heading_sp += omega / PERIODIC_FREQUENCY;
  FLOAT_ANGLE_NORMALIZE(*heading_sp);
#endif

  guidance_euler_cmd.psi = *heading_sp;

#ifdef GUIDANCE_INDI_SPECIFIC_FORCE_GAIN
  guidance_indi_filter_thrust();

  //Add the increment in specific force * specific_force_to_thrust_gain to the filtered thrust
  thrust_in = thrust_filt.o[0] + euler_cmd.z*thrust_in_specific_force_gain;
  Bound(thrust_in, 0, 9600);

#if GUIDANCE_INDI_RC_DEBUG
  if(radio_control.values[RADIO_THROTTLE]<300) {
    thrust_in = 0;
  }
#endif

  //Overwrite the thrust command from guidance_v
  stabilization_cmd[COMMAND_THRUST] = thrust_in;
#endif

  // Set the quaternion setpoint from eulers_zxy
  struct FloatQuat sp_quat;
  float_quat_of_eulers_zxy(&sp_quat, &guidance_euler_cmd);
  float_quat_normalize(&sp_quat);
  QUAT_BFP_OF_REAL(stab_att_sp_quat,sp_quat);
}

#ifdef GUIDANCE_INDI_SPECIFIC_FORCE_GAIN
/**
 * Filter the thrust, such that it corresponds to the filtered acceleration
 */
void guidance_indi_filter_thrust(void)
{
  // Actuator dynamics
  thrust_act = thrust_act + GUIDANCE_INDI_THRUST_DYNAMICS * (thrust_in - thrust_act);

  // same filter as for the acceleration
  update_butterworth_2_low_pass(&thrust_filt, thrust_act);
}
#endif

/**
 * Low pass the accelerometer measurements to remove noise from vibrations.
 * The roll and pitch also need to be filtered to synchronize them with the
 * acceleration
 */
void guidance_indi_propagate_filters(void) {
  struct NedCoor_f *accel = stateGetAccelNed_f();
  update_butterworth_2_low_pass(&filt_accel_ned[0], accel->x);
  update_butterworth_2_low_pass(&filt_accel_ned[1], accel->y);
  update_butterworth_2_low_pass(&filt_accel_ned[2], accel->z);

  update_butterworth_2_low_pass(&roll_filt, eulers_zxy.phi);
  update_butterworth_2_low_pass(&pitch_filt, eulers_zxy.theta);
}

/**
 * Calculate the matrix of partial derivatives of the roll, pitch and thrust
 * w.r.t. the NED accelerations, taking into account the lift of a wing that is
 * horizontal at -90 degrees pitch
 *
 * @param Gmat array to write the matrix to [3x3]
 */
void guidance_indi_calcg_wing(struct FloatMat33 *Gmat) {

  /*Pre-calculate sines and cosines*/
  float sphi = sinf(eulers_zxy.phi);
  float cphi = cosf(eulers_zxy.phi);
  float stheta = sinf(eulers_zxy.theta);
  float ctheta = cosf(eulers_zxy.theta);
  float spsi = sinf(eulers_zxy.psi);
  float cpsi = cosf(eulers_zxy.psi);
  //minus gravity is a guesstimate of the thrust force, thrust measurement would be better

#ifndef GUIDANCE_INDI_PITCH_EFF_SCALING
#define GUIDANCE_INDI_PITCH_EFF_SCALING 1.0
#endif

  /*Amount of lift produced by the wing*/
  float pitch_lift = eulers_zxy.theta;
  Bound(pitch_lift,-M_PI_2,0);
  float lift = sinf(pitch_lift)*9.81;
  float T = cosf(pitch_lift)*-9.81;

  // get the derivative of the lift wrt to theta
  float liftd = guidance_indi_get_liftd(stateGetAirspeed_f(), eulers_zxy.theta);

  RMAT_ELMT(*Gmat, 0, 0) =  cphi*ctheta*spsi*T + cphi*spsi*lift;
  RMAT_ELMT(*Gmat, 1, 0) = -cphi*ctheta*cpsi*T - cphi*cpsi*lift;
  RMAT_ELMT(*Gmat, 2, 0) = -sphi*ctheta*T -sphi*lift;
  RMAT_ELMT(*Gmat, 0, 1) = (ctheta*cpsi - sphi*stheta*spsi)*T*GUIDANCE_INDI_PITCH_EFF_SCALING + sphi*spsi*liftd;
  RMAT_ELMT(*Gmat, 1, 1) = (ctheta*spsi + sphi*stheta*cpsi)*T*GUIDANCE_INDI_PITCH_EFF_SCALING - sphi*cpsi*liftd;
  RMAT_ELMT(*Gmat, 2, 1) = -cphi*stheta*T*GUIDANCE_INDI_PITCH_EFF_SCALING + cphi*liftd;
  RMAT_ELMT(*Gmat, 0, 2) = stheta*cpsi + sphi*ctheta*spsi;
  RMAT_ELMT(*Gmat, 1, 2) = stheta*spsi - sphi*ctheta*cpsi;
  RMAT_ELMT(*Gmat, 2, 2) = cphi*ctheta;
}

/**
 * @brief Get the derivative of lift w.r.t. pitch.
 *
 * @param airspeed The airspeed says most about the flight condition
 *
 * @return The derivative of lift w.r.t. pitch
 */
float guidance_indi_get_liftd(float airspeed, float theta) {
  float liftd = 0.0;
  if(airspeed < 12) {
    float pitch_interp = DegOfRad(theta);
    Bound(pitch_interp, -80.0, -40.0);
    float ratio = (pitch_interp + 40.0)/(-40.);
    liftd = -24.0*ratio;
  } else {
    liftd = -(airspeed - 8.5)*lift_pitch_eff/M_PI*180.0;
  }
  return liftd;
}

