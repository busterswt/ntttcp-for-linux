// ----------------------------------------------------------------------------------
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
// Author: Shihua (Simon) Xiao, sixiao@microsoft.com
// ----------------------------------------------------------------------------------

#include "main.h"

/************************************************************/
//		ntttcp high level functions
/************************************************************/
int run_ntttcp_sender(struct ntttcp_test_endpoint *tep)
{
	int err_code = NO_ERROR;
	struct ntttcp_test *test = tep->test;
	char *log = NULL;

	pthread_attr_t  pth_attrs;
	uint n, t, threads_created = 0, total_conns_created = 0;
	struct ntttcp_stream_client *cs;
	int rc, reply_received;
	uint64_t nbytes = 0, total_bytes = 0;
	void *p_retval;
	struct timeval now;
	double actual_test_time = 0;

	if (test->no_synch == false ) {
		/* Negotiate with receiver on:
		* 1) receiver state: is receiver busy with another test?
		* 2) submit sender's test duration time to receiver to negotiate
		* 3) request receiver to start the test
		*/
		reply_received = create_sender_sync_socket( tep );
		if (reply_received == 0) {
			PRINT_ERR("sender: failed to create sync socket");
			return ERROR_GENERAL;
		}
		tep->synch_socket = reply_received;
		reply_received = query_receiver_busy_state(tep->synch_socket);
		if (reply_received == -1) {
			PRINT_ERR("sender: failed to query receiver state");
			return ERROR_GENERAL;
		}
		if (reply_received == 1) {
			PRINT_ERR("sender: receiver is busy with another test");
			return ERROR_GENERAL;
		}

		reply_received = negotiate_test_duration(tep->synch_socket, test->duration);
		if (reply_received == -1) {
			PRINT_ERR("sender: failed to negotiate test duration with receiver");
			return ERROR_GENERAL;
		}
		if (reply_received != test->duration) {
			if (reply_received == 0) {
				PRINT_INFO("test is negotiated to run with continuous mode");
				set_ntttcp_test_endpoint_test_continuous(tep);
			} else {
				ASPRINTF(&log, "test duration negotiated is: %d seconds", reply_received);
				PRINT_INFO_FREE(log);
			}
		}
		tep->confirmed_duration = reply_received;

		reply_received = request_to_start(tep->synch_socket,
						  tep->test->last_client ? (int)'L' : (int)'R' );
		if (reply_received == -1) {
			PRINT_ERR("sender: failed to sync with receiver to start test");
			return ERROR_GENERAL;
		}
		if (reply_received == 0) {
			PRINT_ERR("sender: receiver refuse to start test right now");
			return ERROR_GENERAL;
		}

		/* if we go here, the pre-test sync has completed */
		PRINT_INFO("Network activity progressing...");
	}
	else {
		PRINT_INFO("Starting sender activity (no sync) ...");
	}

	/* create threads */
	pthread_attr_init(&pth_attrs);
	pthread_attr_setstacksize(&pth_attrs, THREAD_STACK_SIZE);

	for (t = 0; t < test->server_ports; t++) {
		for (n = 0; n < test->threads_per_server_port; n++ ) {
			cs = tep->client_streams[t * test->threads_per_server_port + n];
			/* in client side, multiple connections will (one thread for one connection)
			 * connect to same port on server
			 */
			cs->server_port = test->server_base_port + t;

			/* If sender side is being asked to pin the client source port */
			if (test->client_base_port > 0)
				cs->client_port = test->client_base_port
					          + (t * test->threads_per_server_port + n) * test->conns_per_thread;

			if (test->protocol == TCP) {
				rc = pthread_create(&tep->threads[threads_created],
							&pth_attrs,
							run_ntttcp_sender_tcp_stream,
							(void*)cs);
			}
			else {
				rc = pthread_create(&tep->threads[threads_created],
							&pth_attrs,
							run_ntttcp_sender_udp_stream,
							(void*)cs);
			}

			if (rc) {
				ASPRINTF(&log, "pthread_create() create thread failed. errno = %d", errno);
				PRINT_ERR_FREE(log);
				err_code = ERROR_PTHREAD_CREATE;
				continue;
			}
			else{
				threads_created++;
			}
		}
	}
	pthread_attr_destroy(&pth_attrs);

	ASPRINTF(&log, "%d threads created", threads_created);
	PRINT_INFO_FREE(log);

	turn_on_light();

	/* in the case of running in continuous_mode */
	if (tep->confirmed_duration == 0) {
		sleep (UINT_MAX);
		/* either sleep has elapsed, or sleep was interrupted  by a signal */
		return err_code;
	}

	get_cpu_usage( tep->results->init_cpu_usage );
	get_cpu_usage_from_proc_stat( tep->results->init_cpu_ps );
	get_tcp_retrans( tep->results->init_tcp_retrans );

	/* run the timer. it will trigger turn_off_light() after timer timeout */
	run_test_timer(tep->confirmed_duration);
	tep->state = TEST_RUNNING;
	gettimeofday(&now, NULL);
	tep->start_time = now;

	/* wait test done */
	wait_light_off();

	tep->state = TEST_FINISHED;
	gettimeofday(&now, NULL);
	tep->end_time = now;

	/* calculate the actual test run time */
	actual_test_time = get_time_diff(&tep->end_time, &tep->start_time);

	/*
	 * if actual_test_time < tep->confirmed_duration;
	 * then this indicates that in the sender side, test is being interrupted.
	 * hence, tell receiver about this.
	 */
	if (actual_test_time < tep->confirmed_duration) {
		tell_receiver_test_exit(tep->synch_socket);
	}
	close(tep->synch_socket);

	/* calculate resource usage */
	get_cpu_usage( tep->results->final_cpu_usage );
	get_cpu_usage_from_proc_stat( tep->results->final_cpu_ps );
	get_tcp_retrans( tep->results->final_tcp_retrans );

	/* calculate client side throughput, but exclude the last thread as it is synch thread */
	for (n = 0; n < threads_created; n++) {
		if (pthread_join(tep->threads[n], &p_retval) !=0 ) {
			PRINT_ERR("sender: error when pthread_join");
			continue;
		}
		total_conns_created += tep->client_streams[n]->num_conns_created;
		nbytes = tep->client_streams[n]->total_bytes_transferred;
		total_bytes += nbytes;

		tep->results->threads[n]->total_bytes = nbytes;
		tep->results->threads[n]->actual_test_time = actual_test_time;
	}
	ASPRINTF(&log, "%d connections tested", total_conns_created);
	PRINT_INFO_FREE(log);

	tep->results->total_bytes = total_bytes;
	tep->results->actual_test_time = actual_test_time;

	process_test_results(tep);
	print_test_results(tep);
	if (tep->test->save_xml_log)
		if (write_result_into_log_file(tep))
			PRINT_ERR("Error writing log to xml file");

	return err_code;
}

