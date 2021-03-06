// process.c                                      Daniel Kim (11/29/14)
//
// Backend for Bash, a simple shell with few build-in commands,
// local variables, redirection, backgrounded commands.

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <linux/limits.h>
#include "/c/cs323/Hwk6/parse.h"

// Print error message and die with STATUS
#define error_Exit(msg, status)  perror(msg), _exit(status)



// Set $? to "STATUS" and return STATUS
static int set_status (int status)
{
    char str[20];
    sprintf(str, "%d", status);
    setenv("?",str,1);

    return status;
}


// Reap zombie processes
static void reap_zombies() {//int sig) {
    int status;
    pid_t pid;
    while ((pid = waitpid((pid_t)(-1), &status, WNOHANG)) > 0) {

        status = (WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status));
        fprintf(stderr, "Completed: %d (%d)\n",pid, status);
    }
}



// Set local variables and redirections
static void vars_redir(CMD *pcmd)
{
    // RED_IN
    if (pcmd->fromType == RED_IN) {
        int in = open(pcmd->fromFile, O_RDONLY);
        if (in == -1) 
            error_Exit(pcmd->fromFile,errno);

        dup2(in, 0);
        close(in);
    }
    
    // RED_OUT, RED_APP
    if (pcmd->toType != NONE) {
        int obits = O_CREAT | O_WRONLY;
        if (pcmd->toType == RED_APP)
            obits = obits | O_APPEND;
        else if (pcmd->toType == RED_OUT)
            obits = obits | O_TRUNC;
        
        int out = open(pcmd->toFile, obits, 0644);

        if (out == -1)
            error_Exit(pcmd->toFile,errno);

        dup2(out, 1);
        close(out);

    }
    
    // Set local variables only in this child process
    if (pcmd->nLocal > 0) {
        for (int i = 0; i < pcmd->nLocal; i++) 
            setenv(*((pcmd->locVar)+i), *((pcmd->locVal)+i), 1);
    }
}





