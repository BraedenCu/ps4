// process.c

#include "process.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <signal.h>

// Constants for redirection types
#define NONE 0
#define RED_IN 1
#define RED_IN_HERE 2
#define RED_OUT 3
#define RED_OUT_APP 4

// Constants for command types
#define SIMPLE 1
#define PIPE 2
#define SEP_AND 3
#define SEP_OR 4
#define SEP_END 5
#define SEP_BG 6
#define SUBCMD 7

// Directory stack node
typedef struct dir_node {
    char *path;
    struct dir_node *next;
} dir_node;

// Global directory stack
dir_node *dir_stack = NULL;

// Function Prototypes
int execute_builtin(const CMD *cmd);
int execute_simple(const CMD *cmd);
int execute_pipe(const CMD *cmd);
int execute_conditional(const CMD *cmd);
int execute_background(const CMD *cmd);
int execute_subcmd(const CMD *cmd);
void push_dir(const char *path);
char* pop_dir();
void reap_zombies();

// Helper function to execute built-in commands
int execute_builtin(const CMD *cmd) {
    if (cmd->argc == 0 || cmd->argv[0] == NULL) {
        return 0;
    }

    if (strcmp(cmd->argv[0], "cd") == 0) {
        char *dir;
        if (cmd->argc < 2 || cmd->argv[1] == NULL) {
            dir = getenv("HOME");
            if (dir == NULL) {
                fprintf(stderr, "cd: HOME not set\n");
                return 1;
            }
        } else {
            dir = cmd->argv[1];
        }

        if (chdir(dir) != 0) {
            perror("cd");
            return errno;
        }
        return 0;
    }

    if (strcmp(cmd->argv[0], "pushd") == 0) {
        if (cmd->argc != 2) {
            fprintf(stderr, "pushd: expected exactly one argument\n");
            return 1;
        }

        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            perror("getcwd");
            return errno;
        }

        push_dir(cwd);

        if (chdir(cmd->argv[1]) != 0) {
            perror("pushd");
            pop_dir(); // Revert stack if chdir fails
            return errno;
        }

        // Print current directory followed by stack
        printf("%s", cwd);
        dir_node *current = dir_stack;
        while (current != NULL) {
            printf(" %s", current->path);
            current = current->next;
        }
        printf("\n");

        return 0;
    }

    if (strcmp(cmd->argv[0], "popd") == 0) {
        if (cmd->argc != 1) {
            fprintf(stderr, "popd: expected no arguments\n");
            return 1;
        }

        char *dir = pop_dir();
        if (dir == NULL) {
            fprintf(stderr, "popd: directory stack empty\n");
            return 1;
        }

        if (chdir(dir) != 0) {
            perror("popd");
            free(dir);
            return errno;
        }

        // Print current directory followed by stack
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            perror("getcwd");
            free(dir);
            return errno;
        }

        printf("%s", cwd);
        dir_node *current = dir_stack;
        while (current != NULL) {
            printf(" %s", current->path);
            current = current->next;
        }
        printf("\n");

        free(dir);
        return 0;
    }

    return -1; // Not a built-in command
}

// Helper function to push directory onto stack
void push_dir(const char *path) {
    dir_node *new_node = malloc(sizeof(dir_node));
    if (new_node == NULL) {
        perror("malloc");
        exit(errno);
    }
    new_node->path = strdup(path);
    if (new_node->path == NULL) {
        perror("strdup");
        free(new_node);
        exit(errno);
    }
    new_node->next = dir_stack;
    dir_stack = new_node;
}

// Helper function to pop directory from stack
char* pop_dir() {
    if (dir_stack == NULL) {
        return NULL;
    }
    dir_node *top = dir_stack;
    char *path = strdup(top->path);
    dir_stack = top->next;
    free(top->path);
    free(top);
    return path;
}

// Helper function to reap zombie processes
void reap_zombies() {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        fprintf(stderr, "Completed: %d (%d)\n", pid, WEXITSTATUS(status));
    }
}