int run_ntttcp_receiver(struct ntttcp_test_endpoint *tep)
{
	int err_code = NO_ERROR;
	struct ntttcp_test *test = tep->test;
	char *log = NULL;

	uint t, threads_created = 0;
	struct ntttcp_stream_server *ss;
	int rc;
	uint64_t nbytes = 0, total_bytes = 0;
	struct timeval now;
	double actual_test_time = 0;

	/* create threads */
	for (t = 0; t < test->server_ports; t++) {
		ss = tep->server_streams[t];
		ss->server_port = test->server_base_port + t;

		if (test->protocol == TCP) {
			rc = pthread_create(&tep->threads[t],
						NULL,
						run_ntttcp_receiver_tcp_stream,
						(void*)ss);
		}
		else {
			rc = pthread_create(&tep->threads[t],
						NULL,
						run_ntttcp_receiver_udp_stream,
						(void*)ss);
		}

		if (rc) {
			PRINT_ERR("pthread_create() create thread failed");
			err_code = ERROR_PTHREAD_CREATE;
			continue;
		}
		threads_created++;
	}

	/* create synch thread; and put it to the end of the thread array */
	if (test->no_synch == false) {
		/* ss struct is not used in sync thread, because:
		 * we are only allowed to pass one param to the thread in pthread_create();
		 * but the information stored in ss, is not enough to be used for synch;
		 * so we pass *tep to the pthread_create().
		 * notes:
		 * 1) we will calculate the tcp port for synch stream in create_receiver_sync_socket().
		 *   the synch_port = base_port -1
		 * 2) we will assign the protocol for synch stream to TCP, always, in create_receiver_sync_socket()
		 */
		ss = tep->server_streams[test->server_ports];
		ss->server_port = test->server_base_port - 1; //just for bookkeeping
		ss->protocol = TCP; //just for bookkeeping
		ss->is_sync_thread = true;

		rc = pthread_create(&tep->threads[t],
				NULL,
				create_receiver_sync_socket,
				(void*)tep);
		if (rc) {
			PRINT_ERR("pthread_create() create thread failed");
			err_code = ERROR_PTHREAD_CREATE;
		}
		else {
			threads_created++;
		}
	}

	ASPRINTF(&log, "%d threads created", threads_created);
	PRINT_INFO_FREE(log);

	while ( 1 ) {
		/* for receiver, there are two ways to trigger test start:
		 * a) if synch enabled, then sync thread will trigger turn_on_light() after sync completed;
		 *	see create_receiver_sync_socket()
		 * b) if no synch enabled, then any tcp server accept client connections, the turn_on_light() will be triggered;
		 *	see ntttcp_server_epoll(), or ntttcp_server_select()
		 */
		wait_light_on();

		/* reset the counter?
		 * yes. we need to reset server side perf counters at the beginning, after light-is-on;
		 * this is to handle these cases when:
		 * a) receiver in sync mode, but sender connected as no_sync mode;
		 * in this case, before light-is-on, the threads have some data counted already.
		 * b) receiver is running in a loop; the previous test has finished but the sockets are still working and
		 * receiving data (data arrived with latency); so we need to reset the counter before a new test starting.
		 *
		 * this "reset" is implemented by using __sync_lock_test_and_set().
		 * reference: https://gcc.gnu.org/onlinedocs/gcc-4.4.7/gcc/Atomic-Builtins.html
		 */
		for (t = 0; t < threads_created; t++)
			/* discard the bytes received before test starting */
			(uint64_t)__sync_lock_test_and_set(&(tep->server_streams[t]->total_bytes_transferred), 0);

		/* in the case of running in continuous_mode */
		if (tep->confirmed_duration == 0) {
			sleep (UINT_MAX);
			/* either sleep has elapsed, or sleep was interrupted  by a signal */
			return err_code;
		}

		get_cpu_usage( tep->results->init_cpu_usage );
		get_cpu_usage_from_proc_stat( tep->results->init_cpu_ps );
		get_tcp_retrans( tep->results->init_tcp_retrans );

		/* run the timer. it will trigger turn_off_light() after timer timeout */
		run_test_timer(tep->confirmed_duration);
		tep->state = TEST_RUNNING;
		gettimeofday(&now, NULL);
		tep->start_time = now;

		/* wait test done */
		wait_light_off();
		tep->state = TEST_FINISHED;
		tep->num_remote_endpoints = 0;
		for (t=0; t<MAX_REMOTE_ENDPOINTS; t++) tep->remote_endpoints[t] = -1;
		gettimeofday(&now, NULL);
		tep->end_time = now;

		/* calculate the actual test run time */
		actual_test_time = get_time_diff(&tep->end_time, &tep->start_time);

		/* calculate resource usage */
		get_cpu_usage( tep->results->final_cpu_usage );
		get_cpu_usage_from_proc_stat( tep->results->final_cpu_ps );
		get_tcp_retrans( tep->results->final_tcp_retrans );

		//sleep(1);  //looks like server needs more time to receive data ...

		/* calculate server side throughput */
		total_bytes = 0;
		for (t=0; t < threads_created; t++){
			/* exclude the sync thread */
			if (tep->server_streams[t]->is_sync_thread == true)
				continue;

			/* read and reset the counter */
			nbytes = (uint64_t)__sync_lock_test_and_set(&(tep->server_streams[t]->total_bytes_transferred), 0);
			total_bytes += nbytes;

			tep->results->threads[t]->total_bytes = nbytes;
			tep->results->threads[t]->actual_test_time = actual_test_time;
		}
		tep->results->total_bytes = total_bytes;
		tep->results->actual_test_time = actual_test_time;

		process_test_results(tep);
		print_test_results(tep);
		if (tep->test->save_xml_log)
			if (write_result_into_log_file(tep))
				PRINT_ERR("Error writing log to xml file");

		if (tep->receiver_exit_after_done)
			break;
	}

	for (t=0; t < threads_created; t++) {
		pthread_join(tep->threads[t], NULL);
	}

	return err_code;
}

