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

**Answer:** So the operating system can still boot/function even without VMX. VMX isn't needed for normal OS functionality.

### Question 1.2

> In Step 2, on most machines the firmware has already enabled and
> locked VMX. Why does the code still handle the unlocked case instead
> of just failing?

**Answer:** (your answer)

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

**Answer:** (your answer)

### Question 2.2

> In Step B7 we zero every page of guest memory, and in `eptwalk()` we
> zero every new table. These are two different reasons to zero. State
> each, and say what the guest or the CPU would see if we skipped it.

**Answer:** (your answer)

### Question 2.3

> In Step B8, why does `vmcreate()` copy the image page by page through
> `eptwalk()`, instead of one `memmove` of `len` bytes to the guest's
> first page?

**Answer:** (your answer)

### Question 2.4

> An EPT entry is "present" if any of R, W or X is set, not if one
> particular bit is set. A common bug is to test only bit 0. Name one
> kind of entry that this bug would treat as missing but that the CPU
> treats as valid (hint: see Appendix A.10 for execute-only support), and
> say why our code never creates such an entry.

**Answer:** (your answer)

## Part 3: The VMCS, describing the guest and the host

### Question 3.1

> In `adjustctl()`, why is it enough to compute
> `(want | low) & high`? What does the function do in the case where
> `want` has a bit that `high` has cleared, and why is returning an error
> better than silently dropping the bit?

**Answer:** (your answer)

### Question 3.2

> The exception bitmap is all ones. If it were 0, what would happen to a
> page fault in the guest, step by step, given that the guest's IDTR limit
> is 0? Why is the all-ones setting more useful to someone debugging a
> guest?

**Answer:** (your answer)

### Question 3.3

> Why must "enable EPT" be 1 whenever "unrestricted guest" is 1? Think
> about what the CPU does with a guest linear address when `CR0.PG = 0`
> (SDM 28.6).

**Answer:** (your answer)

### Question 3.4

> The real `CR4` in the VMCS has `VMXE` = 1 but the guest reads it as 0.
> Which two VMCS fields make that work? What happens if the guest tries to
> set `CR4.VMXE` to 1, and why do we not let it?

**Answer:** (your answer)

## Part 4: Entering and leaving the guest

### Question 4.1

> `struct guestregs` has no `rsp` field. Why can the guest's stack
> pointer not be handled the way the other registers are? Where does the
> CPU keep it instead, and what would go wrong if `vmx_exit` tried to
> store `%rsp` into `*regs` like the others?

**Answer:** (your answer)

### Question 4.2

> `HOST_RIP` is written once, in `vmcsinit()`, but `HOST_RSP` is written
> in `vmx_enter` on every entry. What makes one constant and the other
> not? Describe a concrete situation in which the `HOST_RSP` written by
> a previous entry would be wrong.

**Answer:** (your answer)

### Question 4.3

> `vmrun()` always calls `vmx_enter(&vm->regs, 0)`, and so always uses
> `VMLAUNCH`. Which instruction clears the launch state, and where does
> `vmrun()` execute it? What would the VM-instruction error be if the
> code passed `launched = 1` on the first entry?

**Answer:** (your answer)

### Question 4.4

> `vmxhoststate()` is called before every entry, not once. Name two
> fields it writes that can differ between two calls to `vmrun()`, and
> say what is different (CPU, process, or both) for each. What would
> happen at the next VM exit if the field kept the old value?

**Answer:** (your answer)

## Part 5: Running the guest, the vmrun loop and exit handling

### Question 5.1

> In Step 8, `vmclear()` must come before `popcli()`. Describe a
> concrete sequence of events, with two CPUs, in which the opposite
> order corrupts the VMCS. Which sentence of SDM 27.11.1 do you rely on?

**Answer:** (your answer)

### Question 5.2

> We pass `launched = 0` to `vmx_enter` on every entry, so we always use
> `VMLAUNCH`. What would the program have to do differently if we kept
> the VMCS loaded between exits and used `VMRESUME` instead? What would
> we give up?

**Answer:** (your answer)

### Question 5.3

> On an external-interrupt exit, `vmxhandle()` does nothing but return 1,
> yet the interrupt is handled by the host before the guest runs again.
> Where and when exactly does the host handle it? What would change if
> the exit control "acknowledge interrupt on exit" were set?

**Answer:** (your answer)

### Question 5.4

> The `WRMSR` exit and the `OUT` exit are both followed by
> `vm->rip += inslen`. What would happen if the code used a fixed
> length (for instance 2) instead? Name one instruction in `helloguestasm.S`
> or `helloguest.c` that shows the problem.

**Answer:** (your answer)

## Part 6: The other half: the monitor (runvm) and the guest

### Question 6.1

> In `doio()`, why does an `IN` need the monitor to write the guest's
> RAX with `vmsetreg()`, instead of `doio()` just returning a value?
> And why does `completein()` read RAX with `vmgetreg()` for a 1-byte
> `IN` but not for a 4-byte one?

**Answer:** (your answer)

### Question 6.2

> `helloguest.ld` links the guest at `0x100000`. Suppose you change it
> to `0x200000` but leave `VM_LOAD_ADDR` alone. Name two things in the
> guest that will go wrong and say why. (Look at the page tables, the
> `gdtdesc`, and where the first instruction is.)

**Answer:** (your answer)

### Question 6.3

> The guest in Step 8 sets `CR4.PAE` before `CR0.PG`. What happens if
> you swap the order of these steps? Then, for the Checkpoint's
> `runvm helloguest 1` experiment: which message do you get, and which
> check in the kernel produces it?

**Answer:** (your answer)

### Question 6.4

> Each `OUT` in the guest causes a VM exit, a trip through the kernel,
> and a return to `runvm`, which calls `write()`. Why did the design
> put the serial port in `runvm` and not in the kernel? What would you
> gain, and what would you lose, by emulating it inside the kernel?

**Answer:** (your answer)
