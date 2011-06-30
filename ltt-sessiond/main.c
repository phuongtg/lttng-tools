/*
 * Copyright (C) 2011 - David Goulet <david.goulet@polymtl.ca>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>

#include <urcu/list.h>		/* URCU list library (-lurcu) */
#include <lttng/lttng.h>

#include "liblttsessiondcomm.h"
#include "ltt-sessiond.h"
#include "lttngerr.h"
#include "kernel-ctl.h"
#include "ust-ctl.h"
#include "session.h"
#include "traceable-app.h"
#include "lttng-kconsumerd.h"
#include "libustctl.h"

/*
 * TODO:
 * teardown: signal SIGTERM handler -> write into pipe. Threads waits
 * with epoll on pipe and on other pipes/sockets for commands.  Main
 * simply waits on pthread join.
 */

/* Const values */
const char default_home_dir[] = DEFAULT_HOME_DIR;
const char default_tracing_group[] = LTTNG_DEFAULT_TRACING_GROUP;
const char default_ust_sock_dir[] = DEFAULT_UST_SOCK_DIR;
const char default_global_apps_pipe[] = DEFAULT_GLOBAL_APPS_PIPE;

/* Variables */
int opt_verbose;    /* Not static for lttngerr.h */
int opt_quiet;      /* Not static for lttngerr.h */
const char *progname;
const char *opt_tracing_group;
static int opt_sig_parent;
static int opt_daemon;
static int is_root;			/* Set to 1 if the daemon is running as root */
static pid_t ppid;          /* Parent PID for --sig-parent option */
static pid_t kconsumerd_pid;

static char apps_unix_sock_path[PATH_MAX];				/* Global application Unix socket path */
static char client_unix_sock_path[PATH_MAX];			/* Global client Unix socket path */
static char kconsumerd_err_unix_sock_path[PATH_MAX];	/* kconsumerd error Unix socket path */
static char kconsumerd_cmd_unix_sock_path[PATH_MAX];	/* kconsumerd command Unix socket path */

/* Sockets and FDs */
static int client_sock;
static int apps_sock;
static int kconsumerd_err_sock;
static int kconsumerd_cmd_sock;
static int kernel_tracer_fd;

/* Pthread, Mutexes and Semaphores */
static pthread_t kconsumerd_thread;
static pthread_t apps_thread;
static pthread_t client_thread;
static sem_t kconsumerd_sem;

static pthread_mutex_t kconsumerd_pid_mutex;	/* Mutex to control kconsumerd pid assignation */

/*
 *  teardown_kernel_session
 *
 *  Complete teardown of a kernel session. This free all data structure
 *  related to a kernel session and update counter.
 */
static void teardown_kernel_session(struct ltt_session *session)
{
	if (session->kernel_session != NULL) {
		DBG("Tearing down kernel session");
		trace_destroy_kernel_session(session->kernel_session);
		/* Extra precaution */
		session->kernel_session = NULL;
		/* Decrement session count */
		session->kern_session_count--;
	}
}

/*
 *  cleanup
 *
 *  Cleanup the daemon on exit
 */
static void cleanup()
{
	int ret;
	char *cmd;
	struct ltt_session *sess;

	DBG("Cleaning up");

	/* <fun> */
	MSG("\n%c[%d;%dm*** assert failed *** ==> %c[%dm", 27,1,31,27,0);
	MSG("%c[%d;%dmMatthew, BEET driven development works!%c[%dm",27,1,33,27,0);
	/* </fun> */

	/* Stopping all threads */
	DBG("Terminating all threads");
	pthread_cancel(client_thread);
	pthread_cancel(apps_thread);
	if (kconsumerd_pid != 0) {
		pthread_cancel(kconsumerd_thread);
	}

	DBG("Unlinking all unix socket");
	unlink(client_unix_sock_path);
	unlink(apps_unix_sock_path);
	unlink(kconsumerd_err_unix_sock_path);

	DBG("Removing %s directory", LTTNG_RUNDIR);
	ret = asprintf(&cmd, "rm -rf " LTTNG_RUNDIR);
	if (ret < 0) {
		ERR("asprintf failed. Something is really wrong!");
	}

	/* Remove lttng run directory */
	ret = system(cmd);
	if (ret < 0) {
		ERR("Unable to clean " LTTNG_RUNDIR);
	}

	DBG("Cleaning up all session");
	/* Cleanup ALL session */
	cds_list_for_each_entry(sess, &ltt_session_list.head, list) {
		teardown_kernel_session(sess);
		// TODO complete session cleanup (including UST)
	}

	DBG("Closing kernel fd");
	close(kernel_tracer_fd);
}

/*
 *  send_unix_sock
 *
 *  Send data on a unix socket using the liblttsessiondcomm API.
 *
 *  Return lttcomm error code.
 */
static int send_unix_sock(int sock, void *buf, size_t len)
{
	/* Check valid length */
	if (len <= 0) {
		return -1;
	}

	return lttcomm_send_unix_sock(sock, buf, len);
}

/*
 *  clean_command_ctx
 *
 *  Free memory of a command context structure.
 */
static void clean_command_ctx(struct command_ctx *cmd_ctx)
{
	DBG("Clean command context structure %p", cmd_ctx);
	if (cmd_ctx) {
		if (cmd_ctx->llm) {
			free(cmd_ctx->llm);
		}
		if (cmd_ctx->lsm) {
			free(cmd_ctx->lsm);
		}
		free(cmd_ctx);
		cmd_ctx = NULL;
	}
}

/*
 *  send_kconsumerd_fds
 *
 *  Send all stream fds of the kernel session to the consumer.
 */
static int send_kconsumerd_fds(int sock, struct ltt_kernel_session *session)
{
	int ret;
	size_t nb_fd;
	struct ltt_kernel_stream *stream;
	struct ltt_kernel_channel *chan;
	struct lttcomm_kconsumerd_header lkh;
	struct lttcomm_kconsumerd_msg lkm;

	nb_fd = session->stream_count_global;

	/* Setup header */
	lkh.payload_size = (nb_fd + 1) * sizeof(struct lttcomm_kconsumerd_msg);
	lkh.cmd_type = ADD_STREAM;

	DBG("Sending kconsumerd header");

	ret = lttcomm_send_unix_sock(sock, &lkh, sizeof(struct lttcomm_kconsumerd_header));
	if (ret < 0) {
		perror("send kconsumerd header");
		goto error;
	}

	DBG("Sending metadata stream fd");

	/* Send metadata stream fd first */
	lkm.fd = session->metadata_stream_fd;
	lkm.state = ACTIVE_FD;
	lkm.max_sb_size = session->metadata->conf->attr.subbuf_size;
	strncpy(lkm.path_name, session->metadata->pathname, PATH_MAX);

	ret = lttcomm_send_fds_unix_sock(sock, &lkm, &lkm.fd, 1, sizeof(lkm));
	if (ret < 0) {
		perror("send kconsumerd fd");
		goto error;
	}

	cds_list_for_each_entry(chan, &session->channel_list.head, list) {
		cds_list_for_each_entry(stream, &chan->stream_list.head, list) {
			lkm.fd = stream->fd;
			lkm.state = stream->state;
			lkm.max_sb_size = chan->channel->attr.subbuf_size;
			strncpy(lkm.path_name, stream->pathname, PATH_MAX);

			DBG("Sending fd %d to kconsumerd", lkm.fd);

			ret = lttcomm_send_fds_unix_sock(sock, &lkm, &lkm.fd, 1, sizeof(lkm));
			if (ret < 0) {
				perror("send kconsumerd fd");
				goto error;
			}
		}
	}

	DBG("Kconsumerd fds sent");

	return 0;

error:
	return ret;
}

