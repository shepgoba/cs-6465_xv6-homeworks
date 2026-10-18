/*
 * A minimal VT-x hypervisor.
 *
 * References: Intel SDM Volume 3 (System Programming Guide, order
 * number 325384-092, June 2026), chapters 26-33.  The pieces used
 * here are: ch. 26 (introduction to VMX), ch. 27 (the VMCS), ch. 28
 * (VMX non-root operation), ch. 29 (VM entries), ch. 30 (VM exits),
 * ch. 31 (EPT and the TLB), ch. 33 (VMX instruction reference), and
 * Appendix A-C (capability MSRs, VMCS field encodings, exit reasons).
 *
 * vmxinit() turns on VMX operation on each CPU at boot.  User programs
 * create and run VMs with the vmcreate, vmsetreg and vmrun system
 * calls (sysvmx.c), in the style of KVM: the kernel handles the exits
 * it must (host interrupts, EFER accesses) and returns every other
 * exit to the user program, which emulates the device or stops.
 *
 * Terminology.  The "host" is xv6 itself (VMX root operation); the
 * "guest" is the code run by VMLAUNCH (VMX non-root operation).  The
 * processor switches between them in hardware: a "VM entry" loads the
 * guest state area of the VMCS, and a "VM exit" saves the guest state
 * back to the VMCS and loads the host state area.
 *
 * A VM has one vCPU, a VMCS, and guest-physical memory backed by EPT.
 * The guest starts in 32-bit protected mode with paging off, which
 * needs the "unrestricted guest" control; the guest's own trampoline
 * switches to 64-bit mode.
 *
 * To keep things simple, a VMCS is loaded (VMPTRLD) only around each
 * VM entry and cleared (VMCLEAR) right after the exit, before host
 * interrupts are re-enabled.  The process may then be rescheduled on
 * any CPU, and every entry is a VMLAUNCH.
 *
 * Life of a VM, in terms of the functions below:
 *
 *   vmcreate()  allocate VMCS + EPT + guest memory, copy the image in,
 *               fill the VMCS (vmcsinit)
 *   vmsetreg()  set the initial guest registers (RIP, RSP, ...)
 *   vmrun()     loop: load VMCS, write host state, VMLAUNCH (vmx_enter
 *               in vmxasm.S), guest runs, VM exit lands in vmx_exit,
 *               read the exit information, VMCLEAR, handle the exit
 *               (vmxhandle) or return it to user space
 *   exit()      calls vmxfreeproc() to free the process's VMs
 */

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "spinlock.h"
#include "vmx.h"
#include "vmxapi.h"

/* Maximum number of VMs in the whole system (all processes). */
#define NVM 4

/*
 * Everything the kernel remembers about one VM.  The hardware-visible
 * state lives in the VMCS page; the fields here are the parts that we
 * keep in ordinary memory between vmrun() calls.
 */
struct vm {
  int used;                /* slot in use; protected by vms.lock */
  struct proc *owner;      /* only this process may use the VM */
  char *vmcs;              /* VMCS page (4 KB, 4 KB-aligned by kalloc) */
  uint64 *ept;             /* EPT PML4 (kernel virtual address) */
  uint64 eptp;             /* EPT pointer: PML4 phys addr | type | walk */
  uint64 memsz;            /* bytes of guest-physical memory */
  /*
   * Guest state kept here between runs, and copied to and from the
   * VMCS around each entry.  The general-purpose registers are not in
   * the VMCS at all: the processor leaves them alone across entry and
   * exit, so vmxasm.S loads and saves them by hand from/to regs.
   * RIP, RSP, RFLAGS and EFER do live in the VMCS (guest-state area).
   */
  struct guestregs regs;
  uint64 rip, rsp, rflags, efer;
  uint flushed;            /* CPUs where INVEPT has run for this EPTP */
};

/* The table of VMs, with one lock that protects the used/owner fields. */
static struct {
  struct spinlock lock;
  struct vm vm[NVM];
} vms = { .lock = { .name = "vms" } };

/* VMCS revision identifier from IA32_VMX_BASIC; same on every CPU. */
static uint vmcs_revision;

/* The kernel's IDT (trap.c); the host IDTR base on every VM exit. */
extern struct gatedesc idt[];
/* Exit entry point in vmxasm.S; the processor jumps here on VM exit. */
extern char vmx_exit[];
/* Assembly VM entry (vmxasm.S): returns 0 after an exit, -1 on failure. */
int vmx_enter(struct guestregs *regs, int launched);

/*
 * Execute CPUID with the given leaf (and subleaf 0).  Used by
 * vmxinit() to test for VMX support.
 */
static inline void
cpuid_insn(uint leaf, uint *a, uint *b, uint *c, uint *d)
{
  asm volatile("cpuid" : "=a" (*a), "=b" (*b), "=c" (*c), "=d" (*d)
               : "a" (leaf), "c" (0));
}

/*
 * Wrappers for the VMX instructions (SDM ch. 33).
 *
 * VMX instructions report failure in CF (VMfailInvalid: no current
 * VMCS or bad operand) or ZF (VMfailValid: the error number is in the
 * VM-instruction error field of the current VMCS); setna captures
 * either (it sets the byte when CF=1 or ZF=1).
 *
 * VMXON enters VMX operation.  pa is the physical address of the
 * VMXON region.  The instruction takes a memory operand holding that
 * address, hence the "m" constraint.  Called only by vmxinit().
 */
