# HW1 answers

Fill in this file and hand it in with your code. Replace each
`(your answer)` with two or three sentences. Keep the question numbers
(part.question), and do not delete the questions.

- **Name:**
- **Machine and environment you tested on** (CADE lab machine with QEMU, or Bochs):
- **How far did you get?** (for example "all six parts work", or "stuck in part 4, block B"):


## Part 1: Entering VMX operation

### Question 1.1

> In Step 1, why does the function `return` when VMX is missing instead
> of calling `panic()`?

**Answer:** So the operating system can still boot/function even without VMX. VMX isn't needed for normal OS functionality - runvm will exit if the user tries to run a vm later.

### Question 1.2

> In Step 2, on most machines the firmware has already enabled and
> locked VMX. Why does the code still handle the unlocked case instead
> of just failing?

**Answer:** So machines with firmware that doesn't lock MSR_IA32_FEATURE_CONTROL can still work. This just means the firmware hasn't configured the flag, not that the machine isn't actually capable of using VMX. 

### Question 1.3

> In Step 3, what would happen if we skipped `lcr4(cr4)` and went
> straight to `VMXON`?

**Answer:** We get a kernel panic due to undefined opcode exception. The processor relies on CR4 being set correctly to execute VMX instructions.

### Question 1.4

> In Step 4, why does each CPU need its own VMXON region? What could go
> wrong if all CPUs shared one?

**Answer:** Because each CPU needs to track its own state for VMX operation. If each CPU shared one, VMs would race on the same data (if 2 CPUs run a VM at the same time) and would almost certainly crash.

## Part 2: Creating a VM, its memory, and the EPT

### Question 2.1

> An ordinary x86-64 page table translates virtual addresses to physical
> addresses. What does the EPT translate, and who controls it? What
> would go wrong if the hypervisor gave the guest direct access to the
> host's physical memory instead?

**Answer:** The EPT translates guest virtual addresses to guest physical addresses. The VMM owns and maintains the EPT. If the hypervisor gave the guest direct access to physical memory, many bad things could happen. The guest would be allowed to destroy the host (corrupt/break) memory, or read data it's not supposed to (i.e from other guests or the host itself). This is terrible for security and reliability.

### Question 2.2

> In Step B7 we zero every page of guest memory, and in `eptwalk()` we
> zero every new table. These are two different reasons to zero. State
> each, and say what the guest or the CPU would see if we skipped it.

**Answer:** The previous memory from kalloc could contain anything - kernel data or user memory. This is a security risk. It's also UB because the guest's functionality would not be deterministic and would depend on the contents of that memory (likely nonzero).

### Question 2.3

> In Step B8, why does `vmcreate()` copy the image page by page through
> `eptwalk()`, instead of one `memmove` of `len` bytes to the guest's
> first page?

**Answer:** kalloc pages are NOT necessarily contiguous in physical memory, where they should be for the guest. As a result, pages must be copied one by one.

### Question 2.4

> An EPT entry is "present" if any of R, W or X is set, not if one
> particular bit is set. A common bug is to test only bit 0. Name one
> kind of entry that this bug would treat as missing but that the CPU
> treats as valid (hint: see Appendix A.10 for execute-only support), and
> say why our code never creates such an entry.

**Answer:** Pages that are marked execute only would be considered invalid but this is allowed (if IA32_VMX_EPT_VPID_CAP bit 0 is 1). Our code never creates such an entry because every entry is created with EPT_RWX

// vmx.c:544
*e = V2P(next) | EPT_RWX;

// vmx.h:308
#define EPT_R                         (1ULL << 0)
#define EPT_W                         (1ULL << 1)
#define EPT_X                         (1ULL << 2)
#define EPT_RWX                       (EPT_R | EPT_W | EPT_X)

## Part 3: The VMCS, describing the guest and the host

### Question 3.1

> In `adjustctl()`, why is it enough to compute
> `(want | low) & high`? What does the function do in the case where
> `want` has a bit that `high` has cleared, and why is returning an error
> better than silently dropping the bit?

**Answer:** This computation is enough because any cpu-required 0 bits will be 0 in v. Then, we compare v & want to want and if there is any difference, we know that the CPU does not support a feature. Then we can return an error rather than letting execution continue for a trap/vm exit to happen later.

### Question 3.2

> The exception bitmap is all ones. If it were 0, what would happen to a
> page fault in the guest, step by step, given that the guest's IDTR limit
> is 0? Why is the all-ones setting more useful to someone debugging a
> guest?