/*
 *  create_trace_dir
 *
 *  Create the trace output directory.
 */
static int create_trace_dir(struct ltt_kernel_session *session)
{
	int ret;
	struct ltt_kernel_channel *chan;

	/* Create all channel directories */
	cds_list_for_each_entry(chan, &session->channel_list.head, list) {
		DBG("Creating trace directory at %s", chan->pathname);
		// TODO: recursive create dir
		ret = mkdir(chan->pathname, S_IRWXU | S_IRWXG );
		if (ret < 0) {
			if (ret != EEXIST) {
				perror("mkdir trace path");
				ret = -errno;
				goto error;
			}
		}
	}

	return 0;

error:
	return ret;
}

/*
 * 	ust_connect_app
 *
 * 	Return a socket connected to the libust communication socket
 * 	of the application identified by the pid.
 *
 * 	If the pid is not found in the traceable list,
 * 	return -1 to indicate error.
 */
static int ust_connect_app(pid_t pid)
{
	int sock;
	struct ltt_traceable_app *lta;

	DBG("Connect to application pid %d", pid);

	lta = find_app_by_pid(pid);
	if (lta == NULL) {
		/* App not found */
		DBG("Application pid %d not found", pid);
		return -1;
	}

	sock = ustctl_connect_pid(lta->pid);
	if (sock < 0) {
		ERR("Fail connecting to the PID %d", pid);
	}

	return sock;
}

/*
 * 	notify_apps
 *
 *  Notify apps by writing 42 to a named pipe using name.
 * 	Every applications waiting for a ltt-sessiond will be notified
 * 	and re-register automatically to the session daemon.
 *
 * 	Return open or write error value.
 */
static int notify_apps(const char *name)
{
	int fd;
	int ret = -1;

	DBG("Notify the global application pipe");

	/* Try opening the global pipe */
	fd = open(name, O_WRONLY);
	if (fd < 0) {
		goto error;
	}

	/* Notify by writing on the pipe */
	ret = write(fd, "42", 2);
	if (ret < 0) {
		perror("write");
	}

error:
	return ret;
}

/*
 *  setup_lttng_msg
 *
 *  Setup the outgoing data buffer for the response (llm) by allocating the
 *  right amount of memory and copying the original information from the lsm
 *  structure.
 *
 *  Return total size of the buffer pointed by buf.
 */
static int setup_lttng_msg(struct command_ctx *cmd_ctx, size_t size)
{
	int ret, buf_size;

	buf_size = size;

	cmd_ctx->llm = malloc(sizeof(struct lttcomm_lttng_msg) + buf_size);
	if (cmd_ctx->llm == NULL) {
		perror("malloc");
		ret = -ENOMEM;
		goto error;
	}

	/* Copy common data */
	cmd_ctx->llm->cmd_type = cmd_ctx->lsm->cmd_type;
	cmd_ctx->llm->pid = cmd_ctx->lsm->pid;

	cmd_ctx->llm->data_size = size;
	cmd_ctx->lttng_msg_size = sizeof(struct lttcomm_lttng_msg) + buf_size;

	return buf_size;

error:
	return ret;
}

/*
 *  thread_manage_kconsumerd
 *
 *  This thread manage the kconsumerd error sent
 *  back to the session daemon.
 */
static void *thread_manage_kconsumerd(void *data)
{
	int sock, ret;
	enum lttcomm_return_code code;

	DBG("[thread] Manage kconsumerd started");

	ret = lttcomm_listen_unix_sock(kconsumerd_err_sock);
	if (ret < 0) {
		goto error;
	}

	sock = lttcomm_accept_unix_sock(kconsumerd_err_sock);
	if (sock < 0) {
		goto error;
	}

	/* Getting status code from kconsumerd */
	ret = lttcomm_recv_unix_sock(sock, &code, sizeof(enum lttcomm_return_code));
	if (ret <= 0) {
		goto error;
	}

	if (code == KCONSUMERD_COMMAND_SOCK_READY) {
		kconsumerd_cmd_sock = lttcomm_connect_unix_sock(kconsumerd_cmd_unix_sock_path);
		if (kconsumerd_cmd_sock < 0) {
			sem_post(&kconsumerd_sem);
			perror("kconsumerd connect");
			goto error;
		}
		/* Signal condition to tell that the kconsumerd is ready */
		sem_post(&kconsumerd_sem);
		DBG("Kconsumerd command socket ready");
	} else {
		DBG("Kconsumerd error when waiting for SOCK_READY : %s",
				lttcomm_get_readable_code(-code));
		goto error;
	}

	/* Wait for any kconsumerd error */
	ret = lttcomm_recv_unix_sock(sock, &code, sizeof(enum lttcomm_return_code));
	if (ret <= 0) {
		ERR("Kconsumerd closed the command socket");
		goto error;
	}

	ERR("Kconsumerd return code : %s", lttcomm_get_readable_code(-code));

error:
	kconsumerd_pid = 0;
	DBG("Kconsumerd thread dying");
	return NULL;
}

/*
 * 	thread_manage_apps
 *
 * 	This thread manage the application socket communication
 */
static void *thread_manage_apps(void *data)
{
	int sock, ret;

	/* TODO: Something more elegant is needed but fine for now */
	/* FIXME: change all types to either uint8_t, uint32_t, uint64_t
	 * for 32-bit vs 64-bit compat processes. */
	/* replicate in ust with version number */
	struct {
		int reg;	/* 1:register, 0:unregister */
		pid_t pid;
		uid_t uid;
	} reg_msg;

	DBG("[thread] Manage apps started");

	ret = lttcomm_listen_unix_sock(apps_sock);
	if (ret < 0) {
		goto error;
	}

	/* Notify all applications to register */
	notify_apps(default_global_apps_pipe);

	while (1) {
		DBG("Accepting application registration");
		/* Blocking call, waiting for transmission */
		sock = lttcomm_accept_unix_sock(apps_sock);
		if (sock < 0) {
			goto error;
		}

		/* Basic recv here to handle the very simple data
		 * that the libust send to register (reg_msg).
		 */
		ret = recv(sock, &reg_msg, sizeof(reg_msg), 0);
		if (ret < 0) {
			perror("recv");
			continue;
		}

		/* Add application to the global traceable list */
		if (reg_msg.reg == 1) {
			/* Registering */
			ret = register_traceable_app(reg_msg.pid, reg_msg.uid);
			if (ret < 0) {
				/* register_traceable_app only return an error with
				 * ENOMEM. At this point, we better stop everything.
				 */
				goto error;
			}
		} else {
			/* Unregistering */
			unregister_traceable_app(reg_msg.pid);
		}
	}

error:

	return NULL;
}

/*
 *  spawn_kconsumerd_thread
 *
 *  Start the thread_manage_kconsumerd. This must be done after a kconsumerd
 *  exec or it will fails.
 */