int main(int argc, char **argv)
{
	int err_code = NO_ERROR;
	cpu_set_t cpuset;
	struct ntttcp_test *test;
	struct ntttcp_test_endpoint *tep;

	/* catch SIGINT: Ctrl + C */
	if (signal(SIGINT, sig_handler) == SIG_ERR)
		PRINT_ERR("main: error when setting the disposition of the signal SIGINT");

	/*
	 * Set the SIGPIPE handler to SIG_IGN.
	 * This will prevent any socket or pipe write from causing a SIGPIPE signal.
	 */
	signal(SIGPIPE, SIG_IGN);

	print_version();
	test = new_ntttcp_test();
	if (!test) {
		PRINT_ERR("main: error when creating new test");
		exit (-1);
	}

	default_ntttcp_test(test);
	err_code = parse_arguments(test, argc, argv);
	if (err_code != NO_ERROR) {
		PRINT_ERR("main: error when parsing args");
		print_flags(test);
		free(test);
		exit (-1);
	}

	err_code = verify_args(test);
	if (err_code != NO_ERROR) {
		PRINT_ERR("main: error when verifying the args");
		print_flags(test);
		free(test);
		exit (-1);
	}

	if (test->verbose)
		print_flags(test);

	if (!check_resource_limit(test)) {
		PRINT_ERR("main: error when checking resource limits");
		free(test);
		exit (-1);
	}

	turn_off_light();

	if (test->cpu_affinity != -1) {
		CPU_ZERO(&cpuset);
		CPU_SET(test->cpu_affinity, &cpuset);
		PRINT_INFO("main: set cpu affinity");
		if ( pthread_setaffinity_np( pthread_self(), sizeof(cpu_set_t ), &cpuset) != 0 )
			PRINT_ERR("main: cannot set cpu affinity");
	}

	if (test->daemon) {
		PRINT_INFO("main: run this tool in the background");
		if ( daemon(0, 0) != 0 )
			PRINT_ERR("main: cannot run this tool in the background");
	}

	if (test->client_role == true) {
		tep = new_ntttcp_test_endpoint(test, ROLE_SENDER);
		err_code = run_ntttcp_sender(tep);
	}
	else {
		tep = new_ntttcp_test_endpoint(test, ROLE_RECEIVER);
		err_code = run_ntttcp_receiver(tep);
	}

	free_ntttcp_test_endpoint_and_test(tep);
	return err_code;
}
