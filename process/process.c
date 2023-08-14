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

#include "process.h"
#include <memory/memory.h>
#include <debug/debug.h>
#include <stddef.h>
#include <io/print.h>

static struct Process process_table[PROC_TABLE_SIZE];
static int pid_num = 1;
static struct ProcessControl pc;
static bool shutdown = false;

static struct Process* find_unused_slot(void)
{
    struct Process* process = NULL;
    /* The first process slot is reserved only for the idle process */
    for (int i = 1; i < PROC_TABLE_SIZE; i++)
    {
        if (process_table[i].state == UNUSED){
            process = process_table + i;
            break;
        }
    }
    
    return process;
}

static struct Process* alloc_new_process(void)
{
    struct Process* process;

    process = find_unused_slot();
    if (process == NULL)
        return NULL;

    memset(process->name, 0, sizeof(process->name));
    /* Allocate memory for the kernel stack. Each process will have its own kernel stack */
    process->stack = (uint64_t)kalloc();
    ASSERT(process->stack != 0);
    memset((void*)process->stack, 0, STACK_SIZE);

    process->state = INIT;
    process->status = 0;
    process->signals = 0;
    process->wpid = 0;
    /* Assign a PID and increment the global PID counter. Processes may share the same table slot but never the same PID number */
    process->pid = pid_num++;
    /* Get the context frame which is located at the top of the kernel stack */
    process->reg_context = (struct ContextFrame*)(process->stack + STACK_SIZE - sizeof(struct ContextFrame));
    /* Set the stack pointer to 12 GPRs below the context frame where the userspace context is saved */
    process->sp = (uint64_t)process->reg_context - USERSPACE_CONTEXT_SIZE;
    /* By moving 11 registers up the stack, we reach location of register x30 where we store address of trap_return 
       Since return addresses are stored in x30 in aarch64, control reaches there after executing the ret instruction in swap function
       The elr and spsr address set in the register context below will then enable trap_return to switch to EL0 correctly post an eret instruction
       NOTE This is only applicable to the first run. In subsequent runs, the process will resume execution from the point of interruption */
    *(uint64_t*)(REGISTER_POSITION(process->sp, 11)) = (uint64_t)trap_return;
    /* The return address should be set to the userspace base address */
    process->reg_context->elr = USERSPACE_BASE;
    /* In current version of the kernel, all regions (text, stack, data) of a process are expected to lie in the same 2M page  
       Hence, set the stack pointer to the top of the page from where it can grow downwards */
    process->reg_context->sp0 = USERSPACE_BASE + PAGE_SIZE;
    /* Set pstate mode field to 0 (EL0) and DAIF bits to 0 which means no masking of interrupts i.e. interrupts enabled */
    process->reg_context->spsr = 0;
    /* Allocate memory for page map which stores GDT (global directory table) for userspace */
    process->page_map = (uint64_t)kalloc();
    ASSERT(process->page_map != 0);
    memset((void*)process->page_map, 0, PAGE_SIZE);

    return process;
}

static void init_idle_process(void)
{
    struct Process* process;
    /* Allocate the first slot in the process table */
    process = process_table;
    ASSERT(process != NULL);

    process->state = RUNNING;
    process->pid = 0;
    process->daemon = true;
    /* Since this is the first process of the system, page map is initialized with current val of TTBR0 register */
    process->page_map = TO_VIRT(read_gdt());
    pc.curr_process = process;
}

static void init_user_process(void)
{
    struct Process* process;
    const char* filename = "INIT.BIN";
    
    process = alloc_new_process();
    ASSERT(process != NULL);

    ASSERT(setup_uvm(process, (char*)filename));
    memcpy(process->name, (char*)filename, strlen(filename)-(MAX_EXTNAME_BYTES+1));
    process->ppid = 0;
    process->state = READY;
    process->daemon = true;
    /* Initialize signal handlers for the init process */
    init_handlers(process);
    push_back(&pc.ready_que, (struct Node*)process);
}

void init_process(void)
{
    pc.ready_que.head = pc.ready_que.tail = NULL;
    init_idle_process();
    init_def_handlers(&pc);
    init_user_process();
}

