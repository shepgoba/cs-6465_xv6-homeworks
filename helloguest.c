// Hello-world guest for the VT-x hypervisor. Runs in 64-bit mode
// after the trampoline in helloguestasm.S. Built as a flat binary,
// not an xv6 user program: there is no libc and no xv6 kernel here.
//
// The guest has no devices except those that runvm.c emulates. It prints
// by writing characters to the I/O port of a (virtual) serial port.
// Every OUT instruction is intercepted by the hypervisor: the CPU
// leaves guest mode (a VM exit), the kernel decodes the access and
// hands it to the user-level monitor runvm.c, which prints the byte.

// I/O port of COM1, the first PC serial port. runvm.c emulates it.
#define COM1 0x3f8

// Write one byte to an I/O port with the OUT instruction.
// "a" puts data in AL (OUT takes its value from AL) and "Nd" puts the
// port in DX (or as an 8-bit immediate when it is a small constant).
// The instruction touches no memory, but "volatile" keeps the compiler
// from deleting or reordering it, since it has a side effect.
static inline void
outb(unsigned short port, unsigned char data)
{
  asm volatile("outb %0, %1" : : "a" (data), "Nd" (port));
}

/*
static inline unsigned char
inb(unsigned short port)
{
  unsigned char data;
  asm volatile("inb %1, %0" : "=a" (data) : "Nd" (port));
  return data;
}
*/

// Called from the trampoline (helloguestasm.S) once the CPU is in
// 64-bit mode with a stack. When it returns, the trampoline halts.
void
guestmain(void)
{
  const char *s;

  // Send each character of the message, up to the terminating NUL,
  // to COM1. A real driver would first poll the line status register
  // (port COM1+5) for "transmitter empty"; runvm always says yes, so
  // we skip that and just write.
  for(s = "Hello from a VT-x guest\n"; *s; s++)
    outb(COM1, *s);
/*
	outb(COM1, '0' + (inb(0x3F8 + 5) >> 5));
	outb(COM1, '0' + (inb(0x1ff)));
*/
}