static inline int
vmxon(uint64 pa)
{
  uchar fail;
  asm volatile("vmxon %1\n\t setna %0" : "=q" (fail) : "m" (pa) : "cc", "memory");
  return fail ? -1 : 0;
}

/*
 * VMCLEAR: write any cached VMCS data back to memory, mark the VMCS
 * "clear" (so the next entry must be VMLAUNCH), and, if pa is the
 * current VMCS, make no VMCS current.  Called by vmcreate() and after
 * every exit in vmrun().
 */
static inline int
vmclear(uint64 pa)
{
  uchar fail;
  asm volatile("vmclear %1\n\t setna %0" : "=q" (fail) : "m" (pa) : "cc", "memory");
  return fail ? -1 : 0;
}

/*
 * VMPTRLD: make the VMCS at physical address pa the current VMCS of
 * this CPU.  VMREAD/VMWRITE/VMLAUNCH all operate on the current VMCS.
 */
static inline int
vmptrld(uint64 pa)
{
  uchar fail;
  asm volatile("vmptrld %1\n\t setna %0" : "=q" (fail) : "m" (pa) : "cc", "memory");
  return fail ? -1 : 0;
}

/*
 * VMREAD: read the field with the given encoding (vmx.h) from the
 * current VMCS.  The VMCS has an implementation-specific layout, so
 * software must use this instruction rather than touch the page.
 * We do not check for failure: callers only read valid fields of a
 * loaded VMCS.
 */
static inline uint64
vmread(uint64 field)
{
  uint64 val;
  asm volatile("vmread %1, %0" : "=r" (val) : "r" (field) : "cc");
  return val;
}

/*
 * VMWRITE: write val to a field of the current VMCS.  A failure means
 * a bug (bad encoding, read-only field, or no current VMCS), so print
 * the VM-instruction error and panic.
 */
static void
vmwrite(uint64 field, uint64 val)
{
  uchar fail;
  asm volatile("vmwrite %2, %1\n\t setna %0" : "=q" (fail) : "r" (field), "r" (val) : "cc");
  if(fail){
    cprintf("vmwrite 0x%x = 0x%x: error %d\n", field, val,
            (int)vmread(VM_INSTRUCTION_ERROR));
    panic("vmwrite");
  }
}

/*
 * INVEPT: invalidate cached guest-physical translations derived from
 * EPT (SDM 31.4.3.1).  type selects single-context (one EPTP) or
 * all-context.  The instruction takes a 128-bit descriptor in memory:
 * the EPTP in the low 64 bits and 64 reserved bits.
 */
static inline int
invept(uint64 type, uint64 eptp)
{
  struct { uint64 eptp, reserved; } desc = { eptp, 0 };
  uchar fail;
  asm volatile("invept %1, %2\n\t setna %0" : "=q" (fail) : "m" (desc), "r" (type)
               : "cc", "memory");
  return fail ? -1 : 0;
}

/*
 * Enter VMX operation on this CPU.  Called from mpmain() on every CPU.
 * On success sets mycpu()->vmx_ok; on any failure prints a message and
 * leaves the flag clear, so vmcreate()/vmrun() will refuse to work.
 * The steps follow SDM 26.7 ("Enabling and Entering VMX Operation").
 */
void
vmxinit(void)
{
  uint a, b, c, d;
  uint64 fc, cr0, cr4;
  int id = cpuid();
  char *region;

  /* CPUID.1:ECX.VMX[bit 5] says whether the CPU has VT-x at all. */
  cpuid_insn(1, &a, &b, &c, &d);
  if (!(c & CPUID1_ECX_VMX)) {
    cprintf("cpu%d: no VMX support\n", id);
    return;
  }

  fc = rdmsr(MSR_IA32_FEATURE_CONTROL);
  if(!(fc & FEATURE_CONTROL_LOCKED)){
    fc |= FEATURE_CONTROL_LOCKED | FEATURE_CONTROL_VMXON_OUTSIDE_SMX;
    wrmsr(MSR_IA32_FEATURE_CONTROL, fc);
  }

  if(!(fc & FEATURE_CONTROL_VMXON_OUTSIDE_SMX)){
    cprintf("cpu%d: VMX disabled by firmware\n", id);
    return;
  }


  cr0 = rcr0();
  cr0 |= rdmsr(MSR_IA32_VMX_CR0_FIXED0);
  cr0 &= rdmsr(MSR_IA32_VMX_CR0_FIXED1);
  lcr0(cr0);
  cr4 = rcr4() | CR4_VMXE;
  cr4 |= rdmsr(MSR_IA32_VMX_CR4_FIXED0);
  cr4 &= rdmsr(MSR_IA32_VMX_CR4_FIXED1);
  lcr4(cr4);

  vmcs_revision = rdmsr(MSR_IA32_VMX_BASIC) & 0x7FFFFFFF;
  if ((region = kalloc()) == 0){
    cprintf("cpu%d: no memory for the VMXON region\n", id);
    return;
  }
  memset(region, 0, PGSIZE);
  *(uint *)region = vmcs_revision;

  /* VMXON takes the region's physical address. */
  if (vmxon(V2P(region)) < 0) {
    cprintf("cpu%d: vmxon failed\n", id);
    kfree(region);
    return;
  }
  cpus[id].vmx_ok = 1;
  cprintf("cpu%d: VMX enabled\n", id);
}

