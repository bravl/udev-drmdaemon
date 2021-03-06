/**
 * @file drmdaemon.c
 * @Brief  Use DRM to control outputs and resolutions
 * Linking netsockets and DRM together to get dynamic resolution changes
 * @author Bram Vlerick
 * @version 1.0
 * @date 2017-01-16
 * TODO: Change debug define with parameter parsing at boot
 * TODO: cleanup drm_connector_obj list properly
 * TODO: Add queueing mechanism for udev events
 * Note: Select in reading udev statement due to libudev bug
 * http://stackoverflow.com/questions/15687784/libudev-monitoring-returns-null-pointer-on-raspbian
 */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "debug.h"
#include "modeset.h"
#include "queue.h"
#include "udev_helper.h"

/* Pthread mutex used to protect condition variable */
pthread_mutex_t cond_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Uncomment to run without daemon and console logging */
#define DEBUG

static int daemonize()
{
	int x;
	pid_t pid;

	pid = fork();
	if (pid < 0) { /* Failed to create fork */
		logger_log(LOG_LVL_ERROR, "Failed to fork");
		return -1;
	}

	/* Gracefully exit parent */
	if (pid > 0) exit(EXIT_SUCCESS);

	/* Let child become session leader*/
	if (setsid() < 0) return -1;

	/* Ignore signals */
	signal(SIGCHLD, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	pid = fork();
	if (pid < 0) {
		logger_log(LOG_LVL_ERROR, "Failed to create second fork");
		return -1;
	}

	/* Gracefully exit second parent */
	if (pid > 0) exit(EXIT_SUCCESS);
	umask(0);

	/* Close open file discriptors */
	for (x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
		close(x);
	}
	return 0;
}

void *udev_thread_handler(void *data)
{
	struct udev *udev = NULL;
	struct udev_monitor *mon = NULL;
	struct queue *udev_queue = (struct queue *)data;
	udev = udev_new();
	if (!udev) {
		logger_log(LOG_LVL_ERROR, "Failed to create udev instance");
		return 0;
	}
	mon = setup_udev_monitor(udev, "drm");
	if (!mon) {
		/* TODO: remove udev struct */
		return 0;
	}
	logger_log(LOG_LVL_OK, "Udev initialisation ok");

	int fd = udev_monitor_get_fd(mon);
	while (1) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		int ret = select(fd + 1, &fds, NULL, NULL, NULL);
		if (ret > 0 && FD_ISSET(fd, &fds)) {
			struct udev_device *dev =
			    udev_monitor_receive_device(mon);
			if (dev == NULL) {
				logger_log(LOG_LVL_ERROR,
					   "Failed to retrieve device\n");
				continue;
			}

			/* Push data to the udev queue that is handled by main
			 * thread*/
			pthread_mutex_lock(&cond_mutex);
			queue_push(udev_queue, (void *)dev);
			pthread_mutex_unlock(&cond_mutex);
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	int retval = 0;
	pthread_t udev_thread;
	struct queue *udev_queue;
	struct drm_connector_obj *connectors = NULL;

	/*TODO: Add cleanup function!! */
	udev_queue = queue_init(NULL);

#ifndef DEBUG
	if (daemonize() < 0) {
		logger_log(LOG_LVL_ERROR, "Failed to daemonize");
		return -1;
	}
	logger_set_file_logging("log.txt");
#endif
	logger_log(LOG_LVL_INFO, "Running drmdaemon");
	logger_log(LOG_LVL_INFO, "Creating daemon");

	if (init_drm_handler() < 0) {
		retval = -1;
		goto end;
	}
	logger_log(LOG_LVL_INFO, "Populating DRM connector list");
	connectors = populate_drm_conn_list("/dev/dri/card0");
	if (!connectors) {
		logger_log(LOG_LVL_ERROR, "Failed to retrieve connectors");
		retval = -1;
		goto end;
	}
	logger_log(LOG_LVL_OK, "List populated");

	/* Create pthread for udev */
	if (pthread_create(
		&udev_thread, NULL, udev_thread_handler, (void *)udev_queue) <
	    0) {
		logger_log(LOG_LVL_ERROR, "Failed to create pthread");
		goto end;
	}

	/* While wait for condition from udev*/
	/* Trigger DRM comparision when signal is received from udev */
	while (1) {

		pthread_mutex_lock(&cond_mutex);
		if (QUEUE_SIZE(udev_queue) != 0) {
			logger_log(LOG_LVL_INFO, "new items added");
			void *data;
			queue_pop(udev_queue, &data);
			udev_device_unref((struct udev_device *)data);
		}
		pthread_mutex_unlock(&cond_mutex);
	}
end:
	return retval;
}