static void switch_process(struct Process* existing, struct Process* new)
{
    /* Switch the page tables to point to the new user process memory */
    switch_vm(new->page_map);
    /* Swap the currently running process with the new process chosen by the scheduler */
    swap(&existing->sp, new->sp);
    /* The new process in previous context will resume execution here once swapped in unless it's the first time it's running
       In the first run, x30 will point to trap_return. In subsequent scheduling cycles, the return address will point here
       User processes will have this address in x30 and won't be overwritten with trap_return address in subsequent cycles
       The idle process (PID 0) will always resume here even the first time because there's no redirection defined to trap_return for it
       The rationale is that idle process always runs in kernel space i.e. EL1 not in userspace unlike other processes */
    
    /* Use the x5 register to notify the idle process in event of a system shutdown */
    if (shutdown && existing->pid == 0){
        if (existing->reg_context != NULL)
            existing->reg_context->x5 = 1;
    }
}

static void schedule(void)
{
    struct Process* old_process = pc.curr_process;
    struct Process* new_process = NULL;

    /* While returning to user mode from kernel mode, check for any pending signals on the process about to be scheduled */
    while (!empty(&pc.ready_que))
    {
        new_process = (struct Process*)front(&pc.ready_que);
        if (process_table->signals & (1 << SIGTERM))
            printk("Stopping process %s (%d)\n", new_process->name, new_process->pid);
        check_pending_signals(new_process);
        /* If the checked process is still present at the head of the queue, proceed to scheduling it */
        if ((uint64_t)new_process == (uint64_t)front(&pc.ready_que)){
            pop_front(&pc.ready_que);
            break;
        }
        /* Reset the pointer to accomodate the next process at the head of the queue */
        new_process = NULL;
    }
    /* If no other process is ready to run and the queue is empty, schedule the idle process (with below exception)
       Halt the system if the ready and wait queues are both empty and a termination signal has been issued to the idle process */
    if (empty(&pc.ready_que) && !new_process){
        if (empty(&pc.wait_list)){
            if (process_table->signals & (1 << SIGTERM)){
                shutdown = true;
                printk("Shutting down...\n");
            }
        }
        new_process = process_table;
    }

    new_process->state = RUNNING;
    pc.curr_process = new_process;
    /* Set scheduled process as current foreground process if it identifies itself as one and no other process is assuming one */
    if (!new_process->daemon && pc.fg_process == NULL)
        pc.fg_process = new_process;

    switch_process(old_process, new_process);
}

void trigger_scheduler(void)
{
    struct Process* process;

    /* Return and continue running the same process if the ready queue is empty */
    if (empty(&pc.ready_que))
        return;
    /* The current process state needs to be changed from running to ready */
    process = pc.curr_process;
    process->state = READY;

    /* The idle process (PID 0) is run by default and is also not appended to the ready queue */
    if (process->pid != 0)
        push_back(&pc.ready_que, (struct Node*)process);

    schedule();
}

struct Process *get_curr_process(void)
{
    return pc.curr_process;
}

struct Process *get_fg_process(void)
{
    if (pc.fg_process == NULL)
        return NULL;
    if (!(pc.fg_process->state == UNUSED || pc.fg_process->state == KILLED))
        return pc.fg_process;
    return NULL;
}

struct Process *get_process(int pid)
{
    struct Process* process = NULL;

    for (int i = 1; i < PROC_TABLE_SIZE; i++)
    {
        if (process_table[i].state != UNUSED && process_table[i].pid == pid){
            process = &process_table[i];
            break;
        }
    }
    return process;
}

int get_status(int pid)
{
    struct Process* process = get_process(pid);
    return process != NULL ? process->status : INT32_MAX;
}

int get_proc_data(int pid, int *ppid, int *state, char *name, char* args_buf)
{
    int args_size = 0;

    for (int i = 1; i < PROC_TABLE_SIZE; i++)
    {
        if (process_table[i].state != UNUSED && process_table[i].pid == pid){
            if (ppid != NULL)
                *ppid = process_table[i].ppid;
            if (state != NULL)
                *state = process_table[i].state;
            if (name != NULL)
                memcpy(name, process_table[i].name, strlen(process_table[i].name));
            /* Retrieve the program arguments from the bottom of allocated process kernel stack */
            char* arg = (char*)process_table[i].stack;
            /* Omit the first argument since we've already captured program name */
            arg += (strlen(arg)+1);
            int arg_len;
            while (*(arg+args_size) != 0)
            {
                arg_len = strlen(arg+args_size);
                if (args_buf != NULL){
                    memcpy(args_buf+args_size, arg+args_size, arg_len);
                    *(args_buf+args_size+arg_len) = 0;
                }
                args_size += (arg_len+1);
            }
            break;
        }
    }

    return args_size;
}

