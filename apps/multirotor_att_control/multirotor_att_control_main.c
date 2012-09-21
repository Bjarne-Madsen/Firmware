/****************************************************************************
 *
 *   Copyright (C) 2008-2012 PX4 Development Team. All rights reserved.
 *   Author: @author Lorenz Meier <lm@inf.ethz.ch>
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

/**
 * @file multirotor_att_control_main.c
 *
 * Implementation of multirotor attitude control main loop.
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <debug.h>
#include <getopt.h>
#include <time.h>
#include <math.h>
#include <poll.h>
#include <sys/prctl.h>
#include <arch/board/up_hrt.h>
#include <uORB/uORB.h>
#include <uORB/topics/vehicle_status.h>
#include <uORB/topics/vehicle_attitude.h>
#include <uORB/topics/vehicle_attitude_setpoint.h>
#include <uORB/topics/manual_control_setpoint.h>
#include <uORB/topics/offboard_control_setpoint.h>
#include <uORB/topics/vehicle_rates_setpoint.h>
#include <uORB/topics/sensor_combined.h>
#include <uORB/topics/actuator_controls.h>

#include <systemlib/perf_counter.h>

#include "multirotor_attitude_control.h"
#include "multirotor_rate_control.h"

__EXPORT int multirotor_att_control_main(int argc, char *argv[]);


static enum {
	CONTROL_MODE_RATES = 0,
	CONTROL_MODE_ATTITUDE = 1,
} control_mode;


static bool thread_should_exit;
static int mc_task;
static bool motor_test_mode = false;

static int
mc_thread_main(int argc, char *argv[])
{
	/* declare and safely initialize all structs */
	struct vehicle_status_s state;
	memset(&state, 0, sizeof(state));
	struct vehicle_attitude_s att;
	memset(&att, 0, sizeof(att));
	struct vehicle_attitude_setpoint_s att_sp;
	memset(&att_sp, 0, sizeof(att_sp));
	struct manual_control_setpoint_s manual;
	memset(&manual, 0, sizeof(manual));
	struct sensor_combined_s raw;
	memset(&raw, 0, sizeof(raw));
	struct offboard_control_setpoint_s offboard_sp;
	memset(&offboard_sp, 0, sizeof(offboard_sp));
	struct vehicle_rates_setpoint_s rates_sp;
	memset(&rates_sp, 0, sizeof(rates_sp));

	struct actuator_controls_s actuators;

	/* subscribe to attitude, motor setpoints and system state */
	int att_sub = orb_subscribe(ORB_ID(vehicle_attitude));
	int att_setpoint_sub = orb_subscribe(ORB_ID(vehicle_attitude_setpoint));
	int setpoint_sub = orb_subscribe(ORB_ID(offboard_control_setpoint));
	int state_sub = orb_subscribe(ORB_ID(vehicle_status));
	int manual_sub = orb_subscribe(ORB_ID(manual_control_setpoint));
	int sensor_sub = orb_subscribe(ORB_ID(sensor_combined));

	/* 
	 * Do not rate-limit the loop to prevent aliasing
	 * if rate-limiting would be desired later, the line below would
	 * enable it.
	 *
	 * rate-limit the attitude subscription to 200Hz to pace our loop
	 * orb_set_interval(att_sub, 5);
	 */
	struct pollfd fds = { .fd = att_sub, .events = POLLIN };

	/* publish actuator controls */
	for (unsigned i = 0; i < NUM_ACTUATOR_CONTROLS; i++) {
		actuators.control[i] = 0.0f;
	}

	orb_advert_t actuator_pub = orb_advertise(ORB_ID_VEHICLE_ATTITUDE_CONTROLS, &actuators);
	orb_advert_t att_sp_pub = orb_advertise(ORB_ID(vehicle_attitude_setpoint), &att_sp);

	/* register the perf counter */
	perf_counter_t mc_loop_perf = perf_alloc(PC_ELAPSED, "multirotor_att_control");

	/* welcome user */
	printf("[multirotor_att_control] starting\n");

	while (!thread_should_exit) {

		/* wait for a sensor update, check for exit condition every 500 ms */
		poll(&fds, 1, 500);

		perf_begin(mc_loop_perf);

		/* get a local copy of system state */
		orb_copy(ORB_ID(vehicle_status), state_sub, &state);
		/* get a local copy of manual setpoint */
		orb_copy(ORB_ID(manual_control_setpoint), manual_sub, &manual);
		/* get a local copy of attitude */
		orb_copy(ORB_ID(vehicle_attitude), att_sub, &att);
		/* get a local copy of attitude setpoint */
		orb_copy(ORB_ID(vehicle_attitude_setpoint), att_setpoint_sub, &att_sp);
		/* get a local copy of rates setpoint */
		bool updated;
		orb_check(setpoint_sub, &updated);
		if (updated) {
			orb_copy(ORB_ID(offboard_control_setpoint), setpoint_sub, &offboard_sp);
		}
		/* get a local copy of the current sensor values */
		orb_copy(ORB_ID(sensor_combined), sensor_sub, &raw);


		/** STEP 1: Define which input is the dominating control input */

		if (state.flag_control_manual_enabled) {
			/* manual inputs, from RC control or joystick */
			att_sp.roll_body = manual.roll;
			att_sp.pitch_body = manual.pitch;
			att_sp.yaw_body = manual.yaw; // XXX Hack, remove, switch to yaw rate controller
			/* set yaw rate */
			rates_sp.yaw = manual.yaw;
			att_sp.timestamp = hrt_absolute_time();
			
			if (motor_test_mode) {
				att_sp.roll_body = 0.0f;
				att_sp.pitch_body = 0.0f;
				att_sp.yaw_body = 0.0f;
				att_sp.thrust = 0.1f;
			} else {
				att_sp.thrust = manual.throttle;
			}
		} else if (state.flag_control_offboard_enabled) {
			/* offboard inputs */
			if (offboard_sp.mode == OFFBOARD_CONTROL_MODE_DIRECT_RATES) {
				rates_sp.roll = offboard_sp.p1;
				rates_sp.pitch = offboard_sp.p2;
				rates_sp.yaw = offboard_sp.p3;
				rates_sp.thrust = offboard_sp.p4;
			} else if (offboard_sp.mode == OFFBOARD_CONTROL_MODE_DIRECT_ATTITUDE) {
				att_sp.roll_body = offboard_sp.p1;
				att_sp.pitch_body = offboard_sp.p2;
				att_sp.yaw_body = offboard_sp.p3;
				att_sp.thrust = offboard_sp.p4;
			}

			/* decide wether we want rate or position input */
		}

		/** STEP 2: Identify the controller setup to run and set up the inputs correctly */

		/* run attitude controller */
		if (state.flag_control_attitude_enabled) {
			multirotor_control_attitude(&att_sp, &att, &actuators);
		}

		/* XXX could be also run in an independent loop */
		if (state.flag_control_rates_enabled) {
			/* lowpass gyros */
			// XXX
			static float gyro_lp[3] = {0, 0, 0};
			gyro_lp[0] = raw.gyro_rad_s[0];
			gyro_lp[1] = raw.gyro_rad_s[1];
			gyro_lp[2] = raw.gyro_rad_s[2];

			multirotor_control_rates(&rates_sp, gyro_lp, &actuators);
		}

		/* STEP 3: publish the result to the vehicle actuators */
		orb_publish(ORB_ID_VEHICLE_ATTITUDE_CONTROLS, actuator_pub, &actuators);
		orb_publish(ORB_ID(vehicle_attitude_setpoint), att_sp_pub, &att_sp);

		perf_end(mc_loop_perf);
	}

	printf("[multirotor att control] stopping, disarming motors.\n");

	/* kill all outputs */
	for (unsigned i = 0; i < NUM_ACTUATOR_CONTROLS; i++)
		actuators.control[i] = 0.0f;
	orb_publish(ORB_ID_VEHICLE_ATTITUDE_CONTROLS, actuator_pub, &actuators);


	close(att_sub);
	close(state_sub);
	close(manual_sub);
	close(actuator_pub);
	close(att_sp_pub);

	perf_print_counter(mc_loop_perf);
	perf_free(mc_loop_perf);

	fflush(stdout);
	exit(0);
}