static int spawn_kconsumerd_thread(void)
{
	int ret;

	/* Setup semaphore */
	sem_init(&kconsumerd_sem, 0, 0);

	ret = pthread_create(&kconsumerd_thread, NULL, thread_manage_kconsumerd, (void *) NULL);
	if (ret != 0) {
		perror("pthread_create kconsumerd");
		goto error;
	}

	/* Wait for the kconsumerd thread to be ready */
	sem_wait(&kconsumerd_sem);

	if (kconsumerd_pid == 0) {
		ERR("Kconsumerd did not start");
		goto error;
	}

	return 0;

error:
	ret = LTTCOMM_KERN_CONSUMER_FAIL;
	return ret;
}

/*
 *  spawn_kconsumerd
 *
 *  Fork and exec a kernel consumer daemon (kconsumerd).
 *
 *  NOTE: It is very important to fork a kconsumerd BEFORE opening any kernel
 *  file descriptor using the libkernelctl or kernel-ctl functions. So, a
 *  kernel consumer MUST only be spawned before creating a kernel session.
 *
 *  Return pid if successful else -1.
 */
static pid_t spawn_kconsumerd(void)
{
	int ret;
	pid_t pid;

	DBG("Spawning kconsumerd");

	pid = fork();
	if (pid == 0) {
		/*
		 * Exec kconsumerd.
		 */
		execlp("kconsumerd", "kconsumerd", "--verbose", NULL);
		if (errno != 0) {
			perror("kernel start consumer exec");
		}
		exit(EXIT_FAILURE);
	} else if (pid > 0) {
		ret = pid;
		goto error;
	} else {
		perror("kernel start consumer fork");
		ret = -errno;
		goto error;
	}

error:
	return ret;
}

/*
 *  start_kconsumerd
 *
 *  Spawn the kconsumerd daemon and session daemon thread.
 */
static int start_kconsumerd(void)
{
	int ret;

	pthread_mutex_lock(&kconsumerd_pid_mutex);
	if (kconsumerd_pid != 0) {
		goto end;
	}

	ret = spawn_kconsumerd();
	if (ret < 0) {
		ERR("Spawning kconsumerd failed");
		ret = LTTCOMM_KERN_CONSUMER_FAIL;
		pthread_mutex_unlock(&kconsumerd_pid_mutex);
		goto error;
	}

	/* Setting up the global kconsumerd_pid */
	kconsumerd_pid = ret;
	pthread_mutex_unlock(&kconsumerd_pid_mutex);

	DBG("Kconsumerd pid %d", ret);

	DBG("Spawning kconsumerd thread");
	ret = spawn_kconsumerd_thread();
	if (ret < 0) {
		ERR("Fatal error spawning kconsumerd thread");
		goto error;
	}

end:
	pthread_mutex_unlock(&kconsumerd_pid_mutex);
	return 0;

error:
	return ret;
}

/*
 *  init_kernel_tracer
 *
 *  Setup necessary data for kernel tracer action.
 */
static void init_kernel_tracer(void)
{
	/* Set the global kernel tracer fd */
	kernel_tracer_fd = open(DEFAULT_KERNEL_TRACER_PATH, O_RDWR);
	if (kernel_tracer_fd < 0) {
		WARN("No kernel tracer available");
		kernel_tracer_fd = 0;
	}

	DBG("Kernel tracer fd %d", kernel_tracer_fd);
}

/*
 *  start_kernel_trace
 *
 *  Start tracing by creating trace directory and sending FDs to the kernel
 *  consumer.
 */
static int start_kernel_trace(struct ltt_kernel_session *session)
{
	int ret;

	/* Create trace directory */
	ret = create_trace_dir(session);
	if (ret < 0) {
		if (ret == -EEXIST) {
			ret = LTTCOMM_KERN_DIR_EXIST;
		} else {
			ret = LTTCOMM_KERN_DIR_FAIL;
			goto error;
		}
	}

	if (session->kconsumer_fds_sent == 0) {
		ret = send_kconsumerd_fds(kconsumerd_cmd_sock, session);
		if (ret < 0) {
			ERR("Send kconsumerd fds failed");
			ret = LTTCOMM_KERN_CONSUMER_FAIL;
			goto error;
		}

		session->kconsumer_fds_sent = 1;
	}

error:
	return ret;
}

/*
 *  init_default_channel
 *
 *  Allocate a channel structure and fill it.
 */
static struct lttng_channel *init_default_channel(void)
{
	struct lttng_channel *chan;

	chan = malloc(sizeof(struct lttng_channel));
	if (chan == NULL) {
		perror("init channel malloc");
		goto error;
	}

	if (snprintf(chan->name, NAME_MAX, DEFAULT_CHANNEL_NAME) < 0) {
		perror("snprintf defautl channel name");
		return NULL;
	}

	chan->attr.overwrite = DEFAULT_CHANNEL_OVERWRITE;
	chan->attr.subbuf_size = DEFAULT_CHANNEL_SUBBUF_SIZE;
	chan->attr.num_subbuf = DEFAULT_CHANNEL_SUBBUF_NUM;
	chan->attr.switch_timer_interval = DEFAULT_CHANNEL_SWITCH_TIMER;
	chan->attr.read_timer_interval = DEFAULT_CHANNEL_READ_TIMER;
	chan->attr.output = DEFAULT_KERNEL_CHANNEL_OUTPUT;

error:
	return chan;
}

/*
 *  create_kernel_session
 *
 *  Create a kernel tracer session then create the default channel.
 */
static int create_kernel_session(struct ltt_session *session)
{
	int ret;
	struct lttng_channel *chan;

	DBG("Creating kernel session");

	ret = kernel_create_session(session, kernel_tracer_fd);
	if (ret < 0) {
		ret = LTTCOMM_KERN_SESS_FAIL;
		goto error;
	}

	chan = init_default_channel();
	if (chan == NULL) {
		ret = LTTCOMM_FATAL;
		goto error;
	}

	DBG("Creating default kernel channel %s", DEFAULT_CHANNEL_NAME);

	ret = kernel_create_channel(session->kernel_session, chan);
	if (ret < 0) {
		ret = LTTCOMM_KERN_CHAN_FAIL;
		goto error;
	}

error:
	return ret;
}

/*
 * 	process_client_msg
 *
 *  Process the command requested by the lttng client within the command
 *  context structure.  This function make sure that the return structure (llm)
 *  is set and ready for transmission before returning.
 *
 * 	Return any error encountered or 0 for success.
 */
