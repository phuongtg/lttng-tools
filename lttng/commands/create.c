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
#include <popt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "cmd.h"
#include "conf.h"
#include "utils.h"

static char *opt_output_path;
static char *opt_session_name;

enum {
	OPT_HELP = 1,
};

static struct poptOption long_options[] = {
	/* longName, shortName, argInfo, argPtr, value, descrip, argDesc */
	{"help",      'h', POPT_ARG_NONE, 0, OPT_HELP, 0, 0},
	{"output",    'o', POPT_ARG_STRING, &opt_output_path, 0, 0, 0},
	{0, 0, 0, 0, 0, 0, 0}
};

/*
 * usage
 */
static void usage(FILE *ofp)
{
	fprintf(ofp, "usage: lttng create [options] [NAME]\n");
	fprintf(ofp, "\n");
	fprintf(ofp, "  -h, --help           Show this help\n");
	fprintf(ofp, "  -o, --output PATH    Specify output path\n");
	fprintf(ofp, "\n");
}

/*
 *  create_session
 *
 *  Create a tracing session. If no name specified, a default name will be
 *  generated.
 */
static int create_session()
{
	int ret;
	char name[NAME_MAX];
	char *session_name, *path = NULL, *alloc_path;
	time_t rawtime;
	struct tm *timeinfo;

	/* Auto session name creation */
	if (opt_session_name == NULL) {
		time(&rawtime);
		timeinfo = localtime(&rawtime);
		strftime(name, sizeof(name), "auto-%Y%m%d-%H%M%S", timeinfo);
		session_name = name;
		DBG("Auto session name set to %s", session_name);
	} else {
		session_name = opt_session_name;
	}

	/* Auto output path */
	if (opt_output_path == NULL) {
		alloc_path = config_get_default_path();
		if (alloc_path == NULL) {
			ERR("Home path not found.\n \
				 Please specify an output path using -o, --output PATH");
			ret = CMD_FATAL;
			goto error;
		}
	} else {
		alloc_path = opt_output_path;
	}

	path = config_generate_dir_path(alloc_path);
	if (path == NULL) {
		ret = CMD_FATAL;
		goto error;
	}

	/* Init lttng session config */
	ret = config_init(path);
	if (ret < 0) {
		goto error;
	}

	ret = config_add_session_name(path, session_name);
	if (ret < 0) {
		goto error;
	}

	ret = lttng_create_session(session_name, path);
	if (ret < 0) {
		goto error;
	}

	MSG("Session %s created.", session_name);
	MSG("Working directory of created session is %s/%s", path, session_name);

	ret = CMD_SUCCESS;

error:
	if (alloc_path) {
		free(alloc_path);
	}

	if (path) {
		free(path);
	}
	return ret;
}

/*
 *  cmd_list
 *
 *  The 'list <options>' first level command
 */
int cmd_create(int argc, const char **argv)
{
	int opt, ret = CMD_SUCCESS;
	static poptContext pc;

	pc = poptGetContext(NULL, argc, argv, long_options, 0);
	poptReadDefaultConfig(pc, 0);

	while ((opt = poptGetNextOpt(pc)) != -1) {
		switch (opt) {
		case OPT_HELP:
			usage(stderr);
			goto end;
		default:
			usage(stderr);
			ret = CMD_UNDEFINED;
			goto end;
		}
	}

	opt_session_name = (char*) poptGetArg(pc);

	ret = create_session();

end:
	return ret;
}