#include "threads/interrupt.h"
#include <debug.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include "threads/flags.h"
#include "threads/intr-stubs.h"
#include "threads/io.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/gdt.h"
#endif

/* Number of x86_64 interrupts. */
#define INTR_CNT 256

/* Creates an gate that invokes FUNCTION.

   The gate has descriptor privilege level DPL, meaning that it
   can be invoked intentionally when the processor is in the DPL
   or lower-numbered ring.  In practice, DPL==3 allows user mode
   to call into the gate and DPL==0 prevents such calls.  Faults
   and exceptions that occur in user mode still cause gates with
   DPL==0 to be invoked.

   TYPE must be either 14 (for an interrupt gate) or 15 (for a
   trap gate).  The difference is that entering an interrupt gate
   disables interrupts, but entering a trap gate does not.  See
   [IA32-v3a] section 5.12.1.2 "Flag Usage By Exception- or
   Interrupt-Handler Procedure" for discussion. */

struct gate {
	unsigned off_15_0 : 16;   // low 16 bits of offset in segment
	unsigned ss : 16;         // segment selector
	unsigned ist : 3;        // # args, 0 for interrupt/trap gates
	unsigned rsv1 : 5;        // reserved(should be zero I guess)
	unsigned type : 4;        // type(STS_{TG,IG32,TG32})
	unsigned s : 1;           // must be 0 (system)
	unsigned dpl : 2;         // descriptor(meaning new) privilege level
	unsigned p : 1;           // Present
	unsigned off_31_16 : 16;  // high bits of offset in segment
	uint32_t off_32_63;
	uint32_t rsv2;
};

/* The Interrupt Descriptor Table (IDT).  The format is fixed by
   the CPU.  See [IA32-v3a] sections 5.10 "Interrupt Descriptor
   Table (IDT)", 5.11 "IDT Descriptors", 5.12.1.2 "Flag Usage By
   Exception- or Interrupt-Handler Procedure". */
static struct gate idt[INTR_CNT];

static struct desc_ptr idt_desc = {
	.size = sizeof(idt) - 1,
	.address = (uint64_t) idt
};


#define make_gate(g, function, d, t) \
{ \
	ASSERT ((function) != NULL); \
	ASSERT ((d) >= 0 && (d) <= 3); \
	ASSERT ((t) >= 0 && (t) <= 15); \
	*(g) = (struct gate) { \
		.off_15_0 = (uint64_t) (function) & 0xffff, \
		.ss = SEL_KCSEG, \
		.ist = 0, \
		.rsv1 = 0, \
		.type = (t), \
		.s = 0, \
		.dpl = (d), \
		.p = 1, \
		.off_31_16 = ((uint64_t) (function) >> 16) & 0xffff, \
		.off_32_63 = ((uint64_t) (function) >> 32) & 0xffffffff, \
		.rsv2 = 0, \
	}; \
}

/* Creates an interrupt gate that invokes FUNCTION with the given DPL. */
#define make_intr_gate(g, function, dpl) make_gate((g), (function), (dpl), 14)

/* Creates a trap gate that invokes FUNCTION with the given DPL. */
#define make_trap_gate(g, function, dpl) make_gate((g), (function), (dpl), 15)



/* Interrupt handler functions for each interrupt. */
static intr_handler_func *intr_handlers[INTR_CNT];

/* Names for each interrupt, for debugging purposes. */
static const char *intr_names[INTR_CNT];

/* External interrupts are those generated by devices outside the
   CPU, such as the timer.  External interrupts run with
   interrupts turned off, so they never nest, nor are they ever
   pre-empted.  Handlers for external interrupts also may not
   sleep, although they may invoke intr_yield_on_return() to
   request that a new process be scheduled just before the
   interrupt returns. */
static bool in_external_intr;   /* Are we processing an external interrupt? */
static bool yield_on_return;    /* Should we yield on interrupt return? */

/* Programmable Interrupt Controller helpers. */
static void pic_init (void);
static void pic_end_of_interrupt (int irq);