int get_active_pids(int* pid_list)
{
    int count = 0;
    /* Omit the idle process which occupies the first slot in the process table
       The idle process should be always runnning in kernel context until the system is shutdown */
    for(int i = 1; i < PROC_TABLE_SIZE; i++)
    {
        if (process_table[i].state != UNUSED){
            if (pid_list != NULL)
                pid_list[count] = process_table[i].pid;
            count++;
        }
    }

    return count;
}

void switch_parent(int curr_ppid, int new_ppid)
{
    for(int i = 1; i < PROC_TABLE_SIZE; i++)
    {
        /* Reassign parent for all children which have current parent with curr_ppid */
        if (process_table[i].state != UNUSED && process_table[i].ppid == curr_ppid)
            process_table[i].ppid = new_ppid;
    }
}

void sleep(int event)
{
    struct Process* process;

    process = pc.curr_process;
    process->state = SLEEP;
    /* Save the reason of wait which can used in wake_up to selectively wake up processes based on occurred events */
    process->event = event;

    /* Enqueue the process on the wait list so that it cannnot be rescheduled until woken up and placed on ready queue */
    push_back(&pc.wait_list, (struct Node*)process);
    /* Call the scheduler to replace the current process (which just slept) with other process on the ready queue */
    schedule();
    /* If the process was awakened by the kernel to service an important request, put it back to sleep
       An organically awakened process on occurence of specific event will have the event field reset to NONE */
    if (process->event != NONE)
        sleep(process->event);
}

void wake_up(int event)
{
    struct Process* process, *prev_node = NULL;

    /* If an event occurs while a process is on the ready queue servicing a request, clear the event field */
    process = (struct Process*)find_evt(pc.ready_que.head, event);
    while (process != NULL)
    {
        process->event = NONE;
        process = (struct Process*)find_evt((struct Node*)process->next, event);
    }
    
    /* remove first process waiting on event from the wait list */
    process = (struct Process*)remove_evt(&pc.wait_list, (struct Node**)&prev_node, event);
    /* Place it on the ready queue and check if any other processes waiting on the same event
       If they're sleeping, remove from wait list and place them on the ready queue as well */
    while (process != NULL)
    {
        process->event = NONE;
        process->state = READY;
        push_back(&pc.ready_que, (struct Node*)process);
        process = (struct Process*)remove_evt(&pc.wait_list, (struct Node**)&prev_node, event);
    }
}

void exit(struct Process* process, int status, bool sig_handler_req)
{
    if (process == NULL || process->state == UNUSED || process->state == KILLED)
        return;
    process->status |= sig_handler_req ? (status & 0x7f) : ((status & 0xff) << 8);
    /* Set the state to killed and event to PID for the wait function to sweep it later */
    process->state = KILLED;
    process->event = process->pid;
    /* Inform the parent about death of child and pass its exit status */
    struct Process* parent = get_process(process->ppid);
    if (parent != NULL && parent->state != KILLED){
        parent->signals |= (1 << SIGCHLD);
        parent->status = process->status;
        /* If the parent has abandoned this process, make init a foster parent to clean it up as a zombie */
        if (parent->wpid >= 0 && (parent->wpid != process->pid))
            process->ppid = 1;
    }
    else /* Orphan process. Make init a foster parent */
        process->ppid = 1;
    /* Handover potential orphan children if any to the init process */
    switch_parent(process->pid, 1);
    /* Abdicate status as current system foreground process if it was one */
    if (pc.fg_process != NULL){
        if (process->pid == pc.fg_process->pid)
            pc.fg_process = parent && !parent->daemon ? parent : NULL;
    }
    /* Wake up processes that might be paused while this one was running in the foreground */
    if (!process->daemon)
        wake_up(FG_PAUSED);
    push_back(&pc.zombies, (struct Node*)process);

    /* Wake up the process sleeping in wait to clean up this zombie process */
    wake_up(ZOMBIE_CLEANUP);

    /* Put off scheduling if invoked by a signal handler because it will have work to do */
    if (!sig_handler_req)
        schedule();
}