/*
 * Compute a legal value for a VM-execution/exit/entry control field.
 *
 * Each control capability MSR (SDM Appendix A.3-A.5) has two halves:
 * the low 32 bits are the "allowed-0 settings" (a bit that is 1 here
 * must be 1 in the control: the CPU requires it), and the high 32 bits
 * are the "allowed-1 settings" (a bit that is 0 here must be 0 in the
 * control: the CPU doesn't support it).
 *
 * Combine the controls we want with those the CPU requires (the
 * allowed-0 settings, in the low half of the capability MSR), and
 * check that the CPU allows all we want (allowed-1, the high half).
 *
 * msr is the capability MSR, want the bits we need, and *ctl receives
 * the value to write to the VMCS.  Returns 0, or -1 if some wanted
 * control is unsupported.  Called only by vmcsinit().
 */
static int
adjustctl(uint msr, uint want, uint *ctl)
{
   uint64 cap = rdmsr(msr);
  /* Force on the required bits, then mask off unsupported ones. */
  uint v = (want | (uint)cap) & (uint)(cap >> 32);

  /* If masking dropped a bit we asked for, the CPU can't do it. */
  if((v & want) != want){
    cprintf("vmx: controls 0x%x unsupported (MSR 0x%x)\n",
            (uint64)(want & ~v), (uint64)msr);
    return -1;
  }
  *ctl = v;
  return 0;
}

/*
 * Write the four VMCS fields that describe one guest segment register:
 * selector, base, limit and access rights.  seg is a SEG_* index from
 * vmx.h.  The VMCS field encodings are laid out so that segment i's
 * fields are at the ES field + 2*i (see vmx.h), which this function
 * relies on.  Unlike real segment registers, the VMCS holds the
 * "hidden" descriptor cache explicitly, so we must fill it in even
 * though no GDT exists in the guest yet (SDM 27.4.1).
 */
static void
guestseg(int seg, uint sel, uint64 base, uint limit, uint ar)
{
  vmwrite(GUEST_ES_SELECTOR + 2*seg, sel);
  vmwrite(GUEST_ES_BASE + 2*seg, base);
  vmwrite(GUEST_ES_LIMIT + 2*seg, limit);
  vmwrite(GUEST_ES_AR_BYTES + 2*seg, ar);
}

/*
 * Fill in the current VMCS for a new VM.  The VMCS must have been
 * VMCLEARed and VMPTRLDed by the caller (vmcreate()).  The VMCS has
 * six areas (SDM 27.4-27.9): guest state, host state, VM-execution
 * controls, VM-exit controls, VM-entry controls, and VM-exit
 * information.  This function sets up everything that is constant for
 * the life of the VM; the rest (host state, RIP, RSP, RFLAGS, EFER)
 * is rewritten by vmrun() before each entry.  Returns 0, or -1 if the
 * CPU lacks a control we need.
 */