static int process_client_msg(struct command_ctx *cmd_ctx)
{
	int ret;

	DBG("Processing client command %d", cmd_ctx->lsm->cmd_type);

	/* Listing commands don't need a session */
	switch (cmd_ctx->lsm->cmd_type) {
	case LTTNG_CREATE_SESSION:
	case LTTNG_LIST_SESSIONS:
	case LTTNG_LIST_EVENTS:
	case LTTNG_KERNEL_LIST_EVENTS:
	case LTTNG_LIST_TRACEABLE_APPS:
		break;
	default:
		DBG("Getting session %s by name", cmd_ctx->lsm->session_name);
		cmd_ctx->session = find_session_by_name(cmd_ctx->lsm->session_name);
		if (cmd_ctx->session == NULL) {
			/* If session name not found */
			if (cmd_ctx->lsm->session_name != NULL) {
				ret = LTTCOMM_SESS_NOT_FOUND;
			} else {	/* If no session name specified */
				ret = LTTCOMM_SELECT_SESS;
			}
			goto error;
		}
		break;
	}

	/*
	 * Check kernel command for kernel session.
	 */
	switch (cmd_ctx->lsm->cmd_type) {
	case LTTNG_KERNEL_ADD_CONTEXT:
	case LTTNG_KERNEL_CREATE_CHANNEL:
	case LTTNG_KERNEL_DISABLE_ALL_EVENT:
	case LTTNG_KERNEL_DISABLE_CHANNEL:
	case LTTNG_KERNEL_DISABLE_EVENT:
	case LTTNG_KERNEL_ENABLE_ALL_EVENT:
	case LTTNG_KERNEL_ENABLE_CHANNEL:
	case LTTNG_KERNEL_ENABLE_EVENT:
	case LTTNG_KERNEL_LIST_EVENTS:
		/* Kernel tracer check */
		if (kernel_tracer_fd == 0) {
			init_kernel_tracer();
			if (kernel_tracer_fd == 0) {
				ret = LTTCOMM_KERN_NA;
				goto error;
			}
		}

		/* Need a session for kernel command */
		if (cmd_ctx->lsm->cmd_type != LTTNG_KERNEL_LIST_EVENTS &&
				cmd_ctx->session->kernel_session == NULL) {

			ret = create_kernel_session(cmd_ctx->session);
			if (ret < 0) {
				ret = LTTCOMM_KERN_SESS_FAIL;
				goto error;
			}

			/* Start the kernel consumer daemon */
			if (kconsumerd_pid == 0) {
				ret = start_kconsumerd();
				if (ret < 0) {
					goto error;
				}
			}
		}
	}

	/* Connect to ust apps if available pid */
	if (cmd_ctx->lsm->pid > 0) {
		/* Connect to app using ustctl API */
		cmd_ctx->ust_sock = ust_connect_app(cmd_ctx->lsm->pid);
		if (cmd_ctx->ust_sock < 0) {
			ret = LTTCOMM_NO_TRACEABLE;
			goto error;
		}
	}

	/* Process by command type */
	switch (cmd_ctx->lsm->cmd_type) {
	case LTTNG_KERNEL_ADD_CONTEXT:
	{
		int found = 0, no_event = 0;
		struct ltt_kernel_channel *chan;
		struct ltt_kernel_event *event;

		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		/* Check if event name is given */
		if (strlen(cmd_ctx->lsm->u.context.event_name) == 0) {
			no_event = 1;
		}

		if (strlen(cmd_ctx->lsm->u.context.channel_name) == 0) {
			/* Go over all channels */
			DBG("Adding context to all channels");
			cds_list_for_each_entry(chan,
					&cmd_ctx->session->kernel_session->channel_list.head, list) {
				if (no_event) {
					ret = kernel_add_channel_context(chan,
							&cmd_ctx->lsm->u.context.ctx);
					if (ret < 0) {
						continue;
					}
				} else {
					event = get_kernel_event_by_name(cmd_ctx->lsm->u.context.event_name, chan);
					if (event != NULL) {
						ret = kernel_add_event_context(event,
								&cmd_ctx->lsm->u.context.ctx);
						if (ret < 0) {
							ret = LTTCOMM_KERN_CONTEXT_FAIL;
							goto error;
						}
						found = 1;
						break;
					}
				}
			}
		} else {
			chan = get_kernel_channel_by_name(cmd_ctx->lsm->u.context.channel_name,
					cmd_ctx->session->kernel_session);
			if (chan == NULL) {
				ret = LTTCOMM_KERN_CHAN_NOT_FOUND;
				goto error;
			}

			if (no_event) {
				ret = kernel_add_channel_context(chan,
						&cmd_ctx->lsm->u.context.ctx);
				if (ret < 0) {
					ret = LTTCOMM_KERN_CONTEXT_FAIL;
					goto error;
				}
			} else {
				event = get_kernel_event_by_name(cmd_ctx->lsm->u.context.event_name, chan);
				if (event != NULL) {
					ret = kernel_add_event_context(event,
							&cmd_ctx->lsm->u.context.ctx);
					if (ret < 0) {
						ret = LTTCOMM_KERN_CONTEXT_FAIL;
						goto error;
					}
				}
			}
		}

		if (!found && !no_event) {
			ret = LTTCOMM_NO_EVENT;
			goto error;
		}

		ret = LTTCOMM_OK;
		break;
	}
	case LTTNG_KERNEL_CREATE_CHANNEL:
	{
		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		/* Kernel tracer */
		DBG("Creating kernel channel");

		ret = kernel_create_channel(cmd_ctx->session->kernel_session,
				&cmd_ctx->lsm->u.channel.chan);
		if (ret < 0) {
			ret = LTTCOMM_KERN_CHAN_FAIL;
			goto error;
		}

		ret = LTTCOMM_OK;
		break;
	}
	case LTTNG_KERNEL_DISABLE_CHANNEL:
	{
		struct ltt_kernel_channel *chan;

		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		chan = get_kernel_channel_by_name(cmd_ctx->lsm->u.disable.channel_name,
				cmd_ctx->session->kernel_session);
		if (chan == NULL) {
			ret = LTTCOMM_KERN_CHAN_NOT_FOUND;
			goto error;
		} else if (chan->enabled == 1) {
			ret = kernel_disable_channel(chan);
			if (ret < 0) {
				if (ret != EEXIST) {
					ret = LTTCOMM_KERN_CHAN_DISABLE_FAIL;
				}
				goto error;
			}
		}

		kernel_wait_quiescent(kernel_tracer_fd);
		ret = LTTCOMM_OK;
		break;
	}
	case LTTNG_KERNEL_DISABLE_EVENT:
	{
		struct ltt_kernel_channel *chan;
		struct ltt_kernel_event *ev;

		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		chan = get_kernel_channel_by_name(cmd_ctx->lsm->u.disable.channel_name,
				cmd_ctx->session->kernel_session);
		if (chan == NULL) {
			ret = LTTCOMM_KERN_CHAN_NOT_FOUND;
			goto error;
		}

		ev = get_kernel_event_by_name(cmd_ctx->lsm->u.disable.name, chan);
		if (ev != NULL) {
			DBG("Disabling kernel event %s for channel %s.",
					cmd_ctx->lsm->u.disable.name, cmd_ctx->lsm->u.disable.channel_name);
			ret = kernel_disable_event(ev);
			if (ret < 0) {
				ret = LTTCOMM_KERN_ENABLE_FAIL;
				goto error;
			}
		}

		kernel_wait_quiescent(kernel_tracer_fd);
		ret = LTTCOMM_OK;
		break;
	}
	case LTTNG_KERNEL_DISABLE_ALL_EVENT:
	{
		struct ltt_kernel_channel *chan;
		struct ltt_kernel_event *ev;

		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		DBG("Disabling all enabled kernel events");

		chan = get_kernel_channel_by_name(cmd_ctx->lsm->u.disable.channel_name,
				cmd_ctx->session->kernel_session);
		if (chan == NULL) {
			ret = LTTCOMM_KERN_CHAN_NOT_FOUND;
			goto error;
		}

		/* For each event in the kernel session */
		cds_list_for_each_entry(ev, &chan->events_list.head, list) {
			DBG("Disabling kernel event %s for channel %s.",
					ev->event->name, cmd_ctx->lsm->u.disable.channel_name);
			ret = kernel_disable_event(ev);
			if (ret < 0) {
				continue;
			}
		}

		/* Quiescent wait after event disable */
		kernel_wait_quiescent(kernel_tracer_fd);
		ret = LTTCOMM_OK;
		break;
	}
	case LTTNG_KERNEL_ENABLE_CHANNEL:
	{
		struct ltt_kernel_channel *chan;

		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		chan = get_kernel_channel_by_name(cmd_ctx->lsm->u.enable.channel_name,
				cmd_ctx->session->kernel_session);
		if (chan == NULL) {
			ret = LTTCOMM_KERN_CHAN_NOT_FOUND;
			goto error;
		} else if (chan->enabled == 0) {
			ret = kernel_enable_channel(chan);
			if (ret < 0) {
				if (ret != EEXIST) {
					ret = LTTCOMM_KERN_CHAN_ENABLE_FAIL;
				}
				goto error;
			}
		}

		kernel_wait_quiescent(kernel_tracer_fd);
		ret = LTTCOMM_OK;
		break;
	}
	case LTTNG_KERNEL_ENABLE_EVENT:
	{
		struct ltt_kernel_channel *chan;
		struct ltt_kernel_event *ev;

		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		chan = get_kernel_channel_by_name(cmd_ctx->lsm->u.enable.channel_name,
				cmd_ctx->session->kernel_session);
		if (chan == NULL) {
			ret = LTTCOMM_KERN_CHAN_NOT_FOUND;
			goto error;
		}

		ev = get_kernel_event_by_name(cmd_ctx->lsm->u.enable.event.name, chan);
		if (ev == NULL) {
			DBG("Creating kernel event %s for channel %s.",
					cmd_ctx->lsm->u.enable.event.name, chan->channel->name);
			ret = kernel_create_event(&cmd_ctx->lsm->u.enable.event, chan);
		} else {
			DBG("Enabling kernel event %s for channel %s.",
					cmd_ctx->lsm->u.enable.event.name, chan->channel->name);
			ret = kernel_enable_event(ev);
		}

		if (ret < 0) {
			ret = LTTCOMM_KERN_ENABLE_FAIL;
			goto error;
		}

		kernel_wait_quiescent(kernel_tracer_fd);
		ret = LTTCOMM_OK;
		break;
	}
	case LTTNG_KERNEL_ENABLE_ALL_EVENT:
	{
		int pos, size;
		char *event_list, *event, *ptr;
		struct ltt_kernel_channel *chan;
		struct ltt_kernel_event *ev;
		struct lttng_event ev_attr;

		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		DBG("Enabling all kernel event");

		chan = get_kernel_channel_by_name(cmd_ctx->lsm->u.enable.channel_name,
				cmd_ctx->session->kernel_session);
		if (chan == NULL) {
			ret = LTTCOMM_KERN_CHAN_NOT_FOUND;
			goto error;
		}

		/* For each event in the kernel session */
		cds_list_for_each_entry(ev, &chan->events_list.head, list) {
			DBG("Enabling kernel event %s for channel %s.",
					ev->event->name, chan->channel->name);
			ret = kernel_enable_event(ev);
			if (ret < 0) {
				continue;
			}
		}

		size = kernel_list_events(kernel_tracer_fd, &event_list);
		if (size < 0) {
			ret = LTTCOMM_KERN_LIST_FAIL;
			goto error;
		}

		ptr = event_list;
		while ((size = sscanf(ptr, "event { name = %m[^;]; };%n\n", &event, &pos)) == 1) {
			ev = get_kernel_event_by_name(event, chan);
			if (ev == NULL) {
				strncpy(ev_attr.name, event, LTTNG_SYM_NAME_LEN);
				/* Default event type for enable all */
				ev_attr.type = LTTNG_EVENT_TRACEPOINTS;
				/* Enable each single tracepoint event */
				ret = kernel_create_event(&ev_attr, chan);
				if (ret < 0) {
					/* Ignore error here and continue */
				}
			}

			/* Move pointer to the next line */
			ptr += pos + 1;
			free(event);
		}

		free(event_list);

		/* Quiescent wait after event enable */
		kernel_wait_quiescent(kernel_tracer_fd);
		ret = LTTCOMM_OK;
		break;
	}
	case LTTNG_KERNEL_LIST_EVENTS:
	{
		char *event_list;
		ssize_t size = 0;

		DBG("Listing kernel events");

		size = kernel_list_events(kernel_tracer_fd, &event_list);
		if (size < 0) {
			ret = LTTCOMM_KERN_LIST_FAIL;
			goto error;
		}

		/*
		 * Setup lttng message with payload size set to the event list size in
		 * bytes and then copy list into the llm payload.
		 */
		ret = setup_lttng_msg(cmd_ctx, size);
		if (ret < 0) {
			goto setup_error;
		}

		/* Copy event list into message payload */
		memcpy(cmd_ctx->llm->payload, event_list, size);

		free(event_list);

		ret = LTTCOMM_OK;
		break;
	}
	case LTTNG_START_TRACE:
	{
		struct ltt_kernel_channel *chan;

		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		/* Kernel tracing */
		if (cmd_ctx->session->kernel_session != NULL) {
			if (cmd_ctx->session->kernel_session->metadata == NULL) {
				DBG("Open kernel metadata");
				ret = kernel_open_metadata(cmd_ctx->session->kernel_session);
				if (ret < 0) {
					ret = LTTCOMM_KERN_META_FAIL;
					goto error;
				}
			}

			if (cmd_ctx->session->kernel_session->metadata_stream_fd == 0) {
				DBG("Opening kernel metadata stream");
				if (cmd_ctx->session->kernel_session->metadata_stream_fd == 0) {
					ret = kernel_open_metadata_stream(cmd_ctx->session->kernel_session);
					if (ret < 0) {
						ERR("Kernel create metadata stream failed");
						ret = LTTCOMM_KERN_STREAM_FAIL;
						goto error;
					}
				}
			}

			/* For each channel */
			cds_list_for_each_entry(chan, &cmd_ctx->session->kernel_session->channel_list.head, list) {
				if (chan->stream_count == 0) {
					ret = kernel_open_channel_stream(chan);
					if (ret < 0) {
						ERR("Kernel create channel stream failed");
						ret = LTTCOMM_KERN_STREAM_FAIL;
						goto error;
					}
					/* Update the stream global counter */
					cmd_ctx->session->kernel_session->stream_count_global += ret;
				}
			}

			DBG("Start kernel tracing");
			ret = kernel_start_session(cmd_ctx->session->kernel_session);
			if (ret < 0) {
				ERR("Kernel start session failed");
				ret = LTTCOMM_KERN_START_FAIL;
				goto error;
			}

			ret = start_kernel_trace(cmd_ctx->session->kernel_session);
			if (ret < 0) {
				ret = LTTCOMM_KERN_START_FAIL;
				goto error;
			}

			/* Quiescent wait after starting trace */
			kernel_wait_quiescent(kernel_tracer_fd);
		}

		/* TODO: Start all UST traces */

		ret = LTTCOMM_OK;
		break;
	}
	case LTTNG_STOP_TRACE:
	{
		struct ltt_kernel_channel *chan;
		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		/* Kernel tracer */
		if (cmd_ctx->session->kernel_session != NULL) {
			DBG("Stop kernel tracing");

			ret = kernel_metadata_flush_buffer(cmd_ctx->session->kernel_session->metadata_stream_fd);
			if (ret < 0) {
				ERR("Kernel metadata flush failed");
			}

			cds_list_for_each_entry(chan, &cmd_ctx->session->kernel_session->channel_list.head, list) {
				ret = kernel_flush_buffer(chan);
				if (ret < 0) {
					ERR("Kernel flush buffer error");
				}
			}

			ret = kernel_stop_session(cmd_ctx->session->kernel_session);
			if (ret < 0) {
				ERR("Kernel stop session failed");
				ret = LTTCOMM_KERN_STOP_FAIL;
				goto error;
			}

			/* Quiescent wait after stopping trace */
			kernel_wait_quiescent(kernel_tracer_fd);
		}

		/* TODO : User-space tracer */

		ret = LTTCOMM_OK;
		break;
	}
	case LTTNG_CREATE_SESSION:
	{
		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		ret = create_session(cmd_ctx->lsm->session_name, cmd_ctx->lsm->path);
		if (ret < 0) {
			if (ret == -EEXIST) {
				ret = LTTCOMM_EXIST_SESS;
			} else {
				ret = LTTCOMM_FATAL;
			}
			goto error;
		}

		ret = LTTCOMM_OK;
		break;
	}
	case LTTNG_DESTROY_SESSION:
	{
		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		/* Clean kernel session teardown */
		teardown_kernel_session(cmd_ctx->session);

		ret = destroy_session(cmd_ctx->lsm->session_name);
		if (ret < 0) {
			ret = LTTCOMM_FATAL;
			goto error;
		}

		ret = LTTCOMM_OK;
		break;
	}
	/*
	case LTTNG_LIST_TRACES:
	{
		unsigned int trace_count;

		trace_count = get_trace_count_per_session(cmd_ctx->session);
		if (trace_count == 0) {
			ret = LTTCOMM_NO_TRACE;
			goto error;
		}

		ret = setup_lttng_msg(cmd_ctx, sizeof(struct lttng_trace) * trace_count);
		if (ret < 0) {
			goto setup_error;
		}

		get_traces_per_session(cmd_ctx->session,
				(struct lttng_trace *)(cmd_ctx->llm->payload));

		ret = LTTCOMM_OK;
		break;
	}
	*/
	/*
	case UST_CREATE_TRACE:
	{
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		ret = ust_create_trace(cmd_ctx);
		if (ret < 0) {
			goto error;
		}
		break;
	}
	*/
	case LTTNG_LIST_TRACEABLE_APPS:
	{
		unsigned int app_count;

		app_count = get_app_count();
		DBG("Traceable application count : %d", app_count);
		if (app_count == 0) {
			ret = LTTCOMM_NO_APPS;
			goto error;
		}

		ret = setup_lttng_msg(cmd_ctx, sizeof(pid_t) * app_count);
		if (ret < 0) {
			goto setup_error;
		}

		get_app_list_pids((pid_t *)(cmd_ctx->llm->payload));

		ret = LTTCOMM_OK;
		break;
	}
	/*
	case UST_START_TRACE:
	{
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		ret = ust_start_trace(cmd_ctx);
		if (ret < 0) {
			goto setup_error;
		}
		break;
	}
	case UST_STOP_TRACE:
	{
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		ret = ust_stop_trace(cmd_ctx);
		if (ret < 0) {
			goto setup_error;
		}
		break;
	}
	*/
	case LTTNG_LIST_SESSIONS:
	{
		unsigned int session_count;

		session_count = get_session_count();
		if (session_count == 0) {
			ret = LTTCOMM_NO_SESSION;
			goto error;
		}

		ret = setup_lttng_msg(cmd_ctx, sizeof(struct lttng_session) * session_count);
		if (ret < 0) {
			goto setup_error;
		}

		get_lttng_session((struct lttng_session *)(cmd_ctx->llm->payload));

		ret = LTTCOMM_OK;
		break;
	}
	default:
		/* Undefined command */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			goto setup_error;
		}

		ret = LTTCOMM_UND;
		break;
	}

	/* Set return code */
	cmd_ctx->llm->ret_code = ret;

	return ret;