int wait(int pid, int* wstatus, int options)
{
    struct Process* zombie;
    /* Return failure if the given process does not exist or if the PID value is invalid */
    if (pid == 0 || pid < -1)
        return -1;
    pc.curr_process->wpid = pid;
    
    while (1)
    {
        bool has_child = false;
        /* Search for first available zombie child */
        if (pid == -1){
            for(int i = 1; i < PROC_TABLE_SIZE; i++)
            {
                if (process_table[i].state != UNUSED && process_table[i].ppid == pc.curr_process->pid){
                    has_child = true;
                    if (contains(&pc.zombies, (struct Node*)&process_table[i])){
                        pid = process_table[i].pid;
                        break;
                    }
                }
            }
        }
        else{ /* Verify if the PID the current process is waiting for is a valid process */
            struct Process* process = get_process(pid);
            if (process != NULL && process->state != UNUSED)
                has_child = true;
        }
        /* If the current process doesn't have any children, there's no need to wait */
        if (!has_child)
            return -1;
    
        if (!empty(&pc.zombies)){
            zombie = (struct Process*)remove_evt(&pc.zombies, NULL, pid);
            if (zombie != NULL){
                /* There's a chance some process or handler already cleaned up this zombie */
                if (zombie->state != KILLED)
                    break;
                kfree(zombie->stack);
                free_uvm(zombie->page_map);
                /* Decrement ref counts of all files left open by the zombie */
                for(int i = 0; i < MAX_OPEN_FILES; i++)
                {
                    if (zombie->fd_table[i] != NULL){
                        zombie->fd_table[i]->ref_count--;
                        zombie->fd_table[i]->inode->ref_count--;
                        /* Note that we can't release the inode based on only the file entry ref count because other file entries could be pointing to it
                           Hence the in core inode should be released (entry set to NULL) only if the inode ref count is zero */
                        if (zombie->fd_table[i]->inode->ref_count == 0)
                            zombie->fd_table[i]->inode = NULL;
                    }
                }
                /* Mark process table slot free and reset fields which can potentially get carried over to another process using the same slot
                   We needn't clear the entire process table entry since a new process reusing the slot will overwrite other members */
                zombie->state = UNUSED;
                zombie->daemon = false;
                /* Return the wait status to the caller and then clear it */
                if (wstatus != NULL)
                    *wstatus = zombie->status;
                zombie->status = 0;
                /* For this special wait condition, probe for remaining zombies */
                if (pc.curr_process->wpid == -1)
                    wake_up(ZOMBIE_CLEANUP);
                break;
            }
        }
        if (options & WNOHANG)
            return 0;
        sleep(ZOMBIE_CLEANUP);
    }

    return pid;
}

int fork(void)
{
    struct Process* process;

    /* Allocate a new child process */
    process = alloc_new_process();
    if (process == NULL)
        return -1;
    
    /* Copy the process name and set parent process ID */
    memcpy(process->name, pc.curr_process->name, sizeof(process->name));
    process->ppid = pc.curr_process->pid;
    /* Yield current system foreground process status if holding one, which will allow the child to claim it if required */
    if (pc.fg_process != NULL){
        if (pc.curr_process->pid == pc.fg_process->pid)
            pc.fg_process = NULL;
    }
    /* Copy the text, data, stack and other regions of the parent to the child process' memory
       We copy only one page because the current design of the kernel holds all process regions in a single page */
    if (!copy_uvm(process->page_map, pc.curr_process->page_map, PAGE_SIZE))
        return -1;

    /* Replicate the parent file descriptor table for the child since it shares all open files with the parent 
       Increment the global file table entry ref count of open files. The inode ref count will be incremented as usual */
    memcpy(process->fd_table, pc.curr_process->fd_table, MAX_OPEN_FILES * sizeof(struct FileEntry*));
    for(int i = 0; i < MAX_OPEN_FILES; i++)
    {
        if (process->fd_table[i] != NULL){
            process->fd_table[i]->ref_count++;
            process->fd_table[i]->inode->ref_count++;
        }
    }

    /* Copy the context frame so that the child process also resumes at the point after the fork call */
    memcpy(process->reg_context, pc.curr_process->reg_context, sizeof(struct ContextFrame));
    /* Initialize signal handlers for the child process */
    init_handlers(process);
    /* Set the return value for child process to 0 */
    process->reg_context->x0 = 0;
    process->state = READY;
    push_back(&pc.ready_que, (struct Node*)process);

    /* The parent process which called fork will be returned the child process PID */
    return process->pid;
}

