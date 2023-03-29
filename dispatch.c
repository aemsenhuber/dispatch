/**
 * dispatch - Run a sequence of commands with a determined concurrency
 *
 * Copyright 2023 Alexandre Emsenhuber
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h> /* For PIPE_BUF */
#include <errno.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <time.h>

int run_command( char* const* args, int* fd, sigset_t* mask )  {
	const char* name = args[0];

	int pipefds[2];
	int piperes = pipe( pipefds );
	if ( piperes == -1 ) {
		perror( "pipe" );
		return -3;
	}

	pid_t pid = fork();
	if ( pid == -1 ) {
		/* error */
		close( pipefds[0] );
		close( pipefds[1] );
		return -1;
	} else if ( pid == 0 ) {
		/* child */

		/* Reset signal mask */
		if ( sigprocmask( SIG_UNBLOCK, mask, NULL ) == -1 ) {
			perror( "sigprocmask" );
			return 2;
		}

		/* now close all the standard streams, as we will redirect them */
		close( STDIN_FILENO );
		close( STDOUT_FILENO );
		close( STDERR_FILENO );

		/* redirect stdin to /dev/null */
		int fd_in = open( "/dev/null", O_RDWR );
		if ( fd_in != STDIN_FILENO ) {
			/* fd_in is usually 0 since it's now available... */
			dup2( fd_in, STDIN_FILENO );
			close( fd_in );
		}

		/* redirect output to pipe */
		dup2( pipefds[1], STDOUT_FILENO );
		dup2( pipefds[1], STDERR_FILENO );

		close( pipefds[0] );
		close( pipefds[1] );

		/* the following never returns, except if an error occurred */
		execvp( name, args );
		perror( "execvp" );
		exit( 1 );
	} else {
		/* parent */
		close( pipefds[1] );

		/* Do not share the file descriptor across multiple chidren processes */
		fcntl( pipefds[0], F_SETFD, FD_CLOEXEC );

		*fd = pipefds[0];

		return pid;
	}
}

struct child {
	long id;
	pid_t pid;
	int fd;
};

struct state {
	/* Next item to start */
	long next;

	/* Last item to start */
	long end;

	/* Number of concurrent tasks */
	long n_conc;

	/* Number of items for which memory was allocated */
	size_t n_alloc;

	/* Number of tasks running; also the size of the arrays */
	size_t n_running;

	/* Running tasks */
	struct child* tasks;

	/* File descriptor polling with poll(2) */
	struct pollfd* pollfds;
};

void set_n_conc( struct state* state, size_t n_conc ) {
	if ( n_conc > state->n_alloc ) {
		/* We need more memory */
		struct child* new_children = (struct child*) realloc( state->tasks, sizeof( struct child ) * n_conc );
		struct pollfd* new_pollfds = (struct pollfd*) realloc( state->pollfds, sizeof( struct pollfd ) * ( n_conc + 2 ) );
		if ( new_children == NULL || new_pollfds == NULL ) {
			/* Well, some memory failed to allocate */
			if ( state->tasks == NULL || state->pollfds == NULL ) {
				/* Not very useful to set the number of tasks to zero */
				fprintf( stderr, "Error: memory allocation failure\n" );
				exit( 1 );
			} else {
				state->n_conc = state->n_alloc;
				fprintf( stderr, "Error: memory allocation failed; setting number of concurrent tasks to %ld\n", state->n_conc );
			}
		} else {
			/* Memory allocated */
			state->n_alloc = n_conc;
			state->n_conc = n_conc;
		}
		if ( new_children != NULL ) {
			state->tasks = new_children;
		}
		if ( new_pollfds != NULL ) {
			state->pollfds = new_pollfds;
		}
	} else {
		/* We do not release memory, just update the number of tasks */
		state->n_conc = n_conc;
	}
}

void end_pid( struct state* state, pid_t pid ) {
	for ( size_t itr = 0; itr < state->n_running; itr++ ) {
		if ( state->tasks[ itr ].pid == pid ) {
			state->tasks[ itr ].pid = -1;
		}
	}
}

void send_signal( struct state* state, int sig ) {
	for ( size_t itr = 0; itr < state->n_running; itr++ ) {
		if ( state->tasks[ itr ].pid > 1 ) {
			kill( state->tasks[ itr ].pid, sig );
		}
	}
}