**Answer:** After the page fault occurs, the guest will deliver the fault to its own IDT. This will fail becaues the IDTR limit is 0, so a GP exception will be raised. This will trigger a DF exception, which will also fail. A triple fault would reset the hardware on bare metal, but in this case it will trigger a VM exit with exit reason 2. With all exceptions going straight to the VMM, we will learn about the page fault immediately with all information in the VMCS.

### Question 3.3

> Why must "enable EPT" be 1 whenever "unrestricted guest" is 1? Think
> about what the CPU does with a guest linear address when `CR0.PG = 0`
> (SDM 28.6).

**Answer:** Because the guest expects to deal with raw physical memory in unrestricted guest mode. As such, enable EPT must be 1 to maintain a page table for the guest as well. If EPT were not required, then the guest would just read host physical memory which we already know from previous questions is bad (security, reliability, etc).

### Question 3.4

> The real `CR4` in the VMCS has `VMXE` = 1 but the guest reads it as 0.
> Which two VMCS fields make that work? What happens if the guest tries to
> set `CR4.VMXE` to 1, and why do we not let it?

**Answer:** CR4 guest/host mask and CR4 read shadow. Attempting to set cr4.vmxe would cause a vm exit because the bit is 'owned' by the host (set to 1). We don't let it because we do not want to allow nested virtualization.

## Part 4: Entering and leaving the guest

### Question 4.1

> `struct guestregs` has no `rsp` field. Why can the guest's stack
> pointer not be handled the way the other registers are? Where does the
> CPU keep it instead, and what would go wrong if `vmx_exit` tried to
> store `%rsp` into `*regs` like the others?

**Answer:** Both the guest and host stack pointer are stored/loaded in the VMCS by the CPU upon VMLAUNCH / VM exit. Trying to store it like the other registers in vmx_exit would just store the host %rsp into the guest register because the CPU already loaded the host's %rsp upon VM exit.

### Question 4.2

> `HOST_RIP` is written once, in `vmcsinit()`, but `HOST_RSP` is written
> in `vmx_enter` on every entry. What makes one constant and the other
> not? Describe a concrete situation in which the `HOST_RSP` written by
> a previous entry would be wrong.

**Answer:** It makes sense for the exit handler for the VMM to be a fixed place - we will always be parsing the VMCS for the exit reason/data etc, however the host %rsp is inherently variable. Imagine we stored the host %rsp once in the VMCS when we set it up like HOST_RIP, and the CPU loads that %rsp after a vm exit. %rsp is almost certainly different - between setting up the vmcs and actually running vmlaunch, we very likely added some data to the stack. Thus, to know the correct %rsp upon vm exit, we need to set it just before vmlaunch. If we added VMRESUME support later, the kernel stack depth would be different. With this stale %rsp, garbage registers would be loaded in vmx_exit.

### Question 4.3

> `vmrun()` always calls `vmx_enter(&vm->regs, 0)`, and so always uses
> `VMLAUNCH`. Which instruction clears the launch state, and where does
> `vmrun()` execute it? What would the VM-instruction error be if the
> code passed `launched = 1` on the first entry?

**Answer:** VMCLEAR. It's executed in vmx.c:1000 (after the vm exits and stores exit data in the vm structure). VM-instruction error 5

### Question 4.4

> `vmxhoststate()` is called before every entry, not once. Name two
> fields it writes that can differ between two calls to `vmrun()`, and
> say what is different (CPU, process, or both) for each. What would
> happen at the next VM exit if the field kept the old value?

HOST_CR3. Process. Each process has its own page table - if the first process were freed after it exited, the stale cr3 would be used.
HOST_TR_BASE. CPU. A stale TSS would be used if the VM were moved to another CPU.

## Part 5: Running the guest, the vmrun loop and exit handling

### Question 5.1

> In Step 8, `vmclear()` must come before `popcli()`. Describe a
> concrete sequence of events, with two CPUs, in which the opposite
> order corrupts the VMCS. Which sentence of SDM 27.11.1 do you rely on?

