// process.c
#include "process.h"
#include "parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

// Function to execute simple commands
int execute_simple(const CMD *cmd) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return errno;
    } else if (pid == 0) {
        // Child process

        // Handle I/O Redirection
        if (cmd->fromType != NONE) {
            int fd;
            if (cmd->fromType == RED_IN) {
                fd = open(cmd->fromFile, O_RDONLY);
                if (fd < 0) {
                    perror("open");
                    exit(errno);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            // Handle HERE documents (RED_IN_HERE)
            // Implementation needed
        }

        if (cmd->toType != NONE) {
            int fd;
            if (cmd->toType == RED_OUT) {
                fd = open(cmd->toFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            } else if (cmd->toType == RED_OUT_APP) {
                fd = open(cmd->toFile, O_WRONLY | O_CREAT | O_APPEND, 0644);
            } else { // RED_OUT_ERR (&>)
                fd = open(cmd->toFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                // Redirect stderr as well if needed
            }

            if (fd < 0) {
                perror("open");
                exit(errno);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }

        // Handle Local Variables
        for (int i = 0; i < cmd->nLocal; i++) {
            setenv(cmd->locVar[i], cmd->locVal[i], 1);
        }

        // Execute the command
        execvp(cmd->argv[0], cmd->argv);
        // If execvp returns, an error occurred
        perror("execvp");
        exit(errno);
    } else {
        // Parent process
        int status;
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            return errno;
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        } else {
            return 128 + WTERMSIG(status);
        }
    }
}

// Recursive process function
int process(const CMD *cmd) {
    if (cmd == NULL) {
        return 0;
    }

    switch (cmd->type) {
        case SIMPLE:
            return execute_simple(cmd);

        case PIPE: {
            int pipe_fd[2];
            if (pipe(pipe_fd) == -1) {
                perror("pipe");
                return errno;
            }

            pid_t left_pid = fork();
            if (left_pid < 0) {
                perror("fork");
                return errno;
            }

            if (left_pid == 0) {
                // Left child
                dup2(pipe_fd[1], STDOUT_FILENO);
                close(pipe_fd[0]);
                close(pipe_fd[1]);
                exit(process(cmd->left));
            }

            pid_t right_pid = fork();
            if (right_pid < 0) {
                perror("fork");
                return errno;
            }

            if (right_pid == 0) {
                // Right child
                dup2(pipe_fd[0], STDIN_FILENO);
                close(pipe_fd[0]);
                close(pipe_fd[1]);
                exit(process(cmd->right));
            }

            // Parent process
            close(pipe_fd[0]);
            close(pipe_fd[1]);

            int status;
            waitpid(left_pid, &status, 0);
            int left_status = 0;
            if (WIFEXITED(status)) {
                left_status = WEXITSTATUS(status);
            } else {
                left_status = 128 + WTERMSIG(status);
            }

            waitpid(right_pid, &status, 0);
            int right_status = 0;
            if (WIFEXITED(status)) {
                right_status = WEXITSTATUS(status);
            } else {
                right_status = 128 + WTERMSIG(status);
            }

            // For PIPE, return the rightmost command's status
            return right_status;
        }

        case SEP_AND: {
            int left_status = process(cmd->left);
            if (left_status == 0) {
                return process(cmd->right);
            }
            return left_status;
        }

        case SEP_OR: {
            int left_status = process(cmd->left);
            if (left_status != 0) {
                return process(cmd->right);
            }
            return left_status;
        }

        case SEP_END:
        case SEP_BG:
            // Handle sequencing and background execution
            // Implementation needed
            return 0;

        case SUBCMD:
            // Handle subcommands by forking a subshell
            // Implementation needed
            return 0;

        default:
            // Handle NONE or other types
            return 0;
    }
}
