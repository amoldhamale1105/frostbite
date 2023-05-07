# Macros dealing with context and mode switch operations
.macro kernel_entry
    # Allocate space for 36 registers on the stack for interrupted context. We have total of 31 GPRs, 1 stack pointer register and 4 system registers
    sub sp, sp, #(36*8)
    # Use store pair instruction to save all 32 GPRs on the stack, 2 at a time
    stp x0, x1, [sp]
    # Move 16 bytes up in the stack to store the next pair (pair of 64 bit registers = 16 bytes) 
    stp x2, x3, [sp, #(16*1)]
    stp x4, x5, [sp, #(16*2)]
    stp x6, x7, [sp, #(16*3)]
    stp x8, x9, [sp, #(16*4)]
    stp x10, x11, [sp, #(16*5)]
    stp x12, x13, [sp, #(16*6)]
    stp x14, x15, [sp, #(16*7)]
    stp x16, x17, [sp, #(16*8)]
    stp x18, x19, [sp, #(16*9)]
    stp x20, x21, [sp, #(16*10)]
    stp x22, x23, [sp, #(16*11)]
    stp x24, x25, [sp, #(16*12)]
    stp x26, x27, [sp, #(16*13)]
    stp x28, x29, [sp, #(16*14)]
    # Load EL0 stack pointer in x0 to be saved to the register context. Each exception level has its own dedicated stack pointer
    mrs x0, sp_el0
    stp x30, x0, [sp, #(16*15)]
.endm

.macro kernel_exit
    # Load last values in x30 and x0 first so that x0 later doesn't get overwritten with the stack pointer value
    ldp x30, x0, [sp, #(16*15)]
    # Load x0 value to the stack pointer register
    msr sp_el0, x0
    ldp x0, x1, [sp]
    ldp x2, x3, [sp, #(16*1)]
    ldp x4, x5, [sp, #(16*2)]
    ldp x6, x7, [sp, #(16*3)]
    ldp x8, x9, [sp, #(16*4)]
    ldp x2, x3, [sp, #(16*1)]
    ldp x10, x11, [sp, #(16*5)]
    ldp x12, x13, [sp, #(16*6)]
    ldp x14, x15, [sp, #(16*7)]
    ldp x16, x17, [sp, #(16*8)]
    ldp x18, x19, [sp, #(16*9)]
    ldp x20, x21, [sp, #(16*10)]
    ldp x22, x23, [sp, #(16*11)]
    ldp x24, x25, [sp, #(16*12)]
    ldp x26, x27, [sp, #(16*13)]
    ldp x28, x29, [sp, #(16*14)]

    add sp, sp, #(36*8)
    eret
.endm

.macro handler_entry
    # Use exception syndrome register value as second arg which holds information about the current exception
    mrs x1, esr_el1
    # Save the trap number and error code on the stack as part of the context frame
    stp x0, x1, [sp, #(16*16)]
    # Load the link register value to x0 which holds the return address
    mrs x0, elr_el1
    mrs x1, spsr_el1
    # Save the return address and pstate value to the stack as part of the context frame
    stp x0, x1, [sp, #(16*17)]
    # Pass the stack pointer as an argument so that the context frame can be accessed by the handler
    mov x0, sp
    bl handler
.endm

.section .text
.global vector_table
.global enable_timer
.global read_timer_freq
.global read_timer_status
.global set_timer_interval
.global enable_irq
.global pstart

# Align the vector table to a 2KB boundary (0x800 = 2048)
# Aligh each handler within it to 128 byte boundary (0x80 = 128)
.balign 0x800
vector_table:
# Current el with sp0 handlers for EL1
current_el_sp0_sync:
    # Branch to error because these handlers should never be called ideally
    b error

.balign 0x80
current_el_sp0_irq:
    b error

.balign 0x80
current_el_sp0_fiq:
    b error

.balign 0x80
current_el_sp0_serror:
    b error

# Current el with spn handlers for EL1. These are the ones we'll use for EL1 exceptions
.balign 0x80
current_el_spn_sync:
    b sync_handler

.balign 0x80
current_el_spn_irq:
    b irq_handler

.balign 0x80
current_el_spn_fiq:
    b error

.balign 0x80
current_el_spn_serror:
    b error

# Lower el with aarch64 handlers for EL0. These are the ones we'll use for EL0 exceptions
.balign 0x80
lower_el_aarch64_sync:
    b sync_handler

.balign 0x80
lower_el_aarch64_irq:
    b irq_handler

.balign 0x80
lower_el_aarch64_fiq:
    b error

.balign 0x80
lower_el_aarch64_serror:
    b error

# Lower el with aarch32 handlers for EL0
.balign 0x80
lower_el_aarch32_sync:
    b error

.balign 0x80
lower_el_aarch32_irq:
    b error

.balign 0x80
lower_el_aarch32_fiq:
    b error

.balign 0x80
lower_el_aarch32_serror:
    b error

pstart:
    mov sp, x0
trap_return:
    # Restore GPRs and other registers of previous context with the load pair instruction
    # NOTE We don't need to restore trap number and error code from the stack anywhere
    # However, the return address and pstate values need to restored in respective registers
    ldp x0, x1, [sp, #(16*17)]
    msr elr_el1, x0
    msr spsr_el1, x1

    kernel_exit

sync_handler:
    kernel_entry
    # Exception ID 1 means synchronous exception
    mov x0, #1
    handler_entry
    b trap_return

irq_handler:
    kernel_entry
    # Exception ID 2 means hardware (asynchronous exception) interrupt
    mov x0, #2
    handler_entry
    b trap_return

error:
    kernel_entry
    # 0 is unknown error. Pass it as an arg to the global exception handler, handler
    mov x0, #0
    handler_entry
    b trap_return

read_timer_freq:
    # Read the frequency of the system count from the frequency register
    mrs x0, CNTFRQ_EL0
    ret

set_timer_interval:
    # Load TVAL (timer value register) with value in x0
    msr CNTP_TVAL_EL0, x0
    ret

enable_timer:
    # Save the frame pointer (x29) and return address (x30) on the stack because we have nested function calls
    # Push x29 and x30 on the stack using the store pair instruction
    stp x29, x30, [sp, #-16]!

    # Get the system count freq which will be stored in x0 as return value of the read_timer_freq call
    bl read_timer_freq
    # We need timer interrupt trigger every 10 ms, hence dividing frequency by 100
    mov x1, #100
    udiv x0, x0, x1

    # This will set TVAL = interval, CVAL (comparator value register) = TVAL + system count, and start decrementing the TVAL register
    # When CVAL <= system count, the timer interrupt is raised
    bl set_timer_interval

    # Control register's bits 0 (timer enable), 1 (interrupt mask), 2 (status)
    # Bit 0 should be set to high, and bit 1 to zero
    # When an interrupt is raised and the status bit is high, it means that timer interrupt has occured
    mov x0, #1
    msr CNTP_CTL_EL0, x0

    # Pop x29 and x30 from stack using load pair instruction
    ldp x29, x30, [sp], #16
    ret

read_timer_status:
    # Read the status bit of the timer control register to check if it's the timer that the interrupt has been asserted for
    mrs x0, CNTP_CTL_EL0
    ret

enable_irq:
    # In the pstate register, of the DAIF bits, the interrupt bit needs to cleared otherwise they will be masked
    # Write the corresponding bit (bit 2) in the daif clear register
    msr daifclr, #2
    ret
    