error:
	if (cmd_ctx->llm == NULL) {
		DBG("Missing llm structure. Allocating one.");
		if (setup_lttng_msg(cmd_ctx, 0) < 0) {
			goto setup_error;
		}
	}
	/* Notify client of error */
	cmd_ctx->llm->ret_code = ret;

setup_error:
	return ret;
}

/*
 * 	thread_manage_clients
 *
 * 	This thread manage all clients request using the unix
 * 	client socket for communication.
 */
static void *thread_manage_clients(void *data)
{
	int sock, ret;
	struct command_ctx *cmd_ctx;

	DBG("[thread] Manage client started");

	ret = lttcomm_listen_unix_sock(client_sock);
	if (ret < 0) {
		goto error;
	}

	/* Notify parent pid that we are ready
	 * to accept command for client side.
	 */
	if (opt_sig_parent) {
		kill(ppid, SIGCHLD);
	}

	while (1) {
		/* Blocking call, waiting for transmission */
		DBG("Accepting client command ...");
		sock = lttcomm_accept_unix_sock(client_sock);
		if (sock < 0) {
			goto error;
		}

		/* Allocate context command to process the client request */
		cmd_ctx = malloc(sizeof(struct command_ctx));

		/* Allocate data buffer for reception */
		cmd_ctx->lsm = malloc(sizeof(struct lttcomm_session_msg));
		cmd_ctx->llm = NULL;
		cmd_ctx->session = NULL;

		/*
		 * Data is received from the lttng client. The struct
		 * lttcomm_session_msg (lsm) contains the command and data request of
		 * the client.
		 */
		DBG("Receiving data from client ...");
		ret = lttcomm_recv_unix_sock(sock, cmd_ctx->lsm, sizeof(struct lttcomm_session_msg));
		if (ret <= 0) {
			continue;
		}

		// TODO: Validate cmd_ctx including sanity check for security purpose.

		/*
		 * This function dispatch the work to the kernel or userspace tracer
		 * libs and fill the lttcomm_lttng_msg data structure of all the needed
		 * informations for the client. The command context struct contains
		 * everything this function may needs.
		 */
		ret = process_client_msg(cmd_ctx);
		if (ret < 0) {
			/* TODO: Inform client somehow of the fatal error. At this point,
			 * ret < 0 means that a malloc failed (ENOMEM). */
			/* Error detected but still accept command */
			clean_command_ctx(cmd_ctx);
			continue;
		}

		DBG("Sending response (size: %d, retcode: %d)",
				cmd_ctx->lttng_msg_size, cmd_ctx->llm->ret_code);
		ret = send_unix_sock(sock, cmd_ctx->llm, cmd_ctx->lttng_msg_size);
		if (ret < 0) {
			ERR("Failed to send data back to client");
		}

		clean_command_ctx(cmd_ctx);

		/* End of transmission */
		close(sock);
	}

error:
	return NULL;
}