static int
vmcsinit(struct vm *vm)
{
  /* HW1 part 3: write this function; see HW1.md. */
	uint pin, cpu, cpu2, exitctl, entryctl;
  /*
   * IA32_VMX_BASIC bit 55 says that the "TRUE" capability MSRs exist.
   * They report the real default-1 controls; the older MSRs force
   * some bits (like CR3-load exiting) to 1 on old CPUs.
   */
  int t = (rdmsr(MSR_IA32_VMX_BASIC) & VMX_BASIC_TRUE_CTLS) != 0;
  uint64 cr0, cr4;
  int seg;

  if (adjustctl(t ? MSR_IA32_VMX_TRUE_PINBASED_CTLS : MSR_IA32_VMX_PINBASED_CTLS,
               /*
                * Pin-based: exit on external interrupts (so the host
                * kernel, not the guest, handles them) and on NMIs.
                */
               PIN_EXTINT_EXITING | PIN_NMI_EXITING, &pin) < 0 ||
     adjustctl(t ? MSR_IA32_VMX_TRUE_PROCBASED_CTLS : MSR_IA32_VMX_PROCBASED_CTLS,
               /*
                * Primary processor-based: exit on HLT, exit on every
                * IN/OUT (no I/O bitmap), and enable the secondary
                * controls word, which holds EPT and unrestricted guest.
                */
               CPU_HLT_EXITING | CPU_UNCOND_IO_EXITING | CPU_ACTIVATE_SECONDARY,
               &cpu) < 0 ||
     adjustctl(MSR_IA32_VMX_PROCBASED_CTLS2,
               /*
                * Secondary: nested paging via EPT, and unrestricted
                * guest (lets the guest run with CR0.PE/PG off, i.e.
                * real or paging-less protected mode).  Unrestricted
                * guest requires EPT.
                */
               CPU2_ENABLE_EPT | CPU2_UNRESTRICTED_GUEST, &cpu2) < 0 ||
     adjustctl(t ? MSR_IA32_VMX_TRUE_EXIT_CTLS : MSR_IA32_VMX_EXIT_CTLS,
               /*
                * Exit: the host is a 64-bit kernel, so the processor
                * must return to 64-bit mode (HOST_ADDR_SPACE_SIZE);
                * save the guest's EFER into the VMCS on exit and load
                * the host's EFER from the VMCS.  "Acknowledge
                * interrupt on exit" is deliberately not set.
                */
               EXIT_HOST_ADDR_SPACE_SIZE | EXIT_SAVE_IA32_EFER |
               EXIT_LOAD_IA32_EFER, &exitctl) < 0 ||
     adjustctl(t ? MSR_IA32_VMX_TRUE_ENTRY_CTLS : MSR_IA32_VMX_ENTRY_CTLS,
               /*
                * Entry: load the guest's EFER from the VMCS.  The
                * "IA-32e mode guest" bit is set later, in vmrun(),
                * according to the guest's EFER.LMA.
                */
               ENTRY_LOAD_IA32_EFER, &entryctl) < 0)
    return -1;

	if (cpu & (CPU_CR3_LOAD_EXITING | CPU_CR3_STORE_EXITING |
            CPU_USE_IO_BITMAPS | CPU_USE_MSR_BITMAPS)){
    cprintf("vmx: CPU forces unwanted exiting controls 0x%x\n", (uint64)cpu);
    return -1;
  }

	vmwrite(PIN_BASED_VM_EXEC_CONTROL, pin);
  vmwrite(CPU_BASED_VM_EXEC_CONTROL, cpu);
  vmwrite(SECONDARY_VM_EXEC_CONTROL, cpu2);
  vmwrite(VM_EXIT_CONTROLS, exitctl);
  vmwrite(VM_ENTRY_CONTROLS, entryctl);


vmwrite(EXCEPTION_BITMAP, 0xFFFFFFFF);  /* no guest IDT: exit on everything */

  vmwrite(PAGE_FAULT_ERROR_CODE_MASK, 0);
  vmwrite(PAGE_FAULT_ERROR_CODE_MATCH, 0);

	  /* No CR3-target values (they would let some MOV to CR3 skip exiting). */
  vmwrite(CR3_TARGET_COUNT, 0);
  /* No MSR save/load lists on exit or entry. */
  vmwrite(VM_EXIT_MSR_STORE_COUNT, 0);
  vmwrite(VM_EXIT_MSR_LOAD_COUNT, 0);
  vmwrite(VM_ENTRY_MSR_LOAD_COUNT, 0);
  /* Don't inject any event (interrupt/exception) on VM entry. */
  vmwrite(VM_ENTRY_INTR_INFO_FIELD, 0);
  /* Tell the processor where this guest's EPT lives (SDM 27.6.11). */
  vmwrite(EPT_POINTER, vm->eptp);

	vmwrite(CR0_GUEST_HOST_MASK, 0);
  vmwrite(CR0_READ_SHADOW, 0);
  vmwrite(CR4_GUEST_HOST_MASK, CR4_VMXE);
  vmwrite(CR4_READ_SHADOW, 0);

  vmwrite(HOST_RIP, (uint64)vmx_exit);

  cr0 = CR0_PE | CR0_ET | CR0_NE;
  cr0 |= rdmsr(MSR_IA32_VMX_CR0_FIXED0) & ~(uint64)(CR0_PE | CR0_PG);
  cr0 &= rdmsr(MSR_IA32_VMX_CR0_FIXED1);

	cr4 = CR4_VMXE | rdmsr(MSR_IA32_VMX_CR4_FIXED0);
  cr4 &= rdmsr(MSR_IA32_VMX_CR4_FIXED1);
  vmwrite(GUEST_CR0, cr0);
  /* CR3 is irrelevant until the guest turns paging on. */
  vmwrite(GUEST_CR3, 0);
  vmwrite(GUEST_CR4, cr4);
  /* DR7 reset value (bit 10 is reserved-1): no hardware breakpoints. */
  vmwrite(GUEST_DR7, 0x400);

	guestseg(SEG_CS, 0x08, 0, 0xFFFFFFFF, AR_CODE32);
  for(seg = SEG_ES; seg <= SEG_GS; seg++)
    if(seg != SEG_CS)
      guestseg(seg, 0x10, 0, 0xFFFFFFFF, AR_DATA32);
  /*
   * LDTR is marked unusable (as after reset, no LDT), and TR must be
   * valid for VM entry: a busy TSS (32-bit, limit 0xFFFF) is the
   * simplest value that passes the guest-state checks of SDM 29.3.1.2.
   */
  guestseg(SEG_LDTR, 0, 0, 0, AR_UNUSABLE);
  guestseg(SEG_TR, 0, 0, 0xFFFF, AR_TSS32_BUSY);

	vmwrite(GUEST_GDTR_BASE, 0);
  vmwrite(GUEST_GDTR_LIMIT, 0);
  vmwrite(GUEST_IDTR_BASE, 0);
  vmwrite(GUEST_IDTR_LIMIT, 0);

  vmwrite(GUEST_INTERRUPTIBILITY_INFO, 0);
  vmwrite(GUEST_ACTIVITY_STATE, 0);
  vmwrite(GUEST_PENDING_DBG_EXCEPTIONS, 0);
  /* No shadow VMCS: the link pointer must be all ones. */
  vmwrite(VMCS_LINK_POINTER, ~0ULL);
  /* Remaining MSR-backed guest state: all zero. */
  vmwrite(GUEST_IA32_DEBUGCTL, 0);
  vmwrite(GUEST_SYSENTER_CS, 0);
  vmwrite(GUEST_SYSENTER_ESP, 0);
  vmwrite(GUEST_SYSENTER_EIP, 0);

  return 0;
}

