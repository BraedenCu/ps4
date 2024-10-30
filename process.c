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
#include <signal.h>
#include <limits.h>
#include <stdbool.h>

// Define global variables or structures as needed

// Structure for directory stack (used by pushd and popd)
typedef struct DirNode {
    char *path;
    struct DirNode *next;
} DirNode;

DirNode *dir_stack = NULL;

// Function Prototypes
int execute_simple(const CMD *cmd);
int handle_builtin(const CMD *cmd);
int update_status(int status);
void reap_zombies();
void sigchld_handler(int sig);
void pushd_stack(const char *path);
char* popd_stack();
void print_dir_stack();

// Initialize signal handler for SIGCHLD to reap zombie processes
__attribute__((constructor)) void init_signal_handler() {
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

// Signal handler for SIGCHLD
void sigchld_handler(int sig) {
    (void)sig; // Unused parameter
    reap_zombies();
}

// Function to execute simple commands
int execute_simple(const CMD *cmd) 
{
    // Handle built-in commands first
    int builtin_status = handle_builtin(cmd);
    if (builtin_status != -1) {
        // Built-in command was executed
        return builtin_status;
    }

    pid_t pid = fork();
    if (pid < 0) 
    {
        perror("fork");
        return errno;
    } 
    else if (pid == 0) 
    {
        // Child process

        // Handle I/O Redirection
        // Input Redirection
        if (cmd->fromType != NONE) 
        {
            int fd_in;
            if (cmd->fromType == RED_IN) 
            {
                fd_in = open(cmd->fromFile, O_RDONLY);
                if (fd_in < 0) 
                {
                    perror("open");
                    exit(errno);
                }
            } 
            else if (cmd->fromType == RED_IN_HERE) 
            {
                // Handle HERE Document
                // Create a temporary file
                char template[] = "/tmp/heredocXXXXXX";
                fd_in = mkstemp(template);
                if (fd_in < 0) 
                {
                    perror("mkstemp");
                    exit(errno);
                }

                // Write HERE document content to the temporary file
                size_t len = strlen(cmd->fromFile);
                if (write(fd_in, cmd->fromFile, len) != (ssize_t)len) 
                {
                    perror("write");
                    close(fd_in);
                    exit(errno);
                }

                // Reset file offset to the beginning
                if (lseek(fd_in, 0, SEEK_SET) == (off_t)-1) 
                {
                    perror("lseek");
                    close(fd_in);
                    exit(errno);
                }

                // Unlink the file so it will be deleted after closing
                if (unlink(template) == -1) 
                {
                    perror("unlink");
                    close(fd_in);
                    exit(errno);
                }
            }
            else 
            {
                // Unsupported redirection type
                fprintf(stderr, "Unsupported input redirection type\n");
                exit(EXIT_FAILURE);
            }

            // Redirect stdin
            if (dup2(fd_in, STDIN_FILENO) == -1) 
            {
                perror("dup2");
                close(fd_in);
                exit(errno);
            }
            close(fd_in);
        }

        // Output Redirection
        if (cmd->toType != NONE) 
        {
            int fd_out;
            if (cmd->toType == RED_OUT) 
            {
                fd_out = open(cmd->toFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            } 
            else if (cmd->toType == RED_OUT_APP) 
            {
                fd_out = open(cmd->toFile, O_WRONLY | O_CREAT | O_APPEND, 0644);
            } 
            else if (cmd->toType == RED_OUT_ERR) 
            {
                // Redirect both stdout and stderr
                fd_out = open(cmd->toFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd_out < 0) 
                {
                    perror("open");
                    exit(errno);
                }
                if (dup2(fd_out, STDOUT_FILENO) == -1) 
                {
                    perror("dup2");
                    close(fd_out);
                    exit(errno);
                }
                if (dup2(fd_out, STDERR_FILENO) == -1) 
                {
                    perror("dup2");
                    close(fd_out);
                    exit(errno);
                }
                close(fd_out);
                goto redirect_complete;
            }
            else 
            {
                // Unsupported redirection type
                fprintf(stderr, "Unsupported output redirection type\n");
                exit(EXIT_FAILURE);
            }

            if (fd_out < 0) 
            {
                perror("open");
                exit(errno);
            }

            // Redirect stdout
            if (dup2(fd_out, STDOUT_FILENO) == -1) 
            {
                perror("dup2");
                close(fd_out);
                exit(errno);
            }
            close(fd_out);
        }
        else if (cmd->errType != NONE) 
        {
            // Handle stderr redirection if needed in future
            // Currently unused as per specifications
        }

    redirect_complete:

        // Handle Local Variables
        for (int i = 0; i < cmd->nLocal; i++) 
        {
            if (setenv(cmd->locVar[i], cmd->locVal[i], 1) == -1) 
            {
                perror("setenv");
                exit(errno);
            }
        }

        // Execute the command
        execvp(cmd->argv[0], cmd->argv);
        // If execvp returns, an error occurred
        perror("execvp");
        exit(errno);
    } 
    else 
    {
        // Parent process
        int status;
        if (waitpid(pid, &status, 0) == -1) 
        {
            perror("waitpid");
            return errno;
        }
        if (WIFEXITED(status)) 
        {
            return WEXITSTATUS(status);
        } 
        else 
        {
            return 128 + WTERMSIG(status);
        }
    }
}

// Recursive process function
int process(const CMD *cmd) 
{
    if (cmd == NULL) 
    {
        return 0;
    }

    int status = 0;

    switch (cmd->type) 
    {
        case SIMPLE:
            status = execute_simple(cmd);
            break;

        case PIPE: 
        {
            int pipe_fd[2];
            if (pipe(pipe_fd) == -1) 
            {
                perror("pipe");
                return errno;
            }

            pid_t left_pid = fork();
            if (left_pid < 0) 
            {
                perror("fork");
                close(pipe_fd[0]);
                close(pipe_fd[1]);
                return errno;
            }

            if (left_pid == 0) 
            {
                // Left child process
                // Redirect stdout to pipe write end
                if (dup2(pipe_fd[1], STDOUT_FILENO) == -1) 
                {
                    perror("dup2");
                    exit(errno);
                }
                close(pipe_fd[0]);
                close(pipe_fd[1]);

                exit(process(cmd->left));
            }

            pid_t right_pid = fork();
            if (right_pid < 0) 
            {
                perror("fork");
                close(pipe_fd[0]);
                close(pipe_fd[1]);
                return errno;
            }

            if (right_pid == 0) 
            {
                // Right child process
                // Redirect stdin to pipe read end
                if (dup2(pipe_fd[0], STDIN_FILENO) == -1) 
                {
                    perror("dup2");
                    exit(errno);
                }
                close(pipe_fd[0]);
                close(pipe_fd[1]);

                exit(process(cmd->right));
            }

            // Parent process
            close(pipe_fd[0]);
            close(pipe_fd[1]);

            // Wait for both child processes
            int left_status, right_status;
            if (waitpid(left_pid, &left_status, 0) == -1) 
            {
                perror("waitpid");
                return errno;
            }
            if (waitpid(right_pid, &right_status, 0) == -1) 
            {
                perror("waitpid");
                return errno;
            }

            // Return the status of the rightmost command in the pipeline
            if (WIFEXITED(right_status)) 
            {
                status = WEXITSTATUS(right_status);
            } 
            else 
            {
                status = 128 + WTERMSIG(right_status);
            }
            break;
        }

        case SEP_AND: 
        {
            int left_status = process(cmd->left);
            if (left_status == 0) 
            {
                status = process(cmd->right);
            } 
            else 
            {
                status = left_status;
            }
            break;
        }

        case SEP_OR: 
        {
            int left_status = process(cmd->left);
            if (left_status != 0) 
            {
                status = process(cmd->right);
            } 
            else 
            {
                status = left_status;
            }
            break;
        }

        case SEP_END:
        {
            // Execute left command
            process(cmd->left);
            // Execute right command regardless of left's status
            status = process(cmd->right);
            break;
        }

        case SEP_BG:
        {
            pid_t pid = fork();
            if (pid < 0) 
            {
                perror("fork");
                return errno;
            } 
            else if (pid == 0) 
            {
                // Child process
                // Execute the left command
                exit(process(cmd->left));
            } 
            else 
            {
                // Parent process
                fprintf(stderr, "Backgrounded: %d\n", pid);
                // Do not wait for the child
                status = 0; // As per specification, backgrounded commands return status 0
            }

            // Continue processing the right command if any
            if (cmd->right != NULL) 
            {
                status = process(cmd->right);
            }
            break;
        }

        case SUBCMD:
        {
            pid_t pid = fork();
            if (pid < 0) 
            {
                perror("fork");
                return errno;
            } 
            else if (pid == 0) 
            {
                // Child process (subshell)

                // Handle I/O Redirection if any
                // Input Redirection
                if (cmd->fromType != NONE) 
                {
                    int fd_in;
                    if (cmd->fromType == RED_IN) 
                    {
                        fd_in = open(cmd->fromFile, O_RDONLY);
                        if (fd_in < 0) 
                        {
                            perror("open");
                            exit(errno);
                        }
                    } 
                    else if (cmd->fromType == RED_IN_HERE) 
                    {
                        // Handle HERE document
                        char template[] = "/tmp/heredocXXXXXX";
                        fd_in = mkstemp(template);
                        if (fd_in < 0) 
                        {
                            perror("mkstemp");
                            exit(errno);
                        }

                        // Write HERE document content to the temporary file
                        size_t len = strlen(cmd->fromFile);
                        if (write(fd_in, cmd->fromFile, len) != (ssize_t)len) 
                        {
                            perror("write");
                            close(fd_in);
                            exit(errno);
                        }

                        // Reset file offset to the beginning
                        if (lseek(fd_in, 0, SEEK_SET) == (off_t)-1) 
                        {
                            perror("lseek");
                            close(fd_in);
                            exit(errno);
                        }

                        // Unlink the file so it will be deleted after closing
                        if (unlink(template) == -1) 
                        {
                            perror("unlink");
                            close(fd_in);
                            exit(errno);
                        }
                    }
                    else 
                    {
                        // Unsupported redirection type
                        fprintf(stderr, "Unsupported input redirection type in subcommand\n");
                        exit(EXIT_FAILURE);
                    }

                    // Redirect stdin
                    if (dup2(fd_in, STDIN_FILENO) == -1) 
                    {
                        perror("dup2");
                        close(fd_in);
                        exit(errno);
                    }
                    close(fd_in);
                }

                // Output Redirection
                if (cmd->toType != NONE) 
                {
                    int fd_out;
                    if (cmd->toType == RED_OUT) 
                    {
                        fd_out = open(cmd->toFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    } 
                    else if (cmd->toType == RED_OUT_APP) 
                    {
                        fd_out = open(cmd->toFile, O_WRONLY | O_CREAT | O_APPEND, 0644);
                    } 
                    else if (cmd->toType == RED_OUT_ERR) 
                    {
                        // Redirect both stdout and stderr
                        fd_out = open(cmd->toFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (fd_out < 0) 
                        {
                            perror("open");
                            exit(errno);
                        }
                        if (dup2(fd_out, STDOUT_FILENO) == -1) 
                        {
                            perror("dup2");
                            close(fd_out);
                            exit(errno);
                        }
                        if (dup2(fd_out, STDERR_FILENO) == -1) 
                        {
                            perror("dup2");
                            close(fd_out);
                            exit(errno);
                        }
                        close(fd_out);
                        goto subcmd_redirect_complete;
                    }
                    else 
                    {
                        // Unsupported redirection type
                        fprintf(stderr, "Unsupported output redirection type in subcommand\n");
                        exit(EXIT_FAILURE);
                    }

                    if (fd_out < 0) 
                    {
                        perror("open");
                        exit(errno);
                    }

                    // Redirect stdout
                    if (dup2(fd_out, STDOUT_FILENO) == -1) 
                    {
                        perror("dup2");
                        close(fd_out);
                        exit(errno);
                    }
                    close(fd_out);
                }
                else if (cmd->errType != NONE) 
                {
                    // Handle stderr redirection if needed in future
                    // Currently unused as per specifications
                }

            subcmd_redirect_complete:

                // Handle Local Variables
                for (int i = 0; i < cmd->nLocal; i++) 
                {
                    if (setenv(cmd->locVar[i], cmd->locVal[i], 1) == -1) 
                    {
                        perror("setenv");
                        exit(errno);
                    }
                }

                // Process the subcommand
                exit(process(cmd->left));
            } 
            else 
            {
                // Parent process
                int subcmd_status;
                if (waitpid(pid, &subcmd_status, 0) == -1) 
                {
                    perror("waitpid");
                    return errno;
                }
                if (WIFEXITED(subcmd_status)) 
                {
                    status = WEXITSTATUS(subcmd_status);
                } 
                else 
                {
                    status = 128 + WTERMSIG(subcmd_status);
                }
            }
            break;
        }

        case NONE:
        case ERROR:
        default:
            // Unsupported or invalid command type
            fprintf(stderr, "Unsupported or invalid command type\n");
            status = 1;
            break;
    }

    // Update the $? variable
    update_status(status);

    return status;
}

// Function to handle built-in commands
int handle_builtin(const CMD *cmd) 
{
    if (cmd->type != SIMPLE || cmd->argc == 0) 
    {
        return -1; // Not a built-in command
    }

    if (strcmp(cmd->argv[0], "cd") == 0) 
    {
        // Handle cd
        if (cmd->argc == 1) 
        {
            char *home = getenv("HOME");
            if (!home) 
            {
                fprintf(stderr, "cd: HOME not set\n");
                return 1;
            }
            if (chdir(home) != 0) 
            {
                perror("cd");
                return errno;
            }
        } 
        else if (cmd->argc == 2) 
        {
            if (chdir(cmd->argv[1]) != 0) 
            {
                perror("cd");
                return errno;
            }
        } 
        else 
        {
            fprintf(stderr, "cd: too many arguments\n");
            return 1;
        }
        return 0;
    } 
    else if (strcmp(cmd->argv[0], "pushd") == 0) 
    {
        // Handle pushd
        if (cmd->argc != 2) 
        {
            fprintf(stderr, "pushd: wrong number of arguments\n");
            return 1;
        }
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) 
        {
            perror("getcwd");
            return errno;
        }
        if (chdir(cmd->argv[1]) != 0) 
        {
            perror("pushd");
            return errno;
        }
        pushd_stack(cwd);
        print_dir_stack();
        return 0;
    } 
    else if (strcmp(cmd->argv[0], "popd") == 0) 
    {
        // Handle popd
        if (cmd->argc != 1) 
        {
            fprintf(stderr, "popd: wrong number of arguments\n");
            return 1;
        }
        char *path = popd_stack();
        if (!path) 
        {
            fprintf(stderr, "popd: directory stack empty\n");
            return 1;
        }
        if (chdir(path) != 0) 
        {
            perror("popd");
            free(path);
            return errno;
        }
        free(path);
        print_dir_stack();
        return 0;
    }

    return -1; // Not a built-in command
}

// Function to update the $? environment variable
int update_status(int status) 
{
    char status_str[12];
    snprintf(status_str, sizeof(status_str), "%d", status);
    if (setenv("?", status_str, 1) != 0) 
    {
        perror("setenv");
        return errno;
    }
    return status;
}

// Function to reap zombie processes
void reap_zombies() 
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) 
    {
        WARN("Completed: %d (%d)\n", pid, (WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status)));
    }
}

// Function to push a directory onto the stack (used by pushd)
void pushd_stack(const char *path) 
{
    DirNode *new_node = malloc(sizeof(DirNode));
    if (!new_node) 
    {
        perror("malloc");
        exit(errno);
    }
    new_node->path = strdup(path);
    if (!new_node->path) 
    {
        perror("strdup");
        free(new_node);
        exit(errno);
    }
    new_node->next = dir_stack;
    dir_stack = new_node;
}

// Function to pop a directory from the stack (used by popd)
char* popd_stack() 
{
    if (!dir_stack) 
    {
        return NULL;
    }
    DirNode *top = dir_stack;
    dir_stack = dir_stack->next;
    char *path = top->path;
    free(top);
    return path;
}

// Function to print the directory stack (used by pushd and popd)
void print_dir_stack() 
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) 
    {
        perror("getcwd");
        return;
    }
    printf("%s", cwd);
    DirNode *current = dir_stack;
    while (current) 
    {
        printf(" %s", current->path);
        current = current->next;
    }
    printf("\n");
}