// Main process function
int process(const CMD *cmd) {
    if (cmd == NULL) {
        return 0;
    }

    // Reap any zombie processes before processing new commands
    reap_zombies();

    switch (cmd->type) {
        case SIMPLE: {
            // Check for built-in commands
            int builtin_status = execute_builtin(cmd);
            if (builtin_status != -1) {
                // Built-in command was executed
                // Set $? environment variable
                char status_str[10];
                snprintf(status_str, sizeof(status_str), "%d", builtin_status);
                if (setenv("?", status_str, 1) != 0) {
                    perror("setenv");
                }
                return builtin_status;
            }

            // Handle local environment variable assignments
            for (int i = 0; i < cmd->nLocal; i++) {
                if (setenv(cmd->locVar[i], cmd->locVal[i], 1) != 0) {
                    perror("setenv");
                    // Continue setting other variables even if one fails
                }
            }

            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                return errno;
            } else if (pid == 0) {
                // Child process

                // Handle input redirection
                if (cmd->fromType != NONE) {
                    if (cmd->fromType == RED_IN) {
                        int fd_in = open(cmd->fromFile, O_RDONLY);
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
                    // HERE documents are handled in SUBCMD type
                }

                // Handle output redirection
                if (cmd->toType != NONE) {
                    if (cmd->toType == RED_OUT) {
                        int fd_out = open(cmd->toFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                        if (fd_out < 0) {
                            perror("open");
                            exit(errno);
                        }
                        if (dup2(fd_out, STDOUT_FILENO) < 0) {
                            perror("dup2");
                            exit(errno);
                        }
                        close(fd_out);
                    } else if (cmd->toType == RED_OUT_APP) {
                        int fd_out = open(cmd->toFile, O_WRONLY | O_CREAT | O_APPEND, 0666);
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
                }

                // Execute the command
                execvp(cmd->argv[0], cmd->argv);
                // If execvp returns, there was an error
                perror("execvp");
                exit(errno);
            } else {
                // Parent process
                int status;
                if (waitpid(pid, &status, 0) < 0) {
                    perror("waitpid");
                    return errno;
                }

                // Set $? environment variable
                int exit_status;
                if (WIFEXITED(status)) {
                    exit_status = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    exit_status = 128 + WTERMSIG(status);
                } else {
                    exit_status = 1; // Generic error
                }

                char status_str[10];
                snprintf(status_str, sizeof(status_str), "%d", exit_status);
                if (setenv("?", status_str, 1) != 0) {
                    perror("setenv");
                }

                return exit_status;
            }
            break;
        }

        case PIPE: {
            int pipe_fd[2];
            if (pipe(pipe_fd) < 0) {
                perror("pipe");
                return errno;
            }

            pid_t pid_left = fork();
            if (pid_left < 0) {
                perror("fork");
                return errno;
            }

            if (pid_left == 0) {
                // Child process for left command
                dup2(pipe_fd[1], STDOUT_FILENO);
                close(pipe_fd[0]);
                close(pipe_fd[1]);

                int status = process(cmd->left);
                exit(status);
            }

            pid_t pid_right = fork();
            if (pid_right < 0) {
                perror("fork");
                return errno;
            }

            if (pid_right == 0) {
                // Child process for right command
                dup2(pipe_fd[0], STDIN_FILENO);
                close(pipe_fd[0]);
                close(pipe_fd[1]);

                int status = process(cmd->right);
                exit(status);
            }

            // Parent process
            close(pipe_fd[0]);
            close(pipe_fd[1]);

            int status_left, status_right;
            if (waitpid(pid_left, &status_left, 0) < 0) {
                perror("waitpid");
            }
            if (waitpid(pid_right, &status_right, 0) < 0) {
                perror("waitpid");
            }

            // According to pipefail, the status is the status of the rightmost command
            int exit_status;
            if (WIFEXITED(status_right)) {
                exit_status = WEXITSTATUS(status_right);
            } else if (WIFSIGNALED(status_right)) {
                exit_status = 128 + WTERMSIG(status_right);
            } else {
                exit_status = 1;
            }

            char status_str[10];
            snprintf(status_str, sizeof(status_str), "%d", exit_status);
            if (setenv("?", status_str, 1) != 0) {
                perror("setenv");
            }

            return exit_status;
            break;
        }

        case SEP_AND: {
            int status = process(cmd->left);
            if (status == 0) {
                status = process(cmd->right);
            }

            char status_str[10];
            snprintf(status_str, sizeof(status_str), "%d", status);
            if (setenv("?", status_str, 1) != 0) {
                perror("setenv");
            }

            return status;
            break;
        }

        case SEP_OR: {
            int status = process(cmd->left);
            if (status != 0) {
                status = process(cmd->right);
            }

            char status_str[10];
            snprintf(status_str, sizeof(status_str), "%d", status);
            if (setenv("?", status_str, 1) != 0) {
                perror("setenv");
            }

            return status;
            break;
        }

        case SEP_BG: {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                return errno;
            }

            if (pid == 0) {
                // Child process
                // Ignore SIGINT in child if necessary
                // Execute the left command
                int status = process(cmd->left);
                exit(status);
            } else {
                // Parent process
                fprintf(stderr, "Backgrounded: %d\n", pid);
                // Do not wait for the child
                return 0;
            }
            break;
        }

        case SUBCMD: {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                return errno;
            }

            if (pid == 0) {
                // Child process (subshell)

                // Handle local environment variables
                for (int i = 0; i < cmd->nLocal; i++) {
                    if (setenv(cmd->locVar[i], cmd->locVal[i], 1) != 0) {
                        perror("setenv");
                        exit(errno);
                    }
                }

                // Handle input redirection if any
                if (cmd->fromType != NONE) {
                    if (cmd->fromType == RED_IN) {
                        int fd_in = open(cmd->fromFile, O_RDONLY);
                        if (fd_in < 0) {
                            perror("open");
                            exit(errno);
                        }
                        if (dup2(fd_in, STDIN_FILENO) < 0) {
                            perror("dup2");
                            exit(errno);
                        }
                        close(fd_in);
                    } else if (cmd->fromType == RED_IN_HERE) {
                        // Implement HERE document handling
                        // Assuming cmd->fromFile contains the HERE document content
                        int pipe_fd[2];
                        if (pipe(pipe_fd) < 0) {
                            perror("pipe");
                            exit(errno);
                        }

                        // Write the HERE document content to the write end of the pipe
                        ssize_t len = strlen(cmd->fromFile);
                        if (write(pipe_fd[1], cmd->fromFile, len) != len) {
                            perror("write");
                            exit(errno);
                        }
                        close(pipe_fd[1]);

                        // Duplicate the read end to STDIN
                        if (dup2(pipe_fd[0], STDIN_FILENO) < 0) {
                            perror("dup2");
                            exit(errno);
                        }
                        close(pipe_fd[0]);
                    }
                }

                // Handle output redirection if any
                if (cmd->toType != NONE) {
                    if (cmd->toType == RED_OUT) {
                        int fd_out = open(cmd->toFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                        if (fd_out < 0) {
                            perror("open");
                            exit(errno);
                        }
                        if (dup2(fd_out, STDOUT_FILENO) < 0) {
                            perror("dup2");
                            exit(errno);
                        }
                        close(fd_out);
                    } else if (cmd->toType == RED_OUT_APP) {
                        int fd_out = open(cmd->toFile, O_WRONLY | O_CREAT | O_APPEND, 0666);
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
                }

                // Execute the subcommand
                int status = process(cmd->left);
                exit(status);
            } else {
                // Parent process
                int status;
                if (waitpid(pid, &status, 0) < 0) {
                    perror("waitpid");
                    return errno;
                }

                // Set $? environment variable
                int exit_status;
                if (WIFEXITED(status)) {
                    exit_status = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    exit_status = 128 + WTERMSIG(status);
                } else {
                    exit_status = 1; // Generic error
                }

                char status_str[10];
                snprintf(status_str, sizeof(status_str), "%d", exit_status);
                if (setenv("?", status_str, 1) != 0) {
                    perror("setenv");
                }

                return exit_status;
            }
            break;
        }

        case SEP_END: {
            int status = process(cmd->left);
            if (cmd->right != NULL) {
                status = process(cmd->right);
            }

            char status_str[10];
            snprintf(status_str, sizeof(status_str), "%d", status);
            if (setenv("?", status_str, 1) != 0) {
                perror("setenv");
            }

            return status;
            break;
        }

        default:
            fprintf(stderr, "Unsupported command type: %d\n", cmd->type);
            return 1;
    }

    return 0;
}
