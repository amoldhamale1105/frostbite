/* The first user process (init.bin) with PID 1 */
#include "flib.h"
#include "signal.h"
#include <stddef.h>
#include <stdbool.h>

int login_shell_pid;

static int respawn(char* procname, const char* args[], int* new_pid)
{
    int pid, ret = 0;
    
    pid = fork();
    if (pid == 0)
        ret = exec(procname, args);
    else if (pid == -1){
        printf("Init process failed to respawn %s\n", procname);
        ret = 1;
    }
    if (new_pid)
        *new_pid = pid;
    
    return ret;
}

int main(void)
{
    printf("\nWelcome to %s (A minimalistic aarch64 kernel)\n", stringify_value(NAME));
    int pid = fork();
    
    if (pid == 0) /* Child process */
        exec("LOGIN.BIN", NULL);
    else if (pid == -1){
        printf("Init process failed to spawn login shell!\n");
        return 1;
    }
    /* Save the main shell PID to be respawned on exit */
    login_shell_pid = pid;
    
    /* Wait for death of own children and processes orphaned by exiting parents */
    while ((pid = wait((void*) 0)) != -1)
    {
        if (pid == login_shell_pid){
            /* Hang up all processes since the user has logged out */
            kill(-1, SIGHUP);
            sleep(50);
            if (respawn("LOGIN.BIN", NULL, &login_shell_pid) != 0){
                kill(-1, SIGTERM);
                return 1;
            }
        }
    }

    return 0;
}