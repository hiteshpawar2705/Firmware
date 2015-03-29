/****************************************************************************
 *
 *   Copyright (c) 2012-2014 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/*
 * @file attitude_estimator_ekf_main.cpp
 *
 * Extended Kalman Filter for Attitude Estimation.
 *
 * @author Tobias Naegeli <naegelit@student.ethz.ch>
 * @author Lorenz Meier <lm@inf.ethz.ch>
 * @author Thomas Gubler <thomasgubler@gmail.com>
 */

#include <px4_config.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <poll.h>
#include <fcntl.h>
#include <float.h>
#include <sys/prctl.h>
#include <termios.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <uORB/uORB.h>
#include <uORB/topics/debug_key_value.h>
#include <uORB/topics/sensor_combined.h>
#include <uORB/topics/vehicle_attitude.h>
#include <uORB/topics/vehicle_control_mode.h>
#include <uORB/topics/vehicle_gps_position.h>
#include <uORB/topics/vehicle_global_position.h>
#include <uORB/topics/parameter_update.h>
#include <uORB/topics/vision_position_estimate.h>
#include <drivers/drv_hrt.h>

#include <lib/mathlib/mathlib.h>
#include <lib/geo/geo.h>

#include <systemlib/systemlib.h>
#include <systemlib/perf_counter.h>
#include <systemlib/err.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "codegen/AttitudeEKF.h"
#include "attitude_estimator_ekf_params.h"
#ifdef __cplusplus
}
#endif

extern "C" __EXPORT int attitude_estimator_ekf_main(int argc, char *argv[]);

static bool thread_should_exit = false;		/**< Deamon exit flag */
static bool thread_running = false;		/**< Deamon status flag */
static int attitude_estimator_ekf_task;				/**< Handle of deamon task / thread */

/**
 * Mainloop of attitude_estimator_ekf.
 */
int attitude_estimator_ekf_thread_main(int argc, char *argv[]);

/**
 * Print the correct usage.
 */
static void usage(const char *reason);

static void
usage(const char *reason)
{
	if (reason) {
		fprintf(stderr, "%s\n", reason);
	}

	fprintf(stderr, "usage: attitude_estimator_ekf {start|stop|status} [-p <additional params>]\n\n");
}

/**
 * The attitude_estimator_ekf app only briefly exists to start
 * the background job. The stack size assigned in the
 * Makefile does only apply to this management task.
 *
 * The actual stack size should be set in the call
 * to task_spawn_cmd().
 */
int attitude_estimator_ekf_main(int argc, char *argv[])
{
	if (argc < 2) {
		usage("missing command");
		return 1;
	}

	if (!strcmp(argv[1], "start")) {

		if (thread_running) {
			warnx("already running\n");
			/* this is not an error */
			exit(0);
		}

		thread_should_exit = false;
		attitude_estimator_ekf_task = task_spawn_cmd("attitude_estimator_ekf",
					      SCHED_DEFAULT,
					      SCHED_PRIORITY_MAX - 5,
					      7700,
					      attitude_estimator_ekf_thread_main,
					      (argv) ? (char * const *)&argv[2] : (char * const *)NULL);
		return 0;
	}

	if (!strcmp(argv[1], "stop")) {
		thread_should_exit = true;
		return 0;
	}

	if (!strcmp(argv[1], "status")) {
		if (thread_running) {
			warnx("running");
			return 0;

		} else {
			warnx("not started");
			return 1;
		}

		return 0;
	}

	usage("unrecognized command");
	return 1;
}

/*
 * [Rot_matrix,x_aposteriori,P_aposteriori] = attitudeKalmanfilter(dt,z_k,x_aposteriori_k,P_aposteriori_k,knownConst)
 */

/*
 * EKF Attitude Estimator main function.
 *
 * Estimates the attitude recursively once started.
 *
 * @param argc number of commandline arguments (plus command name)
 * @param argv strings containing the arguments
 */
