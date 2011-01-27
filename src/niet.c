#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

#define LOGGER_COMMAND "logger"

int close_on_exec(int fileno) {
	int result = fcntl(fileno, F_GETFD, 0);
	if (result < 0) return result;
	return fcntl(fileno, F_SETFD, result | FD_CLOEXEC);
}

int pipe_to_logger(char* const log_priority, char* const log_tag, int fileno) {
	int result;
	int pipe_handles[2];
	pid_t child;
	
	// create a pipe to talk to the child logger process over
	if (pipe(pipe_handles) < 0) {
		perror("Couldn't create a pipe");
		return -1;
	}
	
	// fork to give us a new process to run the logger
	child = fork();
	if (child < 0) {
		// hit the process limit
		close(pipe_handles[0]);
		close(pipe_handles[1]);
		perror("Couldn't fork to start logger");
		return -1;
		
	} else if (child == 0) {
		// we are the child; we're meant to run logger, which only needs stdin and produces (hopefully) no output.
		
		// the logger shouldn't ever output itself, and it really should close stdout and stderr after it starts 
		// up successfully.  but at least some common implementations don't, and we don't want loggers to hold
		// open pipes to previous loggers, since then we'd end up with a linked list of logger processes which
		// wouldn't go away until the last one has its inputs closed.  we don't want to close our output just yet
		// though, because we want somewhere to complain to if our execvp call fails, so instead we turn on the
		// close-on-exec flag for them.  the logger processes themselves still have nowhere to complain to, but
		// that's unavoidable - where could they send it?
		close_on_exec(STDOUT_FILENO); // ignore errors if these have already been closed
		close_on_exec(STDERR_FILENO);
		
		do { result = dup2(pipe_handles[0], STDIN_FILENO); } while (result < 0 && errno == EINTR); // closes STDIN_FILENO
		if (result < 0) {
			perror("Couldn't attach the pipe to the logger input");
			return -1;
		}
		
		close(pipe_handles[0]); // now it's been dupd, we don't need the allocated descriptor
		close(pipe_handles[1]); // and we won't write into the pipe, and want to notice when the actual writer closes it

		// execute the logger program, replacing this program
		char* arguments[] = {LOGGER_COMMAND, "-p", log_priority, "-t", log_tag, NULL};
		execvp(LOGGER_COMMAND, arguments); // only returns if there's an error
		perror("Couldn't execute logger");
		return -1;
		
	} else {
		// we are the parent; we can write to pipe_handles[1], and it'll go to the child logger process
		do { result = dup2(pipe_handles[1], fileno); } while (result < 0 && errno == EINTR); // closes the original fileno, if it's currently open
		if (result < 0) {
			perror("Couldn't attach the pipe to the logger output");
			return -1;
		}

		close(pipe_handles[1]); // now it's been dupd, we don't need the allocated descriptor
		close(pipe_handles[0]); // and we won't read from the pipe
		return 0;
	}
}

int main(int argc, char* argv[]){
	time_t start_time, end_time;
	pid_t child, dead;
	int status;
	
	char* log_tag = argv[1];
	char* stdout_pri = argv[2];
	char* stderr_pri = argv[3];
	char** program_arguments = argv + 4;
	
	if (argc < 5) {
		fprintf(stderr, "%s",
			"Usage: niet someprogram user.info user.err /usr/bin/someprogram\n" \
		    " - runs /usr/bin/someprogram, piping its stdout to a logger with priority\n" \
		    "   user.info and tag someprogram, and piping its stderr to a logger with\n" \
		    "   priority user.err and tag someprogram.  restarts someprogram if it dies,\n" \
			"   waiting for up to a minute first if it's dying quickly.\n");
		return 100;
	}
	
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	
	while (1) {
		if (pipe_to_logger(stdout_pri, log_tag, STDOUT_FILENO) < 0) return 1;
		if (pipe_to_logger(stderr_pri, log_tag, STDERR_FILENO) < 0) return 2;
		
		// so we're the parent process, and our stdout now goes to one logger process's stdin, and
		// our stderr now goes to a second logger process's stdout.
		fprintf(stdout, "Running %s\n", program_arguments[0]);
		start_time = time(NULL);
		child = fork();
		if (child < 0) {
			perror("Couldn't fork to start program");
			return 3;
			
		} else if (child == 0) {
			// we are the child; run the target program
			execvp(program_arguments[0], program_arguments); // argv[argc] is supposed to be 0, so fine to pass to execvp
			perror("Couldn't execute program");
			return 4;
			
		} else {
			// we are the parent; wait for the child to exit, ignoring terminating loggers
			do {
				dead = wait(&status);
			} while (dead != child);

			if (WIFEXITED(status)) {
				if (WEXITSTATUS(status) == 0) {
					fprintf(stdout, "%s finished successfully\n", program_arguments[0]);
				} else {
					fprintf(stderr, "%s exited with status %d\n", program_arguments[0], WEXITSTATUS(status));
				}
			} else {
				fprintf(stderr, "%s was terminated by signal %d\n", program_arguments[0], WTERMSIG(status));
			}
			
			// if the child exited in t < 60 seconds, wait 60-t seconds before starting it again
			end_time = time(NULL);
			if (end_time >= start_time && // should generally always be true, but clocks can be reset...
				end_time < start_time + 60) {
				int wait_s = 60 - (end_time - start_time);
				fprintf(stdout, "Waiting %ds before respawning %s\n", wait_s, program_arguments[0]);
				sleep(wait_s);
			}
		}
	}
	return 0;
}