/*
 * Host state the processor loads on VM exit (SDM 27.5, 30.5).  It
 * depends on the CPU and the current process, so it is written before
 * every entry: after a VMCLEAR the process may resume on a different
 * CPU, which has a different TSS, GDT and per-CPU base, and different
 * processes have different page tables (CR3).
 *
 * Called by vmrun() with interrupts off and the VMCS current.
 */
static void
vmxhoststate(void)
{
  /* HW1 part 4: write this function; see HW1.md. */
	struct cpu *c = mycpu();

  /* Control registers: the kernel's own, so exits land in the kernel. */
  vmwrite(HOST_CR0, rcr0());
  /* The current process's page table; the kernel half is shared. */
  vmwrite(HOST_CR3, rcr3());
  vmwrite(HOST_CR4, rcr4());

	vmwrite(HOST_CS_SELECTOR, SEG_KCODE << 3);
  vmwrite(HOST_SS_SELECTOR, 0);
  vmwrite(HOST_DS_SELECTOR, 0);
  vmwrite(HOST_ES_SELECTOR, 0);
  vmwrite(HOST_FS_SELECTOR, 0);
  vmwrite(HOST_GS_SELECTOR, 0);
  vmwrite(HOST_TR_SELECTOR, SEG_TSS << 3);

	vmwrite(HOST_FS_BASE, rdmsr(MSR_IA32_FS_BASE));
  vmwrite(HOST_GS_BASE, rdmsr(MSR_IA32_GS_BASE));
  vmwrite(HOST_TR_BASE, (uint64)&c->ts);
  vmwrite(HOST_GDTR_BASE, (uint64)c->gdt);
  vmwrite(HOST_IDTR_BASE, (uint64)idt);

	vmwrite(HOST_IA32_SYSENTER_CS, 0);
  vmwrite(HOST_IA32_SYSENTER_ESP, 0);
  vmwrite(HOST_IA32_SYSENTER_EIP, 0);
  /* Loaded on exit because we set EXIT_LOAD_IA32_EFER in vmcsinit(). */
  vmwrite(HOST_IA32_EFER, rdmsr(EFER_MSR));
}

/*
 * Return the address of the EPT entry that maps guest-physical
 * address gpa.  If alloc is set, create any missing EPT tables.
 * Returns 0 if a table is missing and alloc is clear, or on
 * out-of-memory.
 *
 * EPT uses the same 4-level radix structure as ordinary x86-64 page
 * tables (SDM 31.3.2): 9 bits of the guest-physical address select an
 * entry at each level, starting from bits 47:39 at the PML4 and
 * ending at bits 20:12 in the page table, with the low 12 bits as the
 * page offset.  An entry is "present" if any of its R/W/X bits is set.
 * Used by vmcreate() to map guest memory and to load the image.
 */
static uint64*
eptwalk(uint64 *pml4, uint64 gpa, int alloc)
{
  uint64 *table = pml4, *e;
  char *next;
  int level;
  int index;

  /*
   * Walk levels 3 (PML4), 2 (PDPT) and 1 (PD); level 0, the page
   * table, is indexed after the loop.  Index = 9 bits starting at
   * bit 12 + 9*level.
   */
  for(level = 3; level > 0; level--){
    e = &table[(gpa >> (12 + 9*level)) & 0x1FF];
    if(*e & EPT_RWX){
      /* Present: descend.  The entry holds a physical address. */
      table = (uint64*)P2V(EPT_ADDR(*e));
    } else {
      if(!alloc || (next = kalloc()) == 0)
        return 0;
      /* kalloc() doesn't zero; a non-zero junk entry would look present. */
      memset(next, 0, PGSIZE);
      /*
       * Non-leaf entries grant R/W/X so that only the leaf entry
       * restricts permissions (the effective permission is the AND
       * over all levels).
       */
      *e = V2P(next) | EPT_RWX;
      table = (uint64*)next;
    }
  }

  index = (gpa >> 12) & 0x1ff;
  return &table[index];
}

/*
 * Free an EPT table, the tables below it, and the guest memory they
 * map.  level is 3 for the PML4 and 0 for a page table (the leaf
 * level, whose entries point to guest memory pages rather than to
 * further tables).  Called by vmfree().
 */
static void
eptfree(uint64 *table, int level)
{
  int i;

  for(i = 0; i < 512; i++){
    /* Skip entries that were never populated. */
    if(!(table[i] & EPT_RWX))
      continue;
    if(level > 0)
      eptfree((uint64*)P2V(EPT_ADDR(table[i])), level - 1);
    else
      kfree((char*)P2V(EPT_ADDR(table[i])));
  }
  /* Free this table page itself after its children. */
  kfree((char*)table);
}

/*
 * Release a VM.  The VMCS must not be current on any CPU, which holds
 * whenever the owner is not inside vmrun() (vmrun() VMCLEARs before
 * it returns).  Called by vmcreate() on failure and vmxfreeproc().
 * Safe to call on a partly constructed VM: pointers not yet allocated
 * are null (vmcreate() memsets the struct).
 */
static void
vmfree(struct vm *vm)
{
  /* Free the EPT tree and all guest pages, then the VMCS page. */
  if(vm->ept)
    eptfree(vm->ept, 3);
  if(vm->vmcs)
    kfree(vm->vmcs);
  /* Mark the slot reusable only after its resources are gone. */
  acquire(&vms.lock);
  vm->ept = 0;
  vm->vmcs = 0;
  vm->owner = 0;
  vm->used = 0;
  release(&vms.lock);
}

