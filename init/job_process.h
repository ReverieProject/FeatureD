/* upstart
 *
 * Copyright © 2008 Canonical Ltd.
 * Author: Scott James Remnant <scott@netsplit.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef INIT_JOB_PROCESS_H
#define INIT_JOB_PROCESS_H

#include <sys/types.h>

#include <nih/macros.h>
#include <nih/child.h>
#include <nih/error.h>

#include "process.h"
#include "job_class.h"
#include "job.h"


/**
 * JobProcessErrorType:
 *
 * These constants represent the different steps of process spawning that
 * can produce an error.
 **/
typedef enum job_process_error_type {
	JOB_PROCESS_ERROR_FORK,
	JOB_PROCESS_ERROR_CONSOLE,
	JOB_PROCESS_ERROR_RLIMIT,
	JOB_PROCESS_ERROR_PRIORITY,
	JOB_PROCESS_ERROR_OOM_ADJ,
	JOB_PROCESS_ERROR_CHROOT,
	JOB_PROCESS_ERROR_CHDIR,
	JOB_PROCESS_ERROR_PTRACE,
	JOB_PROCESS_ERROR_EXEC
} JobProcessErrorType;

/**
 * JobProcessError:
 * @error: ordinary NihError,
 * @type: specific error,
 * @arg: relevant argument to @type,
 * @errnum: system error number.
 *
 * This structure builds on NihError to include additional fields useful
 * for an error generated by spawning a process.  @error includes the single
 * error number and human-readable message which are sufficient for many
 * purposes.
 *
 * @type indicates which step of the spawning process failed, @arg is any
 * information relevant to @type (such as the resource limit that could not
 * be set) and @errnum is the actual system error number.
 *
 * If you receive a JOB_PROCESS_ERROR, the returned NihError structure is
 * actually this structure and can be cast to get the additional fields.
 **/
typedef struct job_process_error {
	NihError            error;
	JobProcessErrorType type;
	int                 arg;
	int                 errnum;
} JobProcessError;


NIH_BEGIN_EXTERN

int    job_process_run     (Job *job, ProcessType process);

pid_t  job_process_spawn   (JobClass *class, char * const argv[],
			    char * const *env, int trace)
	__attribute__ ((warn_unused_result));

void   job_process_kill    (Job *job, ProcessType process);

void   job_process_handler (void *ptr, pid_t pid,
			    NihChildEvents event, int status);

Job   *job_process_find    (pid_t pid, ProcessType *process);

NIH_END_EXTERN

#endif /* INIT_JOB_PROCESS_H */
