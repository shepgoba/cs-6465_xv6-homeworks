/*
 * runvm: run a guest image from the file system in a VT-x VM.
 *
 *   runvm image [memory-MB]
 *
 * Loads the image at guest-physical VM_LOAD_ADDR, starts the guest
 * there, and emulates the guest's serial port: bytes the guest writes
 * to COM1 appear on the console. Stops when the guest halts.
 *
 * ROLE OF THIS PROGRAM
 *
 * runvm is the "virtual machine monitor" (VMM) in user space. It plays
 * the role that QEMU plays on top of KVM. The split of work is:
 *
 *   - The kernel (vmx.c) does the privileged part: it owns the VMCS and
 *     the EPT, executes VMLAUNCH, and handles the few VM exits that
 *     only the kernel can handle (host interrupts, the WRMSR to EFER).
 *   - This program does everything else: it decides what the guest
 *     image is, where it starts, and what the guest's "hardware"
 *     looks like. Here the only device is a transmit-only COM1 serial
 *     port.
 *
 * The control flow is the same as in any KVM-based monitor:
 *
 *   vmcreate()   allocate a VM and copy the image into guest memory
 *   vmsetreg()   set the guest's initial registers (here, only RIP)
 *   loop:
 *     vmrun()    enter the guest; returns when the guest does something
 *                the kernel cannot handle by itself (a "VM exit")
 *     inspect struct vmexit, emulate the event, go around again
 *
 * Whenever the guest executes an instruction that the hypervisor
 * configured to trap (OUT/IN, HLT, ...), the CPU leaves guest mode and
 * vmrun() returns here with the reason in struct vmexit (vmxapi.h).
 */

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "vmxapi.h"

/* I/O port of the first serial port (UART 8250/16550) on a PC. */
#define COM1 0x3f8
/* Upper bound on the image size; the image is read into a static buffer. */
#define MAXIMAGE (256*1024)

/* Buffer for the guest image, passed to vmcreate() (a system call). */
static char image[MAXIMAGE];

/*
 * Give the guest the result of an IN of `size` bytes (1, 2 or 4) by
 * writing it into the guest's RAX (vmgetreg, vmsetreg).
 * HW1 part 6: write this function; see HW1.md.
 */
static void
completein(int vm, int size, uint val)
{
	uint64 rax;
	vmgetreg(vm, VMREG_RAX, &rax);


	if (size == 1) {
		uint64 new_rax = (rax & 0xffffffffffffff00) | (val & 0xff);
		vmsetreg(vm, VMREG_RAX, new_rax);
	} else if (size == 2) {
		uint64 new_rax = (rax & 0xffffffffffff0000) | (val & 0xffff);
		vmsetreg(vm, VMREG_RAX, new_rax);
	} else {
		// size = 4
		vmsetreg(vm, VMREG_RAX, val);
	}
}

/*
 * Emulate the guest's port I/O: a transmit-only COM1.
 *
 * Called on an EXIT_IO exit. The kernel already decoded the access
 * into e->io: the port number, the size, the direction, and, for an
 * OUT, the value the guest wrote (data). It also advanced the guest's
 * RIP past the OUT/IN, so after we return the guest simply continues.
 *
 * For an IN, we must supply the value the guest reads, by writing it
 * into the guest's RAX (completein) before the next vmrun().
 */
static void
doio(int vm, struct vmexit *e)
{
  char c;

  /* OUT to the UART's transmit register: print the byte on our console
   * (file descriptor 1). This is how the guest's text reaches the user. */
  if(e->io.port == COM1 && !e->io.in){
    c = e->io.data;
    write(1, &c, 1);
    return;
  }
  /* IN from the line status register (COM1+5): a real driver polls this
   * until the "transmitter empty" bits (0x20 THRE, 0x40 TEMT) are set.
   * We are always ready to accept a byte, so report both bits. */
  if(e->io.port == COM1 + 5 && e->io.in){
    completein(vm, e->io.size, 0x60);  // line status: transmitter empty
    return;
  }
  /* Any other IN targets a port with no device behind it. Real buses
   * return all ones for such reads; OUTs to unknown ports are ignored. */
  if(e->io.in)
    completein(vm, e->io.size, 0xFFFFFFFF);  // no device: reads float high
}