// Execute command list CMDLIST and return status of last command executed
// Return status of process
// SKIP is true if instructed to skip current cmd (left subtree if not SIMPLE),
// whose status is SKIP_STATUS
static int execute (CMD *cmdList, int skip, int skip_status)
{
    CMD *pcmd = cmdList;
    pid_t pid;     // fork()
    int status;    // wait()
    int fd[2];     // Read and write file descriptors for pipe()
   

    // Restore default signal handling
    signal(SIGINT,SIG_DFL);


    // Reap terminated children
    reap_zombies();
    

    // SIMPLE
    if (pcmd->type == SIMPLE) {
        
        // cd
        if (strcmp(*(pcmd->argv),"cd") == 0) {
            
            if (pcmd->argc > 2) {
                fprintf(stderr, "usage: cd  OR  cd <directory-name>\n");
                return set_status(1);
            }

            else {

                // set DIR to directory
                char *dir; 
                if (pcmd->argc == 1) {
                    dir = getenv("HOME");
                    if (dir == NULL) {
                        fprintf(stderr, "cd: $HOME variable not set\n");
                        return set_status(1);
                    }
                }
                else if (pcmd->argc == 2)
                    dir = *((pcmd->argv)+1);

                if (chdir(dir) == -1) {
                    perror("cd: chdir failed");
                    return set_status(errno);
                }
                else // success
                    return set_status(0);

            }

      
        }
        
        // wait 
        else if (strcmp(*(pcmd->argv),"wait") == 0) {

            if (pcmd->argc > 1) {
                fprintf(stderr, "usage: wait\n");
                return set_status(1);
            }

            else {
                errno = 0;

                // Ignore SIGINT while waiting
                signal(SIGINT,SIG_IGN);

                while ((pid = waitpid((pid_t)(-1),&status,0))) { // stay until children done

                    if (errno == ECHILD) // no children left 
                        break;
                    else  {
                        status = (WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status));
                        fprintf(stderr, "Completed: %d (%d)\n",pid, status);
                    }
                    
                }
                signal(SIGINT,SIG_DFL);
                return set_status(0);
            }
        }




        // Other commands (dirs, external)
        else {
            if ((pid = fork()) < 0) {
                perror("SIMPLE: fork failed");
                return set_status(errno);
            }

            else if (pid == 0) {     // child
                

                // local variables and redirection
                vars_redir(pcmd);


                // Built-in command? (dirs)
                if (strcmp(*(pcmd->argv),"dirs") == 0) {
                    
                    if (pcmd->argc > 1) {
                        fprintf(stderr, "usage: dirs\n");
                        _exit(set_status(1));
                    }

                    else {
                        char *cwd = getcwd(NULL,0);
                        if (cwd == NULL) {
                            perror("dirs: getcwd failed");
                            _exit(set_status(errno));
                        }
                        else {
                            printf("%s\n",cwd);
                            fflush(stdout);
                            _exit(set_status(0));

                        }
                    }
                }


                // Run external command
                execvp(*(pcmd->argv), pcmd->argv);
                error_Exit(*(pcmd->argv),errno);
            }
            else {                   // parent
                
                // wait and ignore SIGINT
                signal(SIGINT,SIG_IGN);
                waitpid(pid, &status, 0); 


                signal(SIGINT,SIG_DFL);

                // Set $? to status
                int program_status = (WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status));

                return set_status(program_status);
            }
        }
    }
   


    // PIPE
    else if (pcmd->type == PIPE) {
        

        if ((pid = fork()) < 0) {
            perror("PIPE: fork failed");
            return set_status(errno);
        }

        // ==== CHILD ====
        // execute left subtree
        // spawn grandchild to recursve right subtree

        else if (pid == 0) {          
            

            // Pipe buffer
            if (pipe(fd) == -1)
                error_Exit("PIPE: pipe failed",errno); 
            
            
            if ((pid = fork()) < 0) {
                error_Exit("PIPE: fork failed",errno);
            }
            
            // ==== GRANDCHILD ====
            // execute right subtree
            // close pipe when complete
            // exit
            else if (pid == 0) {
                
                // read from pipe
                if (fd[0] != 0) {
                    dup2(fd[0],0);
                    close(fd[0]);
                }
                // no writing to pipe
                close(fd[1]);
                
                // drag status from end of pipeline
                int right_status = execute(pcmd->right,0,0);

                // Close pipe
                close(0);
                
                _exit(set_status(right_status));

            }
            
            // ====================


            // ==== CHILD ====
            else {
                
                // write to pipe
                if (fd[1] != 1) {
                    dup2(fd[1],1);
                    close(fd[1]);
                }
                // no reading from pipe
                close(fd[0]);
                
                // execute left subtree
                int left_status = execute(pcmd->left,0,0);
                

                // Close pipe
                close(1);

                
                // Wait for rest of pipe
                waitpid(pid, &status, 0);
                status = (WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status));


                // set STATUS to rightmost failure, or 0
                if (status != 0)
                    ;
                else if (left_status != 0)
                    status = left_status;


                _exit(set_status(status));
            }
        }
        
        // ==== PARENT ====
        // wait for child (and grandchild, and so on)
        // set status of PIPE and continue
        else {                        

            signal(SIGINT,SIG_IGN);
            waitpid(pid, &status, 0); 

            signal(SIGINT,SIG_DFL);

            status = (WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status));
            return set_status(status);
        }
    }



    // Subcommands
    else if (pcmd->type == SUBCMD) {

        if ((pid = fork()) < 0) {
            perror("SUBCMD: fork failed");
            return set_status(errno);
        }

        else if (pid == 0) { // child executes left subtree
            
            // local variables and redirection
            vars_redir(pcmd);


            _exit(execute(pcmd->left,0,0)); 
        }

        else { // parent waits for child to terminate

            signal(SIGINT,SIG_IGN);
            waitpid(pid, &status, 0); 
            
            signal(SIGINT,SIG_DFL);

            status = (WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status));
            return set_status(status);
        }
        

    }
   


    // &&, ||
    // Return status of last command
    else if (pcmd->type == SEP_AND || pcmd->type == SEP_OR) {
        
        int left_status = (skip ? skip_status : execute(pcmd->left,0,0));
        int skip_next; // skip next command?

        if (pcmd->type == SEP_AND)
            skip_next = (left_status != 0 ? 1 : 0);
        else if (pcmd->type == SEP_OR)
            skip_next = (left_status == 0 ? 1 : 0);



        if (skip_next) { 

            // === SKIP === 
            // If right subtree is SIMPLE or PIPE or SUBCMD, done
            // Otherwise, execute right subtree with SKIP==TRUE 

            CMD *right_child = pcmd->right;
            if (right_child->type == SIMPLE ||
                right_child->type == PIPE   ||
                right_child->type == SUBCMD) {

                return left_status; // last cmd = skipped cmd
            }

            else
                return execute(right_child,1,left_status);
        }


        else 
            return execute(pcmd->right,0,0);
    }

    // ;
    // Return status of command following ; if exists, or left cmd otherwise
    else if (pcmd->type == SEP_END) {

        int last_status = execute(pcmd->left,0,0);
        if (pcmd->right != NULL)   // execute right subtree if exists
            last_status = execute(pcmd->right,0,0);

        return last_status;
    }
    
    // Backgrounded commands
    else if (pcmd->type == SEP_BG) {

        if ((pid = fork()) < 0) {
            perror("SEP_BG: fork failed");
            return set_status(errno);
        }

        else if (pid == 0) { // child executes left subtree
            _exit(execute(pcmd->left,0,0)); 
        }

        else { // parent continues 
               //  (e.g., executing right subtree or return to main)
               // Return status of right subtree or 0 if nonexistent
            fprintf(stderr, "Backgrounded: %d\n", pid);
            set_status(0);
            if (pcmd->right != NULL)
                return execute(pcmd->right,0,0);

            return 0;


        }
    }

    else
        return EXIT_SUCCESS;
}

void process (CMD *cmdList) 
{
    execute(cmdList,0,0);
}