static void
usage(const char *reason)
{
	if (reason)
		fprintf(stderr, "%s\n", reason);
	fprintf(stderr, "usage: multirotor_att_control [-m <mode>] [-t] {start|status|stop}\n");
	fprintf(stderr, "    <mode> is 'rates' or 'attitude'\n");
	fprintf(stderr, "    -t enables motor test mode with 10%% thrust\n");
	exit(1);
}

int multirotor_att_control_main(int argc, char *argv[])
{
	int	ch;

	control_mode = CONTROL_MODE_RATES;
	unsigned int optioncount = 0;

	while ((ch = getopt(argc, argv, "tm:")) != EOF) {
		switch (ch) {
		case 'm':
			if (!strcmp(optarg, "rates")) {
				control_mode = CONTROL_MODE_RATES;
			} else if (!strcmp(optarg, "attitude")) {
				control_mode = CONTROL_MODE_RATES;
			} else {
				usage("unrecognized -m value");
			}
			optioncount += 2;
			break;
		case 't':
			motor_test_mode = true;
			optioncount += 1;
			break;
		case ':':
			usage("missing parameter");
			break;
		default:
			fprintf(stderr, "option: -%c\n", ch);
			usage("unrecognized option");
		}
	}
	argc -= optioncount;
	//argv += optioncount;

	if (argc < 1)
		usage("missing command");

	if (!strcmp(argv[1+optioncount], "start")) {

		thread_should_exit = false;
		mc_task = task_create("multirotor_att_control", SCHED_PRIORITY_MAX - 15, 2048, mc_thread_main, NULL);
		exit(0);
	}

	if (!strcmp(argv[1+optioncount], "stop")) {
		thread_should_exit = true;
		exit(0);
	}

	usage("unrecognized command");
	exit(1);
}