int attitude_estimator_ekf_thread_main(int argc, char *argv[])
{

	float dt = 0.005f;
/* state vector x has the following entries [ax,ay,az||mx,my,mz||wox,woy,woz||wx,wy,wz]' */
	float z_k[9] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 9.81f, 0.2f, -0.2f, 0.2f};					/**< Measurement vector */
	float x_aposteriori_k[12];		/**< states */
	float P_aposteriori_k[144] = {100.f, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
				     0, 100.f,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
				     0,   0, 100.f,   0,   0,   0,   0,   0,   0,   0,   0,   0,
				     0,   0,   0, 100.f,   0,   0,   0,   0,   0,   0,   0,   0,
				     0,   0,   0,   0,  100.f,  0,   0,   0,   0,   0,   0,   0,
				     0,   0,   0,   0,   0, 100.f,   0,   0,   0,   0,   0,   0,
				     0,   0,   0,   0,   0,   0, 100.f,   0,   0,   0,   0,   0,
				     0,   0,   0,   0,   0,   0,   0, 100.f,   0,   0,   0,   0,
				     0,   0,   0,   0,   0,   0,   0,   0, 100.f,   0,   0,   0,
				     0,   0,   0,   0,   0,   0,   0,   0,  0.0f, 100.0f,   0,   0,
				     0,   0,   0,   0,   0,   0,   0,   0,  0.0f,   0,   100.0f,   0,
				     0,   0,   0,   0,   0,   0,   0,   0,  0.0f,   0,   0,   100.0f,
				    }; /**< init: diagonal matrix with big values */

	float x_aposteriori[12];
	float P_aposteriori[144];

	/* output euler angles */
	float euler[3] = {0.0f, 0.0f, 0.0f};

	float Rot_matrix[9] = {1.f,  0,  0,
			      0,  1.f,  0,
			      0,  0,  1.f
			     };		/**< init: identity matrix */

	float debugOutput[4] = { 0.0f };

	/* Initialize filter */
	AttitudeEKF_initialize();

	struct sensor_combined_s raw;
	memset(&raw, 0, sizeof(raw));
	struct vehicle_gps_position_s gps;
	memset(&gps, 0, sizeof(gps));
	gps.eph = 100000;
	gps.epv = 100000;
	struct vehicle_global_position_s global_pos;
	memset(&global_pos, 0, sizeof(global_pos));
	struct vehicle_attitude_s att;
	memset(&att, 0, sizeof(att));
	struct vehicle_control_mode_s control_mode;
	memset(&control_mode, 0, sizeof(control_mode));

	uint64_t last_data = 0;
	uint64_t last_measurement = 0;
	uint64_t last_vel_t = 0;

	/* current velocity */
	math::Vector<3> vel;
	vel.zero();
	/* previous velocity */
	math::Vector<3> vel_prev;
	vel_prev.zero();
	/* actual acceleration (by GPS velocity) in body frame */
	math::Vector<3> acc;
	acc.zero();
	/* rotation matrix */
	math::Matrix<3, 3> R;
	R.identity();

	/* subscribe to raw data */
	int sub_raw = orb_subscribe(ORB_ID(sensor_combined));
	/* rate-limit raw data updates to 333 Hz (sensors app publishes at 200, so this is just paranoid) */
	orb_set_interval(sub_raw, 3);

	/* subscribe to GPS */
	int sub_gps = orb_subscribe(ORB_ID(vehicle_gps_position));

	/* subscribe to GPS */
	int sub_global_pos = orb_subscribe(ORB_ID(vehicle_global_position));

	/* subscribe to param changes */
	int sub_params = orb_subscribe(ORB_ID(parameter_update));

	/* subscribe to control mode */
	int sub_control_mode = orb_subscribe(ORB_ID(vehicle_control_mode));

	/* subscribe to vision estimate */
	int vision_sub = orb_subscribe(ORB_ID(vision_position_estimate));

	/* advertise attitude */
	orb_advert_t pub_att = orb_advertise(ORB_ID(vehicle_attitude), &att);

	int loopcounter = 0;

	thread_running = true;

	/* keep track of sensor updates */
	uint64_t sensor_last_timestamp[3] = {0, 0, 0};

	struct attitude_estimator_ekf_params ekf_params;
	memset(&ekf_params, 0, sizeof(ekf_params));

	struct attitude_estimator_ekf_param_handles ekf_param_handles = { 0 };

	/* initialize parameter handles */
	parameters_init(&ekf_param_handles);

	bool initialized = false;

	float gyro_offsets[3] = { 0.0f, 0.0f, 0.0f };

	/* magnetic declination, in radians */
	float mag_decl = 0.0f;

	/* rotation matrix for magnetic declination */
	math::Matrix<3, 3> R_decl;
	R_decl.identity();

	struct vision_position_estimate vision {};

	/* register the perf counter */
	perf_counter_t ekf_loop_perf = perf_alloc(PC_ELAPSED, "attitude_estimator_ekf");

	/* Main loop*/
	while (!thread_should_exit) {

		struct pollfd fds[2];
		fds[0].fd = sub_raw;
		fds[0].events = POLLIN;
		fds[1].fd = sub_params;
		fds[1].events = POLLIN;
		int ret = poll(fds, 2, 1000);

		if (ret < 0) {
			/* XXX this is seriously bad - should be an emergency */
		} else if (ret == 0) {
			/* check if we're in HIL - not getting sensor data is fine then */
			orb_copy(ORB_ID(vehicle_control_mode), sub_control_mode, &control_mode);

			if (!control_mode.flag_system_hil_enabled) {
				warnx("WARNING: Not getting sensor data - sensor app running?");
			}

		} else {

			/* only update parameters if they changed */
			if (fds[1].revents & POLLIN) {
				/* read from param to clear updated flag */
				struct parameter_update_s update;
				orb_copy(ORB_ID(parameter_update), sub_params, &update);

				/* update parameters */
				parameters_update(&ekf_param_handles, &ekf_params);
			}

			/* only run filter if sensor values changed */
			if (fds[0].revents & POLLIN) {

				/* get latest measurements */
				orb_copy(ORB_ID(sensor_combined), sub_raw, &raw);

				bool gps_updated;
				orb_check(sub_gps, &gps_updated);
				if (gps_updated) {
					orb_copy(ORB_ID(vehicle_gps_position), sub_gps, &gps);

					if (gps.eph < 20.0f && hrt_elapsed_time(&gps.timestamp_position) < 1000000) {
						mag_decl = math::radians(get_mag_declination(gps.lat / 1e7f, gps.lon / 1e7f));

						/* update mag declination rotation matrix */
						R_decl.from_euler(0.0f, 0.0f, mag_decl);
					}
				}

				bool global_pos_updated;
				orb_check(sub_global_pos, &global_pos_updated);
				if (global_pos_updated) {
					orb_copy(ORB_ID(vehicle_global_position), sub_global_pos, &global_pos);
				}

				if (!initialized) {
					// XXX disabling init for now
					initialized = true;

					// gyro_offsets[0] += raw.gyro_rad_s[0];
					// gyro_offsets[1] += raw.gyro_rad_s[1];
					// gyro_offsets[2] += raw.gyro_rad_s[2];
					// offset_count++;

					// if (hrt_absolute_time() - start_time > 3000000LL) {
					// 	initialized = true;
					// 	gyro_offsets[0] /= offset_count;
					// 	gyro_offsets[1] /= offset_count;
					// 	gyro_offsets[2] /= offset_count;
					// }

				} else {

					perf_begin(ekf_loop_perf);

					/* Calculate data time difference in seconds */
					dt = (raw.timestamp - last_measurement) / 1000000.0f;
					last_measurement = raw.timestamp;
					uint8_t update_vect[3] = {0, 0, 0};

					/* Fill in gyro measurements */
					if (sensor_last_timestamp[0] != raw.timestamp) {
						update_vect[0] = 1;
						// sensor_update_hz[0] = 1e6f / (raw.timestamp - sensor_last_timestamp[0]);
						sensor_last_timestamp[0] = raw.timestamp;
					}

					z_k[0] =  raw.gyro_rad_s[0] - gyro_offsets[0];
					z_k[1] =  raw.gyro_rad_s[1] - gyro_offsets[1];
					z_k[2] =  raw.gyro_rad_s[2] - gyro_offsets[2];

					/* update accelerometer measurements */
					if (sensor_last_timestamp[1] != raw.accelerometer_timestamp) {
						update_vect[1] = 1;
						// sensor_update_hz[1] = 1e6f / (raw.timestamp - sensor_last_timestamp[1]);
						sensor_last_timestamp[1] = raw.accelerometer_timestamp;
					}

					hrt_abstime vel_t = 0;
					bool vel_valid = false;
					if (ekf_params.acc_comp == 1 && gps.fix_type >= 3 && gps.eph < 10.0f && gps.vel_ned_valid && hrt_absolute_time() < gps.timestamp_velocity + 500000) {
						vel_valid = true;
						if (gps_updated) {
							vel_t = gps.timestamp_velocity;
							vel(0) = gps.vel_n_m_s;
							vel(1) = gps.vel_e_m_s;
							vel(2) = gps.vel_d_m_s;
						}

					} else if (ekf_params.acc_comp == 2 && gps.eph < 5.0f && global_pos.timestamp != 0 && hrt_absolute_time() < global_pos.timestamp + 20000) {
						vel_valid = true;
						if (global_pos_updated) {
							vel_t = global_pos.timestamp;
							vel(0) = global_pos.vel_n;
							vel(1) = global_pos.vel_e;
							vel(2) = global_pos.vel_d;
						}
					}

					if (vel_valid) {
						/* velocity is valid */
						if (vel_t != 0) {
							/* velocity updated */
							if (last_vel_t != 0 && vel_t != last_vel_t) {
								float vel_dt = (vel_t - last_vel_t) / 1000000.0f;
								/* calculate acceleration in body frame */
								acc = R.transposed() * ((vel - vel_prev) / vel_dt);
							}
							last_vel_t = vel_t;
							vel_prev = vel;
						}

					} else {
						/* velocity is valid, reset acceleration */
						acc.zero();
						vel_prev.zero();
						last_vel_t = 0;
					}

					z_k[3] = raw.accelerometer_m_s2[0] - acc(0);
					z_k[4] = raw.accelerometer_m_s2[1] - acc(1);
					z_k[5] = raw.accelerometer_m_s2[2] - acc(2);

					/* update magnetometer measurements */
					if (sensor_last_timestamp[2] != raw.magnetometer_timestamp &&
						/* check that the mag vector is > 0 */
						fabsf(sqrtf(raw.magnetometer_ga[0] * raw.magnetometer_ga[0] +
							raw.magnetometer_ga[1] * raw.magnetometer_ga[1] +
							raw.magnetometer_ga[2] * raw.magnetometer_ga[2])) > 0.1f) {
						update_vect[2] = 1;
						// sensor_update_hz[2] = 1e6f / (raw.timestamp - sensor_last_timestamp[2]);
						sensor_last_timestamp[2] = raw.magnetometer_timestamp;
					}

					bool vision_updated = false;
					orb_check(vision_sub, &vision_updated);

					if (vision_updated) {
						orb_copy(ORB_ID(vision_position_estimate), vision_sub, &vision);
					}

					if (vision.timestamp_boot > 0 && (hrt_elapsed_time(&vision.timestamp_boot) < 500000)) {

						math::Quaternion q(vision.q);
						math::Matrix<3, 3> Rvis = q.to_dcm();

						math::Vector<3> v(1.0f, 0.0f, 0.4f);

						math::Vector<3> vn = Rvis * v;

						z_k[6] = vn(0);
						z_k[7] = vn(1);
						z_k[8] = vn(2);
					} else {
						z_k[6] = raw.magnetometer_ga[0];
						z_k[7] = raw.magnetometer_ga[1];
						z_k[8] = raw.magnetometer_ga[2];
					}

					static bool const_initialized = false;

					/* initialize with good values once we have a reasonable dt estimate */
					if (!const_initialized && dt < 0.05f && dt > 0.001f) {
						dt = 0.005f;
						parameters_update(&ekf_param_handles, &ekf_params);

						/* update mag declination rotation matrix */
						if (gps.eph < 20.0f && hrt_elapsed_time(&gps.timestamp_position) < 1000000) {
							mag_decl = math::radians(get_mag_declination(gps.lat / 1e7f, gps.lon / 1e7f));

						} else {
							mag_decl = ekf_params.mag_decl;
						}

						/* update mag declination rotation matrix */
						R_decl.from_euler(0.0f, 0.0f, mag_decl);

						x_aposteriori_k[0] = z_k[0];
						x_aposteriori_k[1] = z_k[1];
						x_aposteriori_k[2] = z_k[2];
						x_aposteriori_k[3] = 0.0f;
						x_aposteriori_k[4] = 0.0f;
						x_aposteriori_k[5] = 0.0f;
						x_aposteriori_k[6] = z_k[3];
						x_aposteriori_k[7] = z_k[4];
						x_aposteriori_k[8] = z_k[5];
						x_aposteriori_k[9] = z_k[6];
						x_aposteriori_k[10] = z_k[7];
						x_aposteriori_k[11] = z_k[8];

						const_initialized = true;
					}

					/* do not execute the filter if not initialized */
					if (!const_initialized) {
						continue;
					}

					/* Call the estimator */
					AttitudeEKF(false, // approx_prediction
							(unsigned char)ekf_params.use_moment_inertia,
							update_vect,
							dt,
							z_k,
							ekf_params.q[0], // q_rotSpeed,
							ekf_params.q[1], // q_rotAcc
							ekf_params.q[2], // q_acc
							ekf_params.q[3], // q_mag
							ekf_params.r[0], // r_gyro
							ekf_params.r[1], // r_accel
							ekf_params.r[2], // r_mag
							ekf_params.moment_inertia_J,
							x_aposteriori,
							P_aposteriori,
							Rot_matrix,
							euler,
							debugOutput);

					/* swap values for next iteration, check for fatal inputs */
					if (std::isfinite(euler[0]) && std::isfinite(euler[1]) && std::isfinite(euler[2])) {
						memcpy(P_aposteriori_k, P_aposteriori, sizeof(P_aposteriori_k));
						memcpy(x_aposteriori_k, x_aposteriori, sizeof(x_aposteriori_k));

					} else {
						/* due to inputs or numerical failure the output is invalid, skip it */
						continue;
					}

					if (last_data > 0 && raw.timestamp - last_data > 30000) {
<<<<<<< HEAD
						warnx("sensor data missed! (%llu)\n", raw.timestamp - last_data);
=======
						warnx("data missed! (%llu)\n", static_cast<unsigned long long>(raw.timestamp - last_data));
>>>>>>> att EKF: Enable execution on Linux
					}

					last_data = raw.timestamp;

					/* send out */
					att.timestamp = raw.timestamp;

					att.roll = euler[0];
					att.pitch = euler[1];
					att.yaw = euler[2] + mag_decl;

					att.rollspeed = x_aposteriori[0];
					att.pitchspeed = x_aposteriori[1];
					att.yawspeed = x_aposteriori[2];
					att.rollacc = x_aposteriori[3];
					att.pitchacc = x_aposteriori[4];
					att.yawacc = x_aposteriori[5];

					att.g_comp[0] = raw.accelerometer_m_s2[0] - acc(0);
					att.g_comp[1] = raw.accelerometer_m_s2[1] - acc(1);
					att.g_comp[2] = raw.accelerometer_m_s2[2] - acc(2);

					/* copy offsets */
					memcpy(&att.rate_offsets, &(x_aposteriori[3]), sizeof(att.rate_offsets));

					/* magnetic declination */

					math::Matrix<3, 3> R_body = (&Rot_matrix[0]);
					R = R_decl * R_body;
					math::Quaternion q;
					q.from_dcm(R);
					/* copy rotation matrix */
					memcpy(&att.R[0], &R.data[0][0], sizeof(att.R));
					memcpy(&att.q[0],&q.data[0],sizeof(att.q));
					att.R_valid = true;

<<<<<<< HEAD
					if (isfinite(att.q[0]) && isfinite(att.q[1])
						&& isfinite(att.q[2]) && isfinite(att.q[3])) {
=======
					if (std::isfinite(att.roll) && std::isfinite(att.pitch) && std::isfinite(att.yaw)) {
>>>>>>> att EKF: Enable execution on Linux
						// Broadcast
						orb_publish(ORB_ID(vehicle_attitude), pub_att, &att);

					} else {
						warnx("ERR: NaN estimate!");
					}

					perf_end(ekf_loop_perf);
				}
			}
		}

		loopcounter++;
	}

	thread_running = false;

	return 0;
}
