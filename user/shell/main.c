/**
    Frostbyte kernel and operating system
    Copyright (C) 2023  Amol Dhamale <amoldhamale1105@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "shell.h"
#include "signal.h"

void sighandler(int signum)
{
    if (signum == SIGINT){
        printf("^C\n");
        interrupted = true;
        /* Re-register handler since kernel resets it to default after first invokation */
        signal(SIGINT, sighandler);
    }
}

int main(int argc, char** argv)
{
    char username[100] = {0};
    char prompt_suffix = '$';
    char cmd_buf[MAX_CMD_BUF_SIZE];
    char echo_buf[MAX_CMD_BUF_SIZE];
    int cmd_size = 0;
    /* Default username in case shell is invoked without the '-u' option */
    memcpy(username, "user", 4);

    if (argc > 1){
        if (strlen(argv[1]) == 2 && memcmp("-u", argv[1], 2) == 0){
            int namelen = strlen(argv[2]);
            memcpy(username, argv[2], namelen);
            username[namelen] = 0;
            if (namelen == 4 && memcmp(username, "root", namelen) == 0)
                prompt_suffix = '#';
        }
    }

    /* Register custom handler for keyboard interrupt so that the shell does not get terminated on Ctrl+C from user */
    signal(SIGINT, sighandler);

    while (1)
    {
        printf("%s@%s:~%c ", username, stringify_value(NAME), prompt_suffix);
        memset(cmd_buf, 0, sizeof(cmd_buf));
        memset(echo_buf, 0, sizeof(echo_buf));
        cmd_size = read_cmd(cmd_buf, echo_buf);
        if (interrupted){
            interrupted = false;
            continue;
        }
        
        if (cmd_size > 0){
            int cmd_pos, arg_count;
            char* cmd_ext;
            char* args[MAX_PROG_ARGS];
            arg_count = get_cmd_info(cmd_buf, echo_buf, &cmd_pos, &cmd_ext, args);
            if (cmd_ext == NULL){
                char* cmd_end = cmd_buf+cmd_pos+strlen(cmd_buf+cmd_pos);
                memcpy(cmd_end, ".BIN", MAX_EXTNAME_BYTES+1);
                *(cmd_end+MAX_EXTNAME_BYTES+1) = 0;
            }
            else if (memcmp(cmd_ext, "BIN", MAX_EXTNAME_BYTES) != 0){
                printf("%s: not an executable\n", echo_buf+cmd_pos);
                continue;
            }
            int fd = open_file(cmd_buf+cmd_pos);
            if (fd < 0)
                printf("%s: command not found\n", echo_buf+cmd_pos);
            else{
                close_file(fd);
                int cmd_pid = fork();
                if (cmd_pid == 0)
                    exec(cmd_buf+cmd_pos, (const char**)args);
                else{
                    /* Don't make the parent wait since it's a background process, so that the shell becomes available to subsequent commands */
                    if (arg_count > 0 && strlen(args[arg_count-1]) == 1 && args[arg_count-1][0] == '&'){
                        printf("[%s] %d\n", echo_buf+cmd_pos, cmd_pid);
                        continue;
                    }
                    waitpid(cmd_pid, NULL, 0);
                }
            }
        }
    }
    
    return 0;
}