int
main(int argc, char *argv[])
{
  struct vmexit e;   /* filled in by the kernel at every VM exit */
  int fd, n, len, vm, memmb;

  /* Command line: the image file, and optionally the size of guest
   * physical memory in megabytes. */
  if(argc < 2 || argc > 3){
    printf(2, "usage: runvm image [memory-MB]\n");
    exit();
  }
  memmb = argc == 3 ? atoi(argv[2]) : 4;

  /* Read the whole image into memory. The kernel never opens files on
   * behalf of the hypervisor, so the monitor does the file I/O. The loop
   * reads until end of file or until the buffer is full. */
  if((fd = open(argv[1], O_RDONLY)) < 0){
    printf(2, "runvm: cannot open %s\n", argv[1]);
    exit();
  }
  for(len = 0; len < MAXIMAGE && (n = read(fd, image + len, MAXIMAGE - len)) > 0; len += n)
    ;
  close(fd);
  /* A full buffer means the file may have been truncated; refuse. */
  if(len == MAXIMAGE){
    printf(2, "runvm: %s is larger than %d bytes\n", argv[1], MAXIMAGE);
    exit();
  }

  /* Step 1: create the VM. The kernel allocates the VMCS and the EPT
   * (guest-physical to host-physical page tables), allocates and zeroes
   * memmb megabytes of guest memory, and copies the image to
   * guest-physical address VM_LOAD_ADDR. Returns a VM id, like a file
   * descriptor, used by the other calls. */
  if((vm = vmcreate(image, len, memmb * 1024 * 1024)) < 0){
    printf(2, "runvm: vmcreate failed\n");
    exit();
  }
  /* Step 2: set the guest's initial registers. All other registers
   * keep the defaults chosen by the kernel (flat 32-bit protected mode,
   * paging off; see vmcsinit() in vmx.c). Execution starts at the first
   * byte of the image, the "start" label of helloguestasm.S. */
  vmsetreg(vm, VMREG_RIP, VM_LOAD_ADDR);

  /* Step 3: the run loop. Each vmrun() executes the guest until a VM
   * exit that the kernel cannot handle itself. */
  for(;;){
    if(vmrun(vm, &e) < 0){
      printf(2, "runvm: vmrun failed\n");
      exit();
    }
    /* A failed VM *entry* (rather than an exit from the guest). The
     * reason field then is the failure code, e.g. 33 for invalid guest
     * state; qual says which check failed. Usually a bug in the
     * guest-state setup. */
    if(e.entryfail){
      printf(2, "runvm: VM entry failed, reason %d, qualification 0x%x\n",
             e.reason, e.qual);
      exit();
    }
    /* Dispatch on the basic exit reason (EXIT_* in vmxapi.h). */
    switch(e.reason){
    case EXIT_IO:
      /* The guest executed IN or OUT: emulate the device, then resume
       * the guest (the continue goes to the next vmrun()). */
      doio(vm, &e);
      continue;
    case EXIT_HLT:
      /* The guest executed HLT. Our guest does so when it is done, so
       * treat this as normal termination. e.rip is the address of the HLT
       * itself; the kernel has already advanced the guest's RIP past it. */
      printf(1, "runvm: guest halted at rip 0x%x\n", e.rip);
      exit();
    case EXIT_VMFAIL:
      /* Not a hardware exit: the VMLAUNCH instruction itself failed
       * (bad VMCS contents). error is the VM-instruction error number
       * from the Intel SDM. */
      printf(2, "runvm: VMLAUNCH failed, VM-instruction error %d\n", e.error);
      exit();
    case EXIT_EXCEPTION_NMI:
      /* The guest took a CPU exception. The hypervisor intercepts all
       * of them since the guest has no IDT. The low 8 bits of the
       * interruption information are the vector (14 = page fault...). */
      printf(2, "runvm: guest exception %d, error code 0x%x, rip 0x%x\n",
             e.intrinfo & 0xff, (uint64)e.intrerr, e.rip);
      exit();
    case EXIT_TRIPLE_FAULT:
      /* The guest faulted while delivering a fault; a real CPU would
       * reset. Common cause: running with a broken page table or GDT. */
      printf(2, "runvm: guest triple fault at rip 0x%x\n", e.rip);
      exit();
    case EXIT_EPT_VIOLATION:
      /* The guest touched guest-physical memory that has no EPT
       * mapping, i.e. beyond the memory size given to vmcreate(). gpa
       * is the offending guest-physical address. */
      printf(2, "runvm: EPT violation at guest-physical 0x%x, rip 0x%x\n",
             e.gpa, e.rip);
      exit();
    }
    /* Any other reason (CPUID, a WRMSR the kernel does not emulate,
     * ...) is not supported by this monitor: report it and give up. */
    printf(2, "runvm: unhandled exit %d, qualification 0x%x, rip 0x%x\n",
           e.reason, e.qual, e.rip);
    exit();
  }
}
