/* Opt-in crash reporter for static musl/ARM builds on a box with no debugger.
 *
 * Force-included (-include) only when a build sets X45_CRASH_TRACE. On SIGSEGV /
 * SIGBUS / SIGILL / SIGABRT it writes to stderr, using nothing but write(): the
 * signal, fault address, thread id, PC, LR, SP, and every word on the next 2 KiB
 * of stack that looks like a code address. Decode those against the *unstripped*
 * binary with arm-linux-addr2line / objdump; the stack scan is a poor man's
 * backtrace and over-reports (stale return addresses), but the real call chain is
 * in there.
 */
#ifndef X45_CRASH_H
#define X45_CRASH_H
#ifdef X45_CRASH_TRACE

#include <signal.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

extern char __executable_start, etext;

static void x45_crash_hex(const char *label, unsigned long v)
{
	char buf[48];
	int n = 0, i;

	while (*label && n < 24)
		buf[n++] = *label++;
	buf[n++] = '0';
	buf[n++] = 'x';
	for (i = 28; i >= 0; i -= 4)
		buf[n++] = "0123456789abcdef"[(v >> i) & 15];
	buf[n++] = '\n';
	write(2, buf, (size_t)n);
}

static void x45_crash_handler(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	unsigned long sp = uc->uc_mcontext.arm_sp, *w, lo, hi;
	int i;

	write(2, "\n=== x45 crash report ===\n", 26);
	x45_crash_hex("signal   ", (unsigned long)sig);
	x45_crash_hex("si_code  ", (unsigned long)si->si_code);
	x45_crash_hex("fault    ", (unsigned long)si->si_addr);
	x45_crash_hex("tid      ", (unsigned long)syscall(SYS_gettid));
	x45_crash_hex("pc       ", uc->uc_mcontext.arm_pc);
	x45_crash_hex("lr       ", uc->uc_mcontext.arm_lr);
	x45_crash_hex("sp       ", sp);
	x45_crash_hex("r0       ", uc->uc_mcontext.arm_r0);
	x45_crash_hex("r1       ", uc->uc_mcontext.arm_r1);
	x45_crash_hex("r4       ", uc->uc_mcontext.arm_r4);
	lo = (unsigned long)&__executable_start;
	hi = (unsigned long)&etext;
	write(2, "-- code addresses on the stack --\n", 34);
	w = (unsigned long *)sp;
	for (i = 0; i < 512; i++) {
		unsigned long v = w[i] & ~1UL;      /* drop the Thumb bit */
		if (v >= lo && v < hi)
			x45_crash_hex("ret      ", w[i]);
	}
	write(2, "=== end ===\n", 12);
	_exit(139);
}

__attribute__((constructor)) static void x45_crash_install(void)
{
	static char stk[16384];                 /* handler must not need the dead stack */
	stack_t ss;
	struct sigaction sa;

	ss.ss_sp = stk;
	ss.ss_size = sizeof stk;
	ss.ss_flags = 0;
	sigaltstack(&ss, 0);
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = x45_crash_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigaction(SIGSEGV, &sa, 0);
	sigaction(SIGBUS, &sa, 0);
	sigaction(SIGILL, &sa, 0);
	sigaction(SIGABRT, &sa, 0);
}

#endif /* X45_CRASH_TRACE */
#endif /* X45_CRASH_H */