int main( int argc, char** argv ) {
	struct state state;
	state.next = 0;
	state.end = 0;
	state.n_conc = 0;
	state.n_running = 0;
	state.n_alloc = 0;
	state.tasks = NULL;
	state.pollfds = NULL;

	size_t shift;
	int monitor_stdin;
	int shutdown = 0;

	if ( getenv( "SLURM_JOB_ID" ) != NULL ) {
		/* We are in SLURM mode */
		if ( argc < 4 ) {
			fprintf( stderr, "Usage: %s <offset_array> <offset_index> <num> <command> [options] ...\n", argv[0] );
			return 2;
		}

		const long offset_array = strtol( argv[ 1 ], NULL, 0 );
		const long offset_index = strtol( argv[ 2 ], NULL, 0 );
		const long num = strtol( argv[ 3 ], NULL, 0 );
		const long task = strtol( getenv( "SLURM_ARRAY_TASK_ID" ), NULL, 0 );
		const long conc = strtol( getenv( "SLURM_CPUS_ON_NODE" ), NULL, 0 );

		state.next = ( offset_array + task ) * num + offset_index;
		state.end = ( offset_array + task + 1 ) * num + offset_index - 1;
		set_n_conc( &state, conc );

		shift = 4;
		monitor_stdin = 0;
	} else {
		if ( argc < 5 ) {
			fprintf( stderr, "Usage: %s <start> <stop> <num> <command> [options] ...\n", argv[0] );
			return 2;
		}

		state.next = strtol( argv[ 1 ], NULL, 0 );
		state.end = strtol( argv[ 2 ], NULL, 0 );

		const long conc = strtol( argv[ 3 ], NULL, 0 );
		set_n_conc( &state, conc );

		shift = 4;
		monitor_stdin = 1;
	}

	char num[65];
	char** args = (char**) calloc( sizeof( char* ), argc - shift + 2 );
	char* buffer = (char*) malloc( PIPE_BUF );

	if ( args == NULL || buffer == NULL ) {
		perror( "malloc" );
		return 2;
	}

	/* Prepare command line */
	size_t idx;
	for ( idx = shift; idx < argc; idx++ ) {
		args[ idx - shift ] = argv[ idx ];
	}
	args[ idx - shift ] = num;
	idx++;
	args[ idx - shift ] = NULL;

	/* Prepare signal masks */
	sigset_t mask;

	sigemptyset( &mask );

	sigaddset( &mask, SIGINT );
	sigaddset( &mask, SIGTERM );
	sigaddset( &mask, SIGUSR1 );
	sigaddset( &mask, SIGUSR2 );
	sigaddset( &mask, SIGCHLD );

	if ( sigprocmask( SIG_BLOCK, &mask, NULL ) == -1 ) {
		perror( "sigprocmask" );
		return 2;
	}

	int signal_fd = signalfd( -1, &mask, SFD_NONBLOCK | SFD_CLOEXEC );
	if ( signal_fd == -1 ) {
		perror( "signalfd" );
		return 2;
	}

	while( ( state.next <= state.end && !shutdown ) || state.n_running > 0 ) {
		while ( state.n_running < state.n_conc && state.next <= state.end && !shutdown ) {
			snprintf( num, 64, "%ld", state.next );

			int fd;
			int res = run_command( args, &fd, &mask );

			if ( res > 0 ) {
				state.tasks[ state.n_running ].id = state.next;
				state.tasks[ state.n_running ].pid = res;
				state.tasks[ state.n_running ].fd = fd;
				state.n_running++;
			} else {
				fprintf( stderr, "Could not start child %ld\n", state.next );
				break;
			}

			state.next++;
		}

		nfds_t nfds = 2;

		/* Standard input */
		state.pollfds[ 0 ].fd = STDIN_FILENO;
		state.pollfds[ 0 ].events = monitor_stdin ? POLLIN : 0;
		state.pollfds[ 0 ].revents = 0;

		/* Signals */
		state.pollfds[ 1 ].fd = signal_fd;
		state.pollfds[ 1 ].events = POLLIN;
		state.pollfds[ 1 ].revents = 0;

		for ( size_t itr = 0; itr < state.n_running; itr++ ) {
			state.pollfds[ nfds ].fd = state.tasks[ itr ].fd;
			state.pollfds[ nfds ].events = POLLIN;
			state.pollfds[ nfds ].revents = 0;
			nfds++;
		}

		int num = poll( state.pollfds, nfds, -1 );
		if ( num < 0 ) {
			if ( errno == EINTR ) continue;
			perror( "poll" );
			return 1;
		}

		if ( state.pollfds[ 0 ].revents & POLLIN ) {
			char* lineptr = NULL;
			size_t linelen = 0;

			ssize_t nread = getline( &lineptr, &linelen, stdin );

			if ( nread > 0 ) {
				char* name = strtok( lineptr, " " );
				if ( strcasecmp( name, "n" ) == 0 || strcasecmp( name, "conc" ) == 0 ) {
					char* num = strtok( NULL, " " );
					if ( num == NULL ) {
						fprintf( stderr, "Error: %s command requires an argument\n", name );
					} else {
						char* endptr;
						long new_conc = strtol( num, &endptr, 0 );
						if ( num == endptr ) {
							fprintf( stderr, "Error: could not parse number %s\n", num );
						} else if ( new_conc < 0 ) {
							fprintf( stderr, "Error: number must be positive\n", num );
						} else {
							set_n_conc( &state, new_conc );
						}
					}
				} else if ( strcasecmp( name, "e" ) == 0 || strcasecmp( name, "end" ) == 0 ) {
					char* num = strtok( NULL, " " );
					if ( num == NULL ) {
						fprintf( stderr, "Error: %s command requires an argument\n", name );
					} else {
						char* endptr;
						long new_end = strtol( num, &endptr, 0 );
						if ( num == endptr ) {
							fprintf( stderr, "Error: could not parse number %s\n", num );
						} else if ( new_end < state.next - 1 ) {
							fprintf( stderr, "Error: number must be at least current state\n", num );
						} else {
							state.end = new_end;
						}
					}
				} else {
					fprintf( stderr, "No such command\n" );
				}
			}

			free( lineptr );
		}

		if ( state.pollfds[ 1 ].revents & POLLIN ) {
			while ( 1 ) {
				struct signalfd_siginfo sig_data;
				int res = read( signal_fd, (void*) &sig_data, sizeof( sig_data ) );

				if ( res == -1 ) {
					if ( errno == EAGAIN || errno == EWOULDBLOCK ) break;
					perror( "read" );
					return 1;
				}

				if ( sig_data.ssi_signo == SIGCHLD && ( sig_data.ssi_code == CLD_EXITED || sig_data.ssi_code == CLD_KILLED || sig_data.ssi_code == CLD_DUMPED ) ) {
					int wstatus;
					waitpid( sig_data.ssi_pid, &wstatus, WNOHANG );
					end_pid( &state, sig_data.ssi_pid );
				} else if ( sig_data.ssi_signo == SIGINT || sig_data.ssi_signo == SIGTERM ) {
					char timebuf[200];
					size_t lstime = 0;
					time_t utime = time( NULL );
					struct tm* ltime = localtime( &utime );
					if ( ltime != NULL ) {
						lstime = strftime( timebuf, sizeof( timebuf ), "%F %T", ltime );
					}

					siginfo_t info_data;
					memset( &info_data, '\0', sizeof( info_data ) );

					info_data.si_signo = sig_data.ssi_signo;
					info_data.si_errno = sig_data.ssi_errno;
					info_data.si_code = sig_data.ssi_code;
					info_data.si_pid = sig_data.ssi_pid;
					info_data.si_uid = sig_data.ssi_uid;
					info_data.si_fd = sig_data.ssi_fd;
					info_data.si_band = sig_data.ssi_band;
					info_data.si_overrun = sig_data.ssi_overrun;
					info_data.si_status = sig_data.ssi_status;
					info_data.si_int = sig_data.ssi_int;

					if ( lstime > 0 ) {
						psiginfo( &info_data, timebuf );
					} else {
						psiginfo( &info_data, NULL );
					}

					shutdown = 1;
					send_signal( &state, sig_data.ssi_signo );
				} else {
					send_signal( &state, sig_data.ssi_signo );
				}
			}
		} else if ( state.pollfds[ 1 ].revents ) {
			fprintf( stderr, "Signal FD is in error state. This should not happen. Stopping.\n" );
			return 1;
		}

		while( 1 ) {
			/* For some reason there seems to be deadlock issues with signalfd */
			/* So we need to do this as well... */
			int wstatus;
			pid_t ended = waitpid( -1, &wstatus, WNOHANG );
			if ( ended == -1 ) {
				if ( errno == EINTR ) continue;
				if ( errno == ECHILD ) break;
				perror( "waitpid" );
				return 1;
			} else if ( ended == 0 ) {
				break;
			} else if ( WIFEXITED( wstatus ) || WIFSIGNALED( wstatus ) ) {
				end_pid( &state, ended );
			}
		}

		for ( size_t itr = 0; itr < state.n_running; itr++ ) {
			if ( state.pollfds[ itr + 2 ].revents & POLLIN ) {
				int res = read( state.tasks[ itr ].fd, buffer, PIPE_BUF );
				if ( res == 0 ) {
					close( state.tasks[ itr ].fd );
					state.tasks[ itr ].fd = -1;
				} else if ( res > 0 ) {
					int offset = 0;
					while ( offset < res ) {
						printf( "Child %lu: ", state.tasks[ itr ].id );
						char* next = (char*) memchr( buffer + offset, '\n', res - offset );
						if ( next == NULL ) {
							fwrite( buffer + offset, res - offset, 1, stdout );
							printf( "\n" );
							break;
						} else {
							fwrite( buffer + offset, next - buffer - offset + 1, 1, stdout );
							offset = next - buffer + 1;
						}
					}

					fflush( stdout );
				}
			} else if ( state.pollfds[ itr + 2 ].revents ) {
				close( state.tasks[ itr ].fd );
				state.tasks[ itr ].fd = -1;
			}
		}

		/* Garbage collection */
		size_t itr = 0;
		while ( itr < state.n_running ) {
			if ( state.tasks[ itr ].pid < 0 && state.tasks[ itr ].fd < 0 ) {
				if ( itr < state.n_running - 1 ) {
					memmove( state.tasks + itr, state.tasks + itr + 1, sizeof( struct child ) * ( state.n_running - itr - 1 ) );
				}
				state.n_running--;
				state.tasks[ state.n_running ].id = 0;
				state.tasks[ state.n_running ].pid = 0;
				state.tasks[ state.n_running ].fd = 0;
			} else {
				itr++;
			}
		}
	}

	free( buffer );
	free( args );
	free( state.pollfds );
	free( state.tasks );
}