/* Interrupt handlers. */
void intr_handler (struct intr_frame *args);

/* Returns the current interrupt status. */
enum intr_level
intr_get_level (void) {
	uint64_t flags;

	/* Push the flags register on the processor stack, then pop the
	   value off the stack into `flags'.  See [IA32-v2b] "PUSHF"
	   and "POP" and [IA32-v3a] 5.8.1 "Masking Maskable Hardware
	   Interrupts". */
	asm volatile ("pushfq; popq %0" : "=g" (flags));

	return flags & FLAG_IF ? INTR_ON : INTR_OFF;
}

/* Enables or disables interrupts as specified by LEVEL and
   returns the previous interrupt status. */
/* 입력에 맞게 인터럽트 레벨을 ON/OFF */
enum intr_level intr_set_level (enum intr_level level) {
	return level == INTR_ON ? intr_enable () : intr_disable ();
}

/* Enables interrupts and returns the previous interrupt status. */
enum intr_level
intr_enable (void) {
	enum intr_level old_level = intr_get_level ();
	ASSERT (!intr_context ());

	/* Enable interrupts by setting the interrupt flag.

	   See [IA32-v2b] "STI" and [IA32-v3a] 5.8.1 "Masking Maskable
	   Hardware Interrupts". */
	asm volatile ("sti");

	return old_level;
}

/* Disables interrupts and returns the previous interrupt status. */
// 인터럽트를 중지하고, 이전 인터럽트 상태를 반환하는 함수
enum intr_level intr_disable (void) {
	enum intr_level old_level = intr_get_level ();

	/* Disable interrupts by clearing the interrupt flag.
	   See [IA32-v2b] "CLI" and [IA32-v3a] 5.8.1 "Masking Maskable
	   Hardware Interrupts". */
	asm volatile ("cli" : : : "memory");

	return old_level;
}

/* Initializes the interrupt system. */
void
intr_init (void) {
	int i;

	/* Initialize interrupt controller. */
	pic_init ();

	/* Initialize IDT. */
	for (i = 0; i < INTR_CNT; i++) {
		make_intr_gate(&idt[i], intr_stubs[i], 0);
		intr_names[i] = "unknown";
	}

#ifdef USERPROG
	/* Load TSS. */
	ltr (SEL_TSS);
#endif

	/* Load IDT register. */
	lidt(&idt_desc);

	/* Initialize intr_names. */
	intr_names[0] = "#DE Divide Error";
	intr_names[1] = "#DB Debug Exception";
	intr_names[2] = "NMI Interrupt";
	intr_names[3] = "#BP Breakpoint Exception";
	intr_names[4] = "#OF Overflow Exception";
	intr_names[5] = "#BR BOUND Range Exceeded Exception";
	intr_names[6] = "#UD Invalid Opcode Exception";
	intr_names[7] = "#NM Device Not Available Exception";
	intr_names[8] = "#DF Double Fault Exception";
	intr_names[9] = "Coprocessor Segment Overrun";
	intr_names[10] = "#TS Invalid TSS Exception";
	intr_names[11] = "#NP Segment Not Present";
	intr_names[12] = "#SS Stack Fault Exception";
	intr_names[13] = "#GP General Protection Exception";
	intr_names[14] = "#PF Page-Fault Exception";
	intr_names[16] = "#MF x87 FPU Floating-Point Error";
	intr_names[17] = "#AC Alignment Check Exception";
	intr_names[18] = "#MC Machine-Check Exception";
	intr_names[19] = "#XF SIMD Floating-Point Exception";
}

/* Registers interrupt VEC_NO to invoke HANDLER with descriptor
   privilege level DPL.  Names the interrupt NAME for debugging
   purposes.  The interrupt handler will be invoked with
   interrupt status set to LEVEL. */
static void