/*
 * usage function on stderr
 */
static void usage(void)
{
	fprintf(stderr, "Usage: %s OPTIONS\n\nOptions:\n", progname);
	fprintf(stderr, "  -h, --help                         Display this usage.\n");
	fprintf(stderr, "  -c, --client-sock PATH             Specify path for the client unix socket\n");
	fprintf(stderr, "  -a, --apps-sock PATH               Specify path for apps unix socket\n");
	fprintf(stderr, "      --kconsumerd-err-sock PATH     Specify path for the kernel consumer error socket\n");
	fprintf(stderr, "      --kconsumerd-cmd-sock PATH     Specify path for the kernel consumer command socket\n");
	fprintf(stderr, "  -d, --daemonize                    Start as a daemon.\n");
	fprintf(stderr, "  -g, --group NAME                   Specify the tracing group name. (default: tracing)\n");
	fprintf(stderr, "  -V, --version                      Show version number.\n");
	fprintf(stderr, "  -S, --sig-parent                   Send SIGCHLD to parent pid to notify readiness.\n");
	fprintf(stderr, "  -q, --quiet                        No output at all.\n");
	fprintf(stderr, "  -v, --verbose                      Verbose mode. Activate DBG() macro.\n");
}

/*
 * daemon argument parsing
 */