/*
 * Look up VM id and check that the calling process owns it, so one
 * process cannot touch another's VM.  Returns 0 if invalid.
 */
static struct vm*
getvm(int id)
{
  if(id < 0 || id >= NVM || !vms.vm[id].used || vms.vm[id].owner != myproc())
    return 0;
  return &vms.vm[id];
}

/*
 * Create a VM with memsz bytes of zeroed guest-physical memory and
 * copy image to guest-physical address VM_LOAD_ADDR.  Returns a VM
 * id, or -1.  Called from the vmcreate system call (sysvmx.c); image
 * is a kernel copy of the guest binary.
 */
int
vmcreate(char *image, int len, int memsz)
{
  struct vm *vm;
  uint64 gpa, *pte, need;
  char *mem;
  int id, n, off, r;

  if (memsz <= 0 || memsz > VM_MAXMEM || len < 0 || VM_LOAD_ADDR + len > memsz)
   	return -1;

  pushcli();
  r = mycpu()->vmx_ok;
  popcli();
  if (!r)
    return -1;

	need = EPT_CAP_WALK_4 | EPT_CAP_MEMTYPE_WB | EPT_CAP_INVEPT | EPT_CAP_INVEPT_SINGLE;
  if ((rdmsr(MSR_IA32_VMX_EPT_VPID_CAP) & need) != need){
    cprintf("vmx: CPU lacks required EPT features\n");
    return -1;
  }

  /* Claim a free VM slot under the lock. */
  acquire(&vms.lock);
  for(id = 0; id < NVM; id++)
    if(!vms.vm[id].used)
      break;
  if(id == NVM){
    release(&vms.lock);
    return -1;
  }
  vm = &vms.vm[id];
  /* Zero everything: null pointers (for vmfree), flushed = 0, regs = 0. */
  memset(vm, 0, sizeof(*vm));
  vm->used = 1;
  vm->owner = myproc();
  release(&vms.lock);

  vm->memsz = PGROUNDUP(memsz);
  vm->rflags = 0x2;  /* bit 1 is reserved and must be 1 */

	if ((vm->vmcs = kalloc()) == 0)
    goto bad;
  memset(vm->vmcs, 0, PGSIZE);

	  /* The EPT PML4: must be zeroed, since zero entries mean "not present". */
  if((vm->ept = (uint64*)kalloc()) == 0)
    goto bad;
  memset(vm->ept, 0, PGSIZE);
  /*
   * EPT pointer (SDM 27.6.11): physical address of the PML4 in the
   * high bits, EPT paging-structure memory type in bits 2:0 (6 =
   * write-back), and page-walk length minus one in bits 5:3 (3 = a
   * 4-level walk).
   */
  vm->eptp = V2P(vm->ept) | EPTP_WALK_4 | EPTP_MEMTYPE_WB;

	for(gpa = 0; gpa < vm->memsz; gpa += PGSIZE){
    if((pte = eptwalk(vm->ept, gpa, 1)) == 0 || (mem = kalloc()) == 0)
      goto bad;
    memset(mem, 0, PGSIZE);
    *pte = V2P(mem) | EPT_RWX | EPT_MEMTYPE_WB;
  }

	for(off = 0; off < len; off += n){
    gpa = VM_LOAD_ADDR + off;
    /* Bytes left in this guest page, capped by the bytes left to copy. */
    n = PGSIZE - gpa % PGSIZE;
    if(n > len - off)
      n = len - off;
    /* Already populated above (the size check guarantees it). */
    pte = eptwalk(vm->ept, gpa, 0);
    memmove((char*)P2V(EPT_ADDR(*pte)) + gpa % PGSIZE, image + off, n);
  }

  /*cprintf("vmcreate: vm %d, memsz %x, eptp %x\n", id, vm->memsz, vm->eptp);
  pte = eptwalk(vm->ept, VM_LOAD_ADDR, 0);
  cprintf("image word %x, guest word %x\n", (uint64)*(uint*)image,
          (uint64)*(uint*)P2V(EPT_ADDR(*pte)));*/
	*(uint*)vm->vmcs = vmcs_revision;
	pushcli();
	if(!mycpu()->vmx_ok || vmclear(V2P(vm->vmcs)) < 0 || vmptrld(V2P(vm->vmcs)) < 0){
		popcli();
		goto bad;
	}
	r = vmcsinit(vm);
	/*
		* Always VMCLEAR when done, even on error, so no CPU holds the
		* VMCS cached and it can be freed or loaded elsewhere.
		*/
	vmclear(V2P(vm->vmcs));
	popcli();
	if(r < 0)
		goto bad;
	return id;

bad:
  vmfree(vm);
  return -1;
}

/*
 * Set one guest register of VM id before (or between) runs.  reg is a
 * VMREG_* code from vmxapi.h.  Returns 0, or -1 for a bad VM or
 * register.  Called from the vmsetreg system call.  The values only
 * go into struct vm; vmrun() copies RIP, RSP and RFLAGS to the VMCS
 * and vmx_enter loads the general-purpose registers.
 */
