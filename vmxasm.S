# VM entry and exit.
#
#   int vmx_enter(struct guestregs *regs, int launched);
#
# Saves the host's callee-saved registers, records the host stack
# pointer in the VMCS, loads the guest's general-purpose registers
# from regs, and enters the guest with VMLAUNCH (launched == 0) or
# VMRESUME. The current VMCS's HOST_RIP must point at vmx_exit.
#
# Returns 0 after a VM exit, with the guest's registers saved in regs.
# Returns -1 if VMLAUNCH/VMRESUME itself failed; the caller reads
# VM_INSTRUCTION_ERROR for the reason.
#
# Why this must be assembly: VM entry and exit do not follow the C
# calling convention. The CPU saves and restores only a few pieces of
# state through the VMCS (RIP, RSP, RFLAGS, control registers,
# segments, some MSRs). It does NOT save or load the general-purpose
# registers, so software must do that. Doing it from C would let the
# compiler use those registers for its own purposes in between.
#
# The big picture, in one place:
#
#   vmx_enter                              (host, called from vmrun)
#     save host callee-saved regs on the host stack
#     VMCS.HOST_RSP = current %rsp          <- where the exit will land
#     load guest regs from *regs
#     VMLAUNCH/VMRESUME  ------------------> guest runs ...
#                                            ... VM exit
#   vmx_exit  <-------- CPU loads HOST_RIP, HOST_RSP from the VMCS
#     save guest regs into *regs
#     restore host callee-saved regs, return 0 to vmrun's caller
#
# vmx_exit is not a function that anyone calls. It is a landing pad:
# the CPU jumps to it, with the stack that vmx_enter prepared, and
# vmx_exit finishes by executing the "ret" that belongs to vmx_enter.
# So from C, a single call to vmx_enter returns only after the guest
# has exited.
#
# The offsets used below (0, 8, 16, ...) are the byte offsets of the
# fields of struct guestregs in vmx.h. They are listed here so they
# can be checked against that structure (8 bytes per field):
#
#   0 rax    8 rbx   16 rcx   24 rdx   32 rsi   40 rdi   48 rbp
#  56 r8    64 r9    72 r10   80 r11   88 r12   96 r13  104 r14
# 112 r15
#
# Note that rdi sits between rsi and rbp, not at the end, and that
# there is no rsp field: the guest's RSP, RIP and RFLAGS live in the
# VMCS (guest-state area), not in struct guestregs.

# Encoding of the VMCS field HOST_RSP (same value as in vmx.h). The
# VMWRITE instruction takes the field encoding as a register operand.
#define HOST_RSP 0x6C14

.globl vmx_enter
vmx_enter:
  # On entry (System V AMD64 calling convention): %rdi = regs (first
  # argument), %esi = launched (second argument), return address at
  # (%rsp). The callee-saved registers are %rbx, %rbp, %r12-%r15.
  #
  # YOUR CODE, block A (HW1 part 4): save the host's callee-saved
  # registers and regs on the stack, and write %rsp into the VMCS field
  # HOST_RSP.
  #   example, one line of it:  pushq %rbp   (saves the host's %rbp)
  pushq %rbp
  pushq %rbx
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  pushq %rdi

  movq $HOST_RSP, %rax

  vmwrite %rsp, %rax

  # Test 'launched' now: the movs below don't change the flags.
  # %esi is about to be overwritten by the guest's RSI, so its zero /
  # non-zero state is captured in ZF here. ZF = 1 means launched == 0
  # (first entry, use VMLAUNCH); ZF = 0 means VMRESUME. No instruction
  # between here and the jnz modifies RFLAGS (mov does not).
  testl %esi, %esi

  # YOUR CODE, block B (HW1 part 4): load the guest's 15 general-purpose
  # registers from *regs (%rdi). Use only mov instructions here: the
  # flags set by the testl above must survive until the jnz below.
  #   example, one line of it:  movq 8(%rdi), %rbx
  #   (%rbx = the 8 bytes at address regs + 8: the guest's rbx)
  movq (%rdi), %rax
  movq 8(%rdi), %rbx
  movq 16(%rdi), %rcx
  movq 24(%rdi), %rdx
  movq 32(%rdi), %rsi
  movq 48(%rdi), %rbp
  movq 56(%rdi), %r8
  movq 64(%rdi), %r9
  movq 72(%rdi), %r10
  movq 80(%rdi), %r11
  movq 88(%rdi), %r12
  movq 96(%rdi), %r13
  movq 104(%rdi), %r14
  movq 112(%rdi), %r15
  movq 40(%rdi), %rdi # do %rdi last

  # Enter the guest. ZF still holds the result of testl above.
  jnz   1f                    # launched != 0: already launched, resume
  # VMLAUNCH is required for the first entry after VMCLEAR (the VMCS
  # launch state is "clear"); VMRESUME is required afterwards. The
  # wrong one fails with a VM-instruction error. Because vmrun
  # VMCLEARs the VMCS after every exit, it always passes launched = 0.
  # On success neither instruction falls through: the guest starts
  # running and control returns only via a VM exit to vmx_exit.
  vmlaunch
  jmp   2f                    # reached only if VMLAUNCH failed