static int parse_args(int argc, char **argv)
{
	int c;

	static struct option long_options[] = {
		{ "client-sock", 1, 0, 'c' },
		{ "apps-sock", 1, 0, 'a' },
		{ "kconsumerd-cmd-sock", 1, 0, 0 },
		{ "kconsumerd-err-sock", 1, 0, 0 },
		{ "daemonize", 0, 0, 'd' },
		{ "sig-parent", 0, 0, 'S' },
		{ "help", 0, 0, 'h' },
		{ "group", 1, 0, 'g' },
		{ "version", 0, 0, 'V' },
		{ "quiet", 0, 0, 'q' },
		{ "verbose", 0, 0, 'v' },
		{ NULL, 0, 0, 0 }
	};

	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "dhqvVS" "a:c:g:s:E:C:", long_options, &option_index);
		if (c == -1) {
			break;
		}

		switch (c) {
		case 0:
			fprintf(stderr, "option %s", long_options[option_index].name);
			if (optarg) {
				fprintf(stderr, " with arg %s\n", optarg);
			}
			break;
		case 'c':
			snprintf(client_unix_sock_path, PATH_MAX, "%s", optarg);
			break;
		case 'a':
			snprintf(apps_unix_sock_path, PATH_MAX, "%s", optarg);
			break;
		case 'd':
			opt_daemon = 1;
			break;
		case 'g':
			opt_tracing_group = strdup(optarg);
			break;
		case 'h':
			usage();
			exit(EXIT_FAILURE);
		case 'V':
			fprintf(stdout, "%s\n", VERSION);
			exit(EXIT_SUCCESS);
		case 'S':
			opt_sig_parent = 1;
			break;
		case 'E':
			snprintf(kconsumerd_err_unix_sock_path, PATH_MAX, "%s", optarg);
			break;
		case 'C':
			snprintf(kconsumerd_cmd_unix_sock_path, PATH_MAX, "%s", optarg);
			break;
		case 'q':
			opt_quiet = 1;
			break;
		case 'v':
			opt_verbose = 1;
			break;
		default:
			/* Unknown option or other error.
			 * Error is printed by getopt, just return */
			return -1;
		}
	}

	return 0;
}

/*
 * 	init_daemon_socket
 *
 * 	Creates the two needed socket by the daemon.
 * 	    apps_sock - The communication socket for all UST apps.
 * 	    client_sock - The communication of the cli tool (lttng).
 */