int
vmsetreg(int id, int reg, uint64 val)
{
  struct vm *vm;

  if((vm = getvm(id)) == 0 || reg < 0 || reg >= VMREG_NREGS)
    return -1;
  switch(reg){
  case VMREG_RSP:
    vm->rsp = val;
    break;
  case VMREG_RIP:
    vm->rip = val;
    break;
  case VMREG_RFLAGS:
    /* RFLAGS bit 1 always reads as 1; VM entry checks for it. */
    vm->rflags = val | 0x2;
    break;
  default:
    /* VMREG_RAX..VMREG_R15 follow the layout of struct guestregs. */
    ((uint64*)&vm->regs)[reg] = val;
  }
  return 0;
}

/*
 * Read one guest register of VM id into *val.  reg is a VMREG_* code
 * from vmxapi.h.  Returns 0, or -1 for a bad VM or register.  Called
 * from the vmgetreg system call; the counterpart of vmsetreg().  The
 * values are those saved at the last exit, so this is meaningful
 * between vmrun() calls (for example, to complete an IN by merging new
 * data into RAX).
 */
int
vmgetreg(int id, int reg, uint64 *val)
{
  struct vm *vm;

 	if((vm = getvm(id)) == 0 || reg < 0 || reg >= VMREG_NREGS)
    return -1;
  switch(reg){
  case VMREG_RSP:
    *val = vm->rsp;
    break;
  case VMREG_RIP:
    *val = vm->rip;
    break;
  case VMREG_RFLAGS:
    *val = vm->rflags;
    break;
  default:
    /* VMREG_RAX..VMREG_R15 follow the layout of struct guestregs. */
    *val = ((uint64*)&vm->regs)[reg];
  }
  return 0;
}

/* Mask for the low `size` bytes (1, 2 or 4) of an I/O access. */
static uint64
iomask(int size)
{
  return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
}

/*
 * Emulate RDMSR/WRMSR.  Only IA32_EFER is supported, which the guest's
 * trampoline writes to enable long mode.  RDMSR/WRMSR pass the MSR
 * number in ECX and the 64-bit value in EDX:EAX.  Returns 0 if
 * handled, or -1 to send the exit to user space.  Called by
 * vmxhandle(); write is nonzero for WRMSR.
 */
static int
vmxmsr(struct vm *vm, int write)
{
  uint64 val;

  if((uint)vm->regs.rcx != EFER_MSR)
    return -1;
  if(write){
    val = (vm->regs.rdx << 32) | (uint)vm->regs.rax;
    /*
     * The processor sets LMA itself when the guest enables paging
     * with LME set; LMA is read-only to software, so preserve ours.
     */
    vm->efer = (val & ~(uint64)EFER_LMA) | (vm->efer & EFER_LMA);
  } else {
    /* Return the saved value; writes to EAX/EDX zero the upper halves. */
    vm->regs.rax = (uint)vm->efer;
    vm->regs.rdx = vm->efer >> 32;
  }
  return 0;
}

/*
 * Handle a VM exit.  Returns 1 to re-enter the guest, or 0 to return
 * the exit to the user program.  e holds the exit information read by
 * vmrun(), and inslen is the length of the guest instruction that
 * caused the exit (valid for exits caused by instruction execution),
 * used to skip the instruction.  Basic exit reasons are in SDM
 * Appendix C.
 */
static int
vmxhandle(struct vm *vm, struct vmexit *e, uint inslen)
{
	
	switch(e->reason){
  case EXIT_EXTERNAL_INTR:
    /*
     * The interrupt is still pending in the local APIC (we don't
     * acknowledge it on exit), so the host took it at popcli().
     * Nothing to do but resume the guest.
     */
    return 1;
  case EXIT_EXCEPTION_NMI:
    /*
     * This reason covers both guest exceptions (which we send to user
     * space for debugging) and NMIs, told apart by the interruption
     * type in the exit interruption-information field.
     */
    if(INTR_INFO_TYPE(e->intrinfo) == INTR_TYPE_NMI)
      return 1;  /* a host NMI; xv6 has no use for it */
    return 0;

	case EXIT_RDMSR:
  case EXIT_WRMSR:
    /*
     * Emulate, then advance RIP past the RDMSR/WRMSR (the processor
     * does not, since it was an exit rather than a completed
     * instruction).  Unsupported MSRs go to user space.
     */
    if(vmxmsr(vm, e->reason == EXIT_WRMSR) < 0)
      return 0;
    vm->rip += inslen;
    return 1;

	case EXIT_HLT:
    /* Skip the HLT so a later vmrun() continues after it. */
    vm->rip += inslen;
    return 0;

	case EXIT_IO:
    /* ... your code ... */
		uint64 qual = e->qual;
		e->io.port = (qual >> 16) & 0xffff;
		e->io.size = qual & 3;
		e->io.in = (qual >> 3) & 1;

		uint64 guest_rax = vm->regs.rax;

		e->io.data = guest_rax & iomask(e->io.size);
		

		vm->rip += inslen;
    return 0;
	default:
		/* Anything else (EPT violation, CPUID, triple fault, ...). */
		return 0;
	}
}

/*
 * Run the guest until an exit the user program must handle, and
 * describe that exit in *ue.  Returns 0, or -1 if the VM doesn't exist
 * or the process was killed.  Called from the vmrun system call.
 *
 * Invariants maintained by every iteration of the loop:
 *  - interrupts are off (pushcli) while the VMCS is current, so the
 *    process stays on one CPU;
 *  - the host state is rewritten before every entry.
 */