register_handler (uint8_t vec_no, int dpl, enum intr_level level,
		intr_handler_func *handler, const char *name) {
	ASSERT (intr_handlers[vec_no] == NULL);
	if (level == INTR_ON) {
		make_trap_gate(&idt[vec_no], intr_stubs[vec_no], dpl);
	}
	else {
		make_intr_gate(&idt[vec_no], intr_stubs[vec_no], dpl);
	}
	intr_handlers[vec_no] = handler;
	intr_names[vec_no] = name;
}

/* Registers external interrupt VEC_NO to invoke HANDLER, which
   is named NAME for debugging purposes.  The handler will
   execute with interrupts disabled. */
void
intr_register_ext (uint8_t vec_no, intr_handler_func *handler,
		const char *name) {
	ASSERT (vec_no >= 0x20 && vec_no <= 0x2f);
	register_handler (vec_no, 0, INTR_OFF, handler, name);
}

/* Registers internal interrupt VEC_NO to invoke HANDLER, which
   is named NAME for debugging purposes.  The interrupt handler
   will be invoked with interrupt status LEVEL.

   The handler will have descriptor privilege level DPL, meaning
   that it can be invoked intentionally when the processor is in
   the DPL or lower-numbered ring.  In practice, DPL==3 allows
   user mode to invoke the interrupts and DPL==0 prevents such
   invocation.  Faults and exceptions that occur in user mode
   still cause interrupts with DPL==0 to be invoked.  See
   [IA32-v3a] sections 4.5 "Privilege Levels" and 4.8.1.1
   "Accessing Nonconforming Code Segments" for further
   discussion. */
void
intr_register_int (uint8_t vec_no, int dpl, enum intr_level level,
		intr_handler_func *handler, const char *name)
{
	ASSERT (vec_no < 0x20 || vec_no > 0x2f);
	register_handler (vec_no, dpl, level, handler, name);
}

/* Returns true during processing of an external interrupt
   and false at all other times. */
/* 외부 인터럽트(CPU 외부의 I/O 디바이스나 timer 등)가 들어왔는지 확인 */
bool intr_context (void) {
	return in_external_intr;
}

/* During processing of an external interrupt, directs the
   interrupt handler to yield to a new process just before
   returning from the interrupt.  May not be called at any other
   time. */
void
intr_yield_on_return (void) {
	ASSERT (intr_context ());
	yield_on_return = true;
}

/* 8259A Programmable Interrupt Controller. */

/* Every PC has two 8259A Programmable Interrupt Controller (PIC)
   chips.  One is a "master" accessible at ports 0x20 and 0x21.
   The other is a "slave" cascaded onto the master's IRQ 2 line
   and accessible at ports 0xa0 and 0xa1.  Accesses to port 0x20
   set the A0 line to 0 and accesses to 0x21 set the A1 line to
   1.  The situation is similar for the slave PIC.

   By default, interrupts 0...15 delivered by the PICs will go to
   interrupt vectors 0...15.  Unfortunately, those vectors are
   also used for CPU traps and exceptions.  We reprogram the PICs
   so that interrupts 0...15 are delivered to interrupt vectors
   32...47 (0x20...0x2f) instead. */

/* Initializes the PICs.  Refer to [8259A] for details. */
static void
pic_init (void) {
	/* Mask all interrupts on both PICs. */
	outb (0x21, 0xff);
	outb (0xa1, 0xff);

	/* Initialize master. */
	outb (0x20, 0x11); /* ICW1: single mode, edge triggered, expect ICW4. */
	outb (0x21, 0x20); /* ICW2: line IR0...7 -> irq 0x20...0x27. */
	outb (0x21, 0x04); /* ICW3: slave PIC on line IR2. */
	outb (0x21, 0x01); /* ICW4: 8086 mode, normal EOI, non-buffered. */

	/* Initialize slave. */
	outb (0xa0, 0x11); /* ICW1: single mode, edge triggered, expect ICW4. */
	outb (0xa1, 0x28); /* ICW2: line IR0...7 -> irq 0x28...0x2f. */
	outb (0xa1, 0x02); /* ICW3: slave ID is 2. */
	outb (0xa1, 0x01); /* ICW4: 8086 mode, normal EOI, non-buffered. */

	/* Unmask all interrupts. */
	outb (0x21, 0x00);
	outb (0xa1, 0x00);
}

