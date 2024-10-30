#include "process.h"
#include <stdio.h>

int process(const CMD *cmd) {
	if (cmd == NULL) {
		return 0;
	}
	if (cmd->type == SIMPLE) {
		pid_t pid = fork();
		// check for input redirection
		if (cmd->fromType != NONE) {
			// Input redirection
			int fd_in;
			if (cmd->fromType == RED_IN) {
				fd_in = open(cmd->fromFile, O_RDONLY);
			} else {
				// Handle other input redirection types if needed
			}
			if (fd_in < 0) {
				perror("open");
				exit(errno);
			}
			if (dup2(fd_in, STDIN_FILENO) < 0) {
				perror("dup2");
				exit(errno);
			}
			close(fd_in);
		}
		// check for output redirection
		if (cmd->toType != NONE) {
			// Output redirection
			int fd_out;
			if (cmd->toType == RED_OUT) {
				fd_out = open(cmd->toFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			} else if (cmd->toType == RED_OUT_APP) {
				fd_out = open(cmd->toFile, O_WRONLY | O_CREAT | O_APPEND, 0666);
			} else {
				// Handle other output redirection types if needed
			}
			if (fd_out < 0) {
				perror("open");
				exit(errno);
			}
			if (dup2(fd_out, STDOUT_FILENO) < 0) {
				perror("dup2");
				exit(errno);
			}
			close(fd_out);
		}
		if (pid < 0) {
			perror("fork");
			return errno;
		}
		else if (pid == 0) {
			execvp(cmd->argv[0], cmd->argv);
			perror("execvp");
			exit(errno);
		}
		else {
			int status;
			if (waitpid(pid, &status, 0) < 0) {
				perror("waitpid");
				return errno;
			}
			int exit_status = WEXITSTATUS(status);

			char status_str[10];
			snprintf(status_str, sizeof(status_str), "%d", exit_status);

			if (setenv("?", status_str, 1) < 0) {
				perror("setenv");
			}

			return exit_status;
		}
	}
	printf("Unsupported command type: %d\n", cmd->type);
	return 0;
}