int
vmrun(int id, struct vmexit *ue)
{
  struct vm *vm;
  struct vmexit e;
  uint64 reason, entryctl;
  uint inslen;
  int c, r;

  if((vm = getvm(id)) == 0)
    return -1;

  for(;;){
    /* Check for kill between guest runs, since the guest may spin. */
    if(myproc()->killed)
      return -1;
    memset(&e, 0, sizeof(e));
    inslen = 0;

    /*
     * Disable interrupts: from VMPTRLD until VMCLEAR the VMCS is
     * bound to this CPU, so we must not be rescheduled elsewhere.
     */
    pushcli();
    c = cpuid();
    if(!cpus[c].vmx_ok || vmptrld(V2P(vm->vmcs)) < 0){
      popcli();
      return -1;
    }
    /*
     * Guest-physical translations are cached per CPU and tagged by
     * EPTP.  Flush any left from an earlier VM whose EPT used the same
     * page, the first time this VM runs on this CPU.
     * (SDM 31.4.3.4: software must INVEPT when it changes mappings or
     * reuses an EPT root.)  One bit per CPU records that we did so.
     */
    if(!(vm->flushed & (1 << c))){
      invept(INVEPT_SINGLE_CONTEXT, vm->eptp);
      vm->flushed |= 1 << c;
    }
    /* Host state: CPU- and process-specific, so rewrite it every time. */
    vmxhoststate();
    /* Guest state that changes outside the guest: copy into the VMCS. */
    vmwrite(GUEST_RIP, vm->rip);
    vmwrite(GUEST_RSP, vm->rsp);
    vmwrite(GUEST_RFLAGS, vm->rflags);
    vmwrite(GUEST_IA32_EFER, vm->efer);
    /*
     * "IA-32e mode guest" must match EFER.LMA, which the guest sets
     * by enabling paging with EFER.LME = 1.  (VM-entry checks, SDM
     * 29.3.1.1.)  So recompute the control from the saved EFER on
     * every entry: clear the bit, then set it if LMA is on.
     */
    entryctl = vmread(VM_ENTRY_CONTROLS) & ~(uint64)ENTRY_IA32E_MODE_GUEST;
    if(vm->efer & EFER_LMA)
      entryctl |= ENTRY_IA32E_MODE_GUEST;
    vmwrite(VM_ENTRY_CONTROLS, entryctl);

    /*
     * Enter the guest (vmxasm.S).  The second argument is "launched":
     * 0 means VMLAUNCH.  Because we VMCLEAR after every exit, the VMCS
     * is always in the clear state here, so we never use VMRESUME.
     * Returns 0 after a VM exit, or -1 if the entry instruction itself
     * failed (invalid controls or host/guest state).
     */
    r = vmx_enter(&vm->regs, 0);

    /*
     * Back in the host.  The VMCS is still current on this CPU, so
     * read everything we need now, before VMCLEAR.
     */
    if(r < 0){
      /* VMLAUNCH failed: report the VM-instruction error (SDM 33.4). */
      e.reason = EXIT_VMFAIL;
      e.error = vmread(VM_INSTRUCTION_ERROR);
      e.rip = vm->rip;
    } else {
      /*
       * Exit reason: bits 15:0 are the basic reason, and bit 31 is
       * set if this was a failed VM entry (SDM 30.2.1), for example
       * invalid guest state.
       */
      reason = vmread(VM_EXIT_REASON);
      e.reason = reason & 0xFFFF;
      e.entryfail = (reason & EXIT_REASON_ENTRY_FAILURE) != 0;
      /* Extra detail whose meaning depends on the exit reason. */
      e.qual = vmread(EXIT_QUALIFICATION);
      e.intrinfo = vmread(VM_EXIT_INTR_INFO);
      e.intrerr = vmread(VM_EXIT_INTR_ERROR_CODE);
      /* Faulting guest-physical address, for EPT violations. */
      e.gpa = vmread(GUEST_PHYSICAL_ADDRESS);
      /* Length of the exiting instruction, to skip it. */
      inslen = vmread(VM_EXIT_INSTRUCTION_LEN);
      /* Save the guest state the processor stored in the VMCS. */
      vm->rip = vmread(GUEST_RIP);
      vm->rsp = vmread(GUEST_RSP);
      vm->rflags = vmread(GUEST_RFLAGS);
      /* Saved because of EXIT_SAVE_IA32_EFER; picks up a new LMA. */
      vm->efer = vmread(GUEST_IA32_EFER);
      e.rip = vm->rip;
    }
    /* Release the VMCS and turn interrupts back on (HW1 part 5, Step 8). */
	vmclear(V2P(vm->vmcs));
    popcli();  /* a pending host interrupt is delivered here */

    /*
     * Failed entries and VMLAUNCH failures always go to user space.
     * Otherwise vmxhandle() decides; returning 1 loops to re-enter.
     */
    if(e.reason == EXIT_VMFAIL || e.entryfail || !vmxhandle(vm, &e, inslen))
      break;
  }
  /* Report the exit that ended the loop to the user program. */
  *ue = e;
  return 0;
}

/*
 * Free the VMs of a process that is exiting.  Called from exit() in
 * proc.c.  The process cannot be inside vmrun(), so no VMCS is
 * current, as vmfree() requires.
 */
void
vmxfreeproc(struct proc *p)
{
  int id;

  for(id = 0; id < NVM; id++)
    if(vms.vm[id].used && vms.vm[id].owner == p)
      vmfree(&vms.vm[id]);
}