int exec(struct Process* process, char* name, const char* args[])
{
    int fd;
    uint32_t size;

    fd = open_file(process, name);
    if (fd == -1)
        return -1;

    /* Get the size and count of passed arguments for the new program */
    int arg_count = 0;
    int arg_size = 0;
    if (args != NULL){
        int new_arg_size;
        while (args[arg_count] != NULL)
        {
            new_arg_size = strlen(args[arg_count]);
            if (new_arg_size == 1 && args[arg_count][0] == '&'){
                process->daemon = true;
                /* Yield the foreground status if inherited from the parent during a forking event */
                if (pc.fg_process != NULL){
                    if (process->pid == pc.fg_process->pid)
                        pc.fg_process = NULL;
                }
                break;
            }
            arg_size += (new_arg_size+1);
            arg_count++;
        }
    }
    arg_size += (strlen(name)+1);

    /* Use the bottom of the allocated stack space for saving programs args instead of current stack pointer address
       This is done to avoid overwriting kernel stack at stack pointer location which contains parent call stack */
    char* arg_val_ks = (char*)process->stack;
    
    /* Copy the program name and args to process kernel stack */
    int arg_len[arg_count+1];
    arg_len[0] = strlen(name);
    memcpy(arg_val_ks, name, arg_len[0]);
    arg_val_ks[arg_len[0]] = 0;
    arg_val_ks += (arg_len[0]+1);
    for(int i = 0; i < arg_count; i++)
    {
        arg_len[i+1] = strlen(args[i]);
        memcpy(arg_val_ks, (char*)args[i], arg_len[i+1]);
        arg_val_ks[arg_len[i+1]] = 0;
        arg_val_ks += (arg_len[i+1]+1);
    }
    /* Set new name in the process table entry. NOTE Parent process ID would remain the same */
    memset(process->name, 0, sizeof(process->name));
    memcpy(process->name, name, strlen(name)-(MAX_EXTNAME_BYTES+1));
    /* In exec call, the regions of the current process are overwritten with the regions of the new process and PID remains the same.
       Hence there's no need to allocate new memory for the new program */
    memset((void*)USERSPACE_BASE, 0, PAGE_SIZE);
    size = get_file_size(process, fd);
    /* We use the userspace virt address as buffer because memory was previously allocated for the process which called exec */
    size = read_file(process, fd, (void*)USERSPACE_BASE, size);
    /* Here if the exec operation fails, only option is to exit because we've cleared the regions of original process */
    if (size == UINT32_MAX)
        exit(process, 1, false);

    close_file(process, fd);
    /* Clear any previously set custom handlers and initialize default signal handlers for the new process */
    memset(process->handlers, 0, sizeof(SIGHANDLER)*TOTAL_SIGNALS);
    init_handlers(process);
    /* Clear the previous process' context frame since we don't return to it */
    memset(process->reg_context, 0, sizeof(struct ContextFrame));
    /* The return address should be set to start of text section of new process i.e. the userspace base address */
    process->reg_context->elr = USERSPACE_BASE;
    /* Set the user program stack pointer to highest page address from where it can grow downwards */
    process->reg_context->sp0 = USERSPACE_BASE + PAGE_SIZE;
    /* Set pstate mode field to 0 (EL0) and DAIF bits to 0 which means no masking of interrupts i.e. interrupts enabled */
    process->reg_context->spsr = 0;
    /* Save arg count in x2 since x0 will be overwritten by the syscall return value when this function returns
       Before calling main, userspace programs can move this value to x0 to be identified as first arg to main */
    process->reg_context->x2 = arg_count+1;
    /* Make room for program arg pointers and content on the userspace stack */
    process->reg_context->sp0 -= (arg_count+1)*8;
    int64_t* arg_ptr = (int64_t*)process->reg_context->sp0;
    process->reg_context->sp0 -= UPPER_BOUND(arg_size, 8);

    /* Copy program arguments from the kernel stack to the user stack for the process to access */
    char* arg_val = (char*)process->reg_context->sp0;
    arg_val_ks = (char*)process->stack;

    memcpy(arg_val, arg_val_ks, arg_len[0]);
    *arg_ptr = (int64_t)arg_val;
    arg_ptr++;
    arg_val += (arg_len[0]+1);
    arg_val_ks += (arg_len[0]+1);
    for(int i = 1; i <= arg_count; i++)
    {
        memcpy(arg_val, arg_val_ks, arg_len[i]);
        *arg_ptr = (int64_t)arg_val;
        arg_ptr++;
        arg_val += (arg_len[i]+1);
        arg_val_ks += (arg_len[i]+1);
    }

    /* Save the argument addresses location on the stack to x1 to be used as second argument to main */
    process->reg_context->x1 = (int64_t)arg_ptr - (arg_count+1)*8;

    return 0;
}

int kill(int pid, int signal)
{
    if (signal < 0 || signal > TOTAL_SIGNALS-1)
        return -1;
    if (pid == -1){ /* Send signal to all processes except init process (PID 1) */
        int curr_pid = get_curr_process()->pid;
        for(int i = 2; i < PROC_TABLE_SIZE; i++)
        {
            /* The signal is not meant for the process which sent it */
            if (process_table[i].pid == curr_pid)
                continue;
            if (!(process_table[i].state == UNUSED || process_table[i].state == KILLED)){
                process_table[i].signals |= (1 << signal);
                /* Wake up sleeping processes to act on the broadcast signal */
                if (process_table[i].state == SLEEP){
                    remove(&pc.wait_list, (struct Node*)&process_table[i]);
                    process_table[i].state = READY;
                    push_back(&pc.ready_que, (struct Node*)&process_table[i]);
                }
            }
            else if (process_table[i].state == KILLED && signal == SIGHUP){
                if (process_table[i].ppid != 1){ /* Release rogue or unattended zombie not owned by init */
                    kfree(process_table[i].stack);
                    free_uvm(process_table[i].page_map);
                    /* Decrement ref counts of all files left open by the zombie */
                    for(int i = 0; i < MAX_OPEN_FILES; i++)
                    {
                        if (process_table[i].fd_table[i] != NULL){
                            process_table[i].fd_table[i]->ref_count--;
                            process_table[i].fd_table[i]->inode->ref_count--;
                            /* Note that we can't release the inode based on only the file entry ref count because other file entries could be pointing to it
                            Hence the in core inode should be released (entry set to NULL) only if the inode ref count is zero */
                            if (process_table[i].fd_table[i]->inode->ref_count == 0)
                                process_table[i].fd_table[i]->inode = NULL;
                        }
                    }
                    process_table[i].state = UNUSED;
                    process_table[i].daemon = false;
                }
            }
        }
        /* Prepare to terminate the init and idle process, since a system wide SIGTERM implies a shutdown request */
        if (signal == SIGTERM){
            (process_table+1)->signals |= (1 << signal);
            process_table->signals |= (1 << signal);
        }
        /* Reset the PID counter on a system wide hang up signal which suggests user log out */
        if (signal == SIGHUP)
            pid_num = 2;
        return 0;
    }
    if (pid == 0){ /* Send signal to all children */
        struct Process* process = get_curr_process();
        for(int i = 2; i < PROC_TABLE_SIZE; i++)
        {
            /* The signal is not meant for the process which sent it */
            if (process_table[i].pid == process->pid)
                continue;
            if (!(process_table[i].state == UNUSED || process_table[i].state == KILLED) && 
                process->pid == process_table[i].ppid){
                process_table[i].signals |= (1 << signal);
                /* Wake up sleeping processes to act on the group signal */
                if (process_table[i].state == SLEEP){
                    remove(&pc.wait_list, (struct Node*)&process_table[i]);
                    process_table[i].state = READY;
                    push_back(&pc.ready_que, (struct Node*)&process_table[i]);
                }
            }
        }
        return 0;
    }
    struct Process* target_proc = get_process(pid);
    if (!target_proc)
        return -1;
    
    target_proc->signals |= (1 << signal);
    /* Wake up the process if sleeping and place it on the ready queue, for it to act on the received signal */
    if (target_proc->state == SLEEP){
        remove(&pc.wait_list, (struct Node*)target_proc);
        target_proc->state = READY;
        push_back(&pc.ready_que, (struct Node*)target_proc);
    }

    return 0;
}