**Answer:** 
Imagine cpu0 just ran vmexit and is about to run vmclear in vmrun. If popcli is executed on cpu0 before vmclear, cpu0 may get sidetracked into a bunch of other work from some interrupt. During this time, cpu1 could get scheduled this process and would write the wrong VMCS data back (the correct data is stored in cpu0's internal caches). The process (still belonging to cpu1) could then vmentry again, write a bunch of data back to the VMCS on vmclear and then be switched to cpu0 after this cycle of vmrun (right after popcli). cpu0 would then write its old VMCS back to data and completely mess the guest state up
From SDM: "A VMCS that is made active on more than one logical processor may become corrupted (see below)"

### Question 5.2

> We pass `launched = 0` to `vmx_enter` on every entry, so we always use
> `VMLAUNCH`. What would the program have to do differently if we kept
> the VMCS loaded between exits and used `VMRESUME` instead? What would
> we give up?

**Answer:** We would need to maintain a launched flag for each vm rather than passing 0 to vmx_enter every time. We would also need to make sure we run VMCLEAR if the cpu is switched during VM execution (to properly flush the VMCS). What we would lose is the simplicity to just schedule the VM on whatever cpu we want without tracking much.

### Question 5.3

> On an external-interrupt exit, `vmxhandle()` does nothing but return 1,
> yet the interrupt is handled by the host before the guest runs again.
> Where and when exactly does the host handle it? What would change if
> the exit control "acknowledge interrupt on exit" were set?

**Answer:** The host handles the interrupt after popcli is executed (after vm exit in vmrun()). If we set "acknowledge interrupt on exit" without changing anything, the interrupt would be lost because the CPU does not execute this in hardware. The VMM would need to handle it in software.

### Question 5.4

> The `WRMSR` exit and the `OUT` exit are both followed by
> `vm->rip += inslen`. What would happen if the code used a fixed
> length (for instance 2) instead? Name one instruction in `helloguestasm.S`
> or `helloguest.c` that shows the problem.

**Answer:** OUT instructions can be 1 byte long, not just 2. The 1 byte version reads the port from %dx, which helloguest.c actually generates (it has to because the port is > 255 and the 2 byte version only allows a imm8) If we fixed this to 2 bytes, %rip would be off by 1 and would completely break the instruction stream.
  10006a:	ba f8 03 00 00       	mov    $0x3f8,%edx
  100070:	ee                   	out    %al,(%dx)

## Part 6: The other half: the monitor (runvm) and the guest

### Question 6.1

> In `doio()`, why does an `IN` need the monitor to write the guest's
> RAX with `vmsetreg()`, instead of `doio()` just returning a value?
> And why does `completein()` read RAX with `vmgetreg()` for a 1-byte
> `IN` but not for a 4-byte one?

**Answer:** We need to emulate the IN instruction, so doio returning a value would only be useful for the VMM. Thus, we need to write to the guest's register state.

For 1-byte writes, we need the old %rax to properly preserve the upper bits when we write the lower bits. If we simply wrote to rax directly with a 0 extended write to %eax, %rax upper bits would be destroyed. The 4-byte write will do this anyway so we do not need to read the old %rax (there is nothing to preserve)

### Question 6.2

> `helloguest.ld` links the guest at `0x100000`. Suppose you change it
> to `0x200000` but leave `VM_LOAD_ADDR` alone. Name two things in the
> guest that will go wrong and say why. (Look at the page tables, the
> `gdtdesc`, and where the first instruction is.)

**Answer:** Any absolute references will point to garbage data instead of the intended value.
PML4 will be completely wrong: movl    $pml4, %eax
call    guestmain will also fail

### Question 6.3

> The guest in Step 8 sets `CR4.PAE` before `CR0.PG`. What happens if
> you swap the order of these steps? Then, for the Checkpoint's
> `runvm helloguest 1` experiment: which message do you get, and which
> check in the kernel produces it?

**Answer:** We get a pageful - runvm: guest exception 14, error code 0x0, rip 0x10000B

The check below catches the error:
  if (memsz <= 0 || memsz > VM_MAXMEM || len < 0 || VM_LOAD_ADDR + len > memsz)
   	return -2;

### Question 6.4

> Each `OUT` in the guest causes a VM exit, a trip through the kernel,
> and a return to `runvm`, which calls `write()`. Why did the design
> put the serial port in `runvm` and not in the kernel? What would you
> gain, and what would you lose, by emulating it inside the kernel?

**Answer:** You would save on some context switches by emulating the OUT directly in the kernel. However, this would make it so you have to update the kernel every time you want to want to add a new port to emulate. The main benefit of keeping it in userspace is isolation - a bug there won't lead to kernel compromise.