static int init_daemon_socket()
{
	int ret = 0;
	mode_t old_umask;

	old_umask = umask(0);

	/* Create client tool unix socket */
	client_sock = lttcomm_create_unix_sock(client_unix_sock_path);
	if (client_sock < 0) {
		ERR("Create unix sock failed: %s", client_unix_sock_path);
		ret = -1;
		goto end;
	}

	/* File permission MUST be 660 */
	ret = chmod(client_unix_sock_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (ret < 0) {
		ERR("Set file permissions failed: %s", client_unix_sock_path);
		perror("chmod");
		goto end;
	}

	/* Create the application unix socket */
	apps_sock = lttcomm_create_unix_sock(apps_unix_sock_path);
	if (apps_sock < 0) {
		ERR("Create unix sock failed: %s", apps_unix_sock_path);
		ret = -1;
		goto end;
	}

	/* File permission MUST be 666 */
	ret = chmod(apps_unix_sock_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	if (ret < 0) {
		ERR("Set file permissions failed: %s", apps_unix_sock_path);
		perror("chmod");
		goto end;
	}

end:
	umask(old_umask);
	return ret;
}

/*
 * 	check_existing_daemon
 *
 * 	Check if the global socket is available.
 *  If yes, error is returned.
 */
static int check_existing_daemon()
{
	int ret;

	ret = access(client_unix_sock_path, F_OK);
	if (ret == 0) {
		ret = access(apps_unix_sock_path, F_OK);
	}

	return ret;
}

/*
 *  get_home_dir
 *
 *  Return pointer to home directory path using
 *  the env variable HOME.
 *
 *  Default : /tmp
 */
static const char *get_home_dir(void)
{
	const char *home_path;

	if ((home_path = (const char *) getenv("HOME")) == NULL) {
		home_path = default_home_dir;
	}

	return home_path;
}

/*
 *  set_permissions
 *
 *  Set the tracing group gid onto the client socket.
 *
 *  Race window between mkdir and chown is OK because we are going from
 *  less permissive (root.root) to more permissive (root.tracing).
 */
static int set_permissions(void)
{
	int ret;
	struct group *grp;

	/* Decide which group name to use */
	(opt_tracing_group != NULL) ?
		(grp = getgrnam(opt_tracing_group)) :
		(grp = getgrnam(default_tracing_group));

	if (grp == NULL) {
		ERR("Missing tracing group. Aborting execution.\n");
		ret = -1;
		goto end;
	}

	/* Set lttng run dir */
	ret = chown(LTTNG_RUNDIR, 0, grp->gr_gid);
	if (ret < 0) {
		ERR("Unable to set group on " LTTNG_RUNDIR);
		perror("chown");
	}

	/* lttng client socket path */
	ret = chown(client_unix_sock_path, 0, grp->gr_gid);
	if (ret < 0) {
		ERR("Unable to set group on %s", client_unix_sock_path);
		perror("chown");
	}

	/* kconsumerd error socket path */
	ret = chown(kconsumerd_err_unix_sock_path, 0, grp->gr_gid);
	if (ret < 0) {
		ERR("Unable to set group on %s", kconsumerd_err_unix_sock_path);
		perror("chown");
	}

	DBG("All permissions are set");

end:
	return ret;
}

/*
 *  create_lttng_rundir
 *
 *  Create the lttng run directory needed for all
 *  global sockets and pipe.
 */
static int create_lttng_rundir(void)
{
	int ret;

	ret = mkdir(LTTNG_RUNDIR, S_IRWXU | S_IRWXG );
	if (ret < 0) {
		if (errno != EEXIST) {
			ERR("Unable to create " LTTNG_RUNDIR);
			goto error;
		} else {
			ret = 0;
		}
	}

error:
	return ret;
}

/*
 *  set_kconsumerd_sockets
 *
 *  Setup sockets and directory needed by the kconsumerd
 *  communication with the session daemon.
 */
static int set_kconsumerd_sockets(void)
{
	int ret;

	if (strlen(kconsumerd_err_unix_sock_path) == 0) {
		snprintf(kconsumerd_err_unix_sock_path, PATH_MAX, KCONSUMERD_ERR_SOCK_PATH);
	}

	if (strlen(kconsumerd_cmd_unix_sock_path) == 0) {
		snprintf(kconsumerd_cmd_unix_sock_path, PATH_MAX, KCONSUMERD_CMD_SOCK_PATH);
	}

	ret = mkdir(KCONSUMERD_PATH, S_IRWXU | S_IRWXG);
	if (ret < 0) {
		if (errno != EEXIST) {
			ERR("Failed to create " KCONSUMERD_PATH);
			goto error;
		}
		ret = 0;
	}

	/* Create the kconsumerd error unix socket */
	kconsumerd_err_sock = lttcomm_create_unix_sock(kconsumerd_err_unix_sock_path);
	if (kconsumerd_err_sock < 0) {
		ERR("Create unix sock failed: %s", kconsumerd_err_unix_sock_path);
		ret = -1;
		goto error;
	}

	/* File permission MUST be 660 */
	ret = chmod(kconsumerd_err_unix_sock_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (ret < 0) {
		ERR("Set file permissions failed: %s", kconsumerd_err_unix_sock_path);
		perror("chmod");
		goto error;
	}

error:
	return ret;
}

/*
 *  sighandler
 *
 *  Signal handler for the daemon
 */
static void sighandler(int sig)
{
	switch (sig) {
		case SIGPIPE:
			DBG("SIGPIPE catched");
			return;
		case SIGINT:
			DBG("SIGINT catched");
			cleanup();
			break;
		case SIGTERM:
			DBG("SIGTERM catched");
			cleanup();
			break;
		default:
			break;
	}

	exit(EXIT_SUCCESS);
}

/*
 *  set_signal_handler
 *
 *  Setup signal handler for :
 *		SIGINT, SIGTERM, SIGPIPE
 */
static int set_signal_handler(void)
{
	int ret = 0;
	struct sigaction sa;
	sigset_t sigset;

	if ((ret = sigemptyset(&sigset)) < 0) {
		perror("sigemptyset");
		return ret;
	}

	sa.sa_handler = sighandler;
	sa.sa_mask = sigset;
	sa.sa_flags = 0;
	if ((ret = sigaction(SIGTERM, &sa, NULL)) < 0) {
		perror("sigaction");
		return ret;
	}

	if ((ret = sigaction(SIGINT, &sa, NULL)) < 0) {
		perror("sigaction");
		return ret;
	}

	if ((ret = sigaction(SIGPIPE, &sa, NULL)) < 0) {
		perror("sigaction");
		return ret;
	}

	DBG("Signal handler set for SIGTERM, SIGPIPE and SIGINT");

	return ret;
}

/*
 *  set_ulimit
 *
 *  Set open files limit to unlimited. This daemon can open a large number of
 *  file descriptors in order to consumer multiple kernel traces.
 */
static void set_ulimit(void)
{
	int ret;
	struct rlimit lim;

	lim.rlim_cur = 65535;
	lim.rlim_max = 65535;

	ret = setrlimit(RLIMIT_NOFILE, &lim);
	if (ret < 0) {
		perror("failed to set open files limit");
	}
}

/*
 * main
 */
int main(int argc, char **argv)
{
	int ret = 0;
	void *status;

	/* Parse arguments */
	progname = argv[0];
	if ((ret = parse_args(argc, argv) < 0)) {
		goto error;
	}

	/* Daemonize */
	if (opt_daemon) {
		ret = daemon(0, 0);
		if (ret < 0) {
			perror("daemon");
			goto error;
		}
	}

	/* Check if daemon is UID = 0 */
	is_root = !getuid();

	/* Set all sockets path */
	if (is_root) {
		ret = create_lttng_rundir();
		if (ret < 0) {
			goto error;
		}

		if (strlen(apps_unix_sock_path) == 0) {
			snprintf(apps_unix_sock_path, PATH_MAX,
					DEFAULT_GLOBAL_APPS_UNIX_SOCK);
		}

		if (strlen(client_unix_sock_path) == 0) {
			snprintf(client_unix_sock_path, PATH_MAX,
					DEFAULT_GLOBAL_CLIENT_UNIX_SOCK);
		}

		ret = set_kconsumerd_sockets();
		if (ret < 0) {
			goto error;
		}

		/* Setup kernel tracer */
		init_kernel_tracer();

		/* Set ulimit for open files */
		set_ulimit();
	} else {
		if (strlen(apps_unix_sock_path) == 0) {
			snprintf(apps_unix_sock_path, PATH_MAX,
					DEFAULT_HOME_APPS_UNIX_SOCK, get_home_dir());
		}

		/* Set the cli tool unix socket path */
		if (strlen(client_unix_sock_path) == 0) {
			snprintf(client_unix_sock_path, PATH_MAX,
					DEFAULT_HOME_CLIENT_UNIX_SOCK, get_home_dir());
		}
	}

	DBG("Client socket path %s", client_unix_sock_path);
	DBG("Application socket path %s", apps_unix_sock_path);

	/* See if daemon already exist. If any of the two
	 * socket needed by the daemon are present, this test fails
	 */
	if ((ret = check_existing_daemon()) == 0) {
		ERR("Already running daemon.\n");
		/* We do not goto error because we must not
		 * cleanup() because a daemon is already running.
		 */
		exit(EXIT_FAILURE);
	}

	if (set_signal_handler() < 0) {
		goto error;
	}

	/* Setup the needed unix socket */
	if (init_daemon_socket() < 0) {
		goto error;
	}

	/* Set credentials to socket */
	if (is_root && (set_permissions() < 0)) {
		goto error;
	}

	/* Get parent pid if -S, --sig-parent is specified. */
	if (opt_sig_parent) {
		ppid = getppid();
	}

	while (1) {
		/* Create thread to manage the client socket */
		ret = pthread_create(&client_thread, NULL, thread_manage_clients, (void *) NULL);
		if (ret != 0) {
			perror("pthread_create");
			goto error;
		}

		/* Create thread to manage application socket */
		ret = pthread_create(&apps_thread, NULL, thread_manage_apps, (void *) NULL);
		if (ret != 0) {
			perror("pthread_create");
			goto error;
		}

		ret = pthread_join(client_thread, &status);
		if (ret != 0) {
			perror("pthread_join");
			goto error;
		}
	}

	cleanup();
	exit(EXIT_SUCCESS);

error:
	cleanup();
	exit(EXIT_FAILURE);
}