/* Sends an end-of-interrupt signal to the PIC for the given IRQ.
   If we don't acknowledge the IRQ, it will never be delivered to
   us again, so this is important.  */
static void
pic_end_of_interrupt (int irq) {
	ASSERT (irq >= 0x20 && irq < 0x30);

	/* Acknowledge master PIC. */
	outb (0x20, 0x20);

	/* Acknowledge slave PIC if this is a slave interrupt. */
	if (irq >= 0x28)
		outb (0xa0, 0x20);
}
/* Interrupt handlers. */

/* Handler for all interrupts, faults, and exceptions.  This
   function is called by the assembly language interrupt stubs in
   intr-stubs.S.  FRAME describes the interrupt and the
   interrupted thread's registers. */
void
intr_handler (struct intr_frame *frame) {
	bool external;
	intr_handler_func *handler;

	/* External interrupts are special.
	   We only handle one at a time (so interrupts must be off)
	   and they need to be acknowledged on the PIC (see below).
	   An external interrupt handler cannot sleep. */
	external = frame->vec_no >= 0x20 && frame->vec_no < 0x30;
	if (external) {
		ASSERT (intr_get_level () == INTR_OFF);
		ASSERT (!intr_context ());

		in_external_intr = true;
		yield_on_return = false;
	}

	/* Invoke the interrupt's handler. */
	handler = intr_handlers[frame->vec_no];
	if (handler != NULL)
		handler (frame);
	else if (frame->vec_no == 0x27 || frame->vec_no == 0x2f) {
		/* There is no handler, but this interrupt can trigger
		   spuriously due to a hardware fault or hardware race
		   condition.  Ignore it. */
	} else {
		/* No handler and not spurious.  Invoke the unexpected
		   interrupt handler. */
		intr_dump_frame (frame);
		PANIC ("Unexpected interrupt");
	}

	/* Complete the processing of an external interrupt. */
	if (external) {
		ASSERT (intr_get_level () == INTR_OFF);
		ASSERT (intr_context ());

		in_external_intr = false;
		pic_end_of_interrupt (frame->vec_no);

		if (yield_on_return)
			thread_yield ();
	}
}

/* Dumps interrupt frame F to the console, for debugging. */
void
intr_dump_frame (const struct intr_frame *f) {
	/* CR2 is the linear address of the last page fault.
	   See [IA32-v2a] "MOV--Move to/from Control Registers" and
	   [IA32-v3a] 5.14 "Interrupt 14--Page Fault Exception
	   (#PF)". */
	uint64_t cr2 = rcr2();
	printf ("Interrupt %#04llx (%s) at rip=%llx\n",
			f->vec_no, intr_names[f->vec_no], f->rip);
	printf (" cr2=%016llx error=%16llx\n", cr2, f->error_code);
	printf ("rax %016llx rbx %016llx rcx %016llx rdx %016llx\n",
			f->R.rax, f->R.rbx, f->R.rcx, f->R.rdx);
	printf ("rsp %016llx rbp %016llx rsi %016llx rdi %016llx\n",
			f->rsp, f->R.rbp, f->R.rsi, f->R.rdi);
	printf ("rip %016llx r8 %016llx  r9 %016llx r10 %016llx\n",
			f->rip, f->R.r8, f->R.r9, f->R.r10);
	printf ("r11 %016llx r12 %016llx r13 %016llx r14 %016llx\n",
			f->R.r11, f->R.r12, f->R.r13, f->R.r14);
	printf ("r15 %016llx rflags %08llx\n", f->R.r15, f->eflags);
	printf ("es: %04x ds: %04x cs: %04x ss: %04x\n",
			f->es, f->ds, f->cs, f->ss);
}

/* Returns the name of interrupt VEC. */
const char *
intr_name (uint8_t vec) {
	return intr_names[vec];
}