1:
  vmresume                    # reached only if launched != 0
2:
  # VMLAUNCH/VMRESUME fell through: the entry failed (CF or ZF set).
  # CF = 1 is "VMfailInvalid" (no current VMCS), ZF = 1 is
  # "VMfailValid" (error number in VM_INSTRUCTION_ERROR). We do not
  # look at the flags: reaching this line at all means failure, and
  # the C caller reads VM_INSTRUCTION_ERROR. The guest has not run, so
  # *regs was not modified. Unwind exactly what we pushed:
  addq  $8, %rsp              # drop the saved regs pointer
  popq  %r15                  # restore host callee-saved registers,
  popq  %r14                  # in reverse order of the pushes
  popq  %r13
  popq  %r12
  popq  %rbx
  popq  %rbp
  movl  $-1, %eax             # return value: -1 means entry failed
  ret                         # back to the C caller, on its stack

# The processor lands here on VM exit, with %rsp equal to HOST_RSP
# and the guest's general-purpose registers still live.
# On a VM exit the CPU has already: saved guest RSP, RIP, RFLAGS and
# the exit information into the VMCS; loaded host CR0/CR3/CR4, the
# segment registers, GDTR/IDTR, and EFER from the VMCS host-state
# area; set RFLAGS to 0x2 (interrupts disabled, so nothing can
# interrupt us here); and loaded RSP and RIP from HOST_RSP/HOST_RIP.
# Nothing else has changed: %rax..%r15 still hold the guest's values.
.globl vmx_exit
vmx_exit:
  # YOUR CODE (HW1 part 4): save the guest's 15 general-purpose
  # registers into *regs, restore the host's callee-saved registers,
  # return 0 from vmx_enter.
  #   example, one line of it:  movq %rbx, 8(%rdi)
  #   (the 8 bytes at address regs + 8 = the guest's rbx, if %rdi = regs)
  pushq %rdi
  movq 8(%rsp), %rdi # rdi holds regs now

  movq  %rax, 0(%rdi)  # store guest regs INTO *regs
  movq  %rbx, 8(%rdi)
  movq  %rcx, 16(%rdi)
  movq  %rdx, 24(%rdi)
  movq  %rsi, 32(%rdi)
  movq  %rbp, 48(%rdi)
  movq  %r8,  56(%rdi)
  movq  %r9,  64(%rdi)
  movq  %r10, 72(%rdi)
  movq  %r11, 80(%rdi)
  movq  %r12, 88(%rdi)
  movq  %r13, 96(%rdi)
  movq  %r14, 104(%rdi)
  movq  %r15, 112(%rdi)

  popq  %rax
  movq  %rax, 40(%rdi) # save rdi now 

  addq   $8, %rsp # pop regs
  popq  %r15
  popq  %r14
  popq  %r13
  popq  %r12
  popq  %rbx
  popq  %rbp
  xorl %eax, %eax
  ret      