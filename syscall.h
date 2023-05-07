#ifndef SYSCALL_H
#define SYSCALL_H

#include "handler.h"

typedef int (*SYSTEMCALL)(int64_t *arg);
void init_system_call(void);
void system_call(struct ContextFrame* ctx);

#define TOTAL_SYSCALL_FUNCTIONS 10

#endif