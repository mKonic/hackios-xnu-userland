/*
 * libredwood-chrono.dylib: code of ours inside chronod, the daemon that asks the home screen's widgets for
 * their timelines (see inject.c for how it gets there).
 *
 *   - Widget sessions get ten times WidgetKit's time before their watchdog fires. chronod gives every
 *     request to a widget extension a deadline (WidgetKit's DefaultWatchdogTimeoutProvider: 10 s for
 *     getPlaceholders and its like, up to 90 s), cancels the session when it passes, marks the reload
 *     failed and tries again later. Under QEMU a SwiftUI widget (Calendar, Weather) takes longer than
 *     that to archive its views, so every attempt was cancelled just before it ended and made again,
 *     several times a boot: a third of the processors for the first two minutes, and the widgets stayed
 *     blank. The deadlines of the operations without a count are a table of doubles in WidgetKit's
 *     constant data (on a page with no code); a private copy of that page has them ten times longer.
 *     getTimelines(n)'s 30 s a timeline and the 10 s of the other operation with a count are immediates
 *     in the closure's code (fmov d1, #30.0 / #10.0): in a private copy of that code page they become
 *     loads of the table's 30 s and 10 s entries, ten times longer by then.
 */
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CACHE_BASE      0x180000000UL           /* where the cache was built to be, before the slide */
/* iOS 16.5 (20F66) WidgetKit: the closure of DefaultWatchdogTimeoutProvider reads its deadlines here */
#define WATCHDOG_TABLE  0x1935458e0UL
#define WATCHDOG_COUNT  6
#define WATCHDOG_SCALE  10
/* ... and its two immediates: fmov d1, #10.0 and fmov d1, #30.0, made ldr d1, table[0] and table[3] */
#define WATCHDOG_FMOV10 0x1934f82ccUL
#define WATCHDOG_FMOV30 0x1934f82dcUL

static const double watchdog_expected[WATCHDOG_COUNT] = { 10, 10, 15, 30, 90, 30 };

extern const void *_dyld_get_shared_cache_range(size_t *length);

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

static void
longer_watchdogs(uintptr_t slide)
{
	double *table = (double *)(WATCHDOG_TABLE + slide);
	vm_address_t page = (vm_address_t)table & ~(vm_address_t)(vm_page_size - 1);
	double scaled[WATCHDOG_COUNT];

	if (memcmp(table, watchdog_expected, sizeof(watchdog_expected)) != 0) {
		say("redwood: WidgetKit's watchdog deadlines are not where they were; left alone\n");
		return;
	}
	for (int i = 0; i < WATCHDOG_COUNT; i++) {
		scaled[i] = watchdog_expected[i] * WATCHDOG_SCALE;
	}
	/* constant data shared by every process: a private writable copy of the page */
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("redwood: WidgetKit's watchdog deadlines: cannot make the page writable\n");
		return;
	}
	memcpy(table, scaled, sizeof(scaled));
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	say("redwood: widget sessions get %dx WidgetKit's time before their watchdog fires\n", WATCHDOG_SCALE);
}

/* A load of the 64-bit literal at `target` into d1, for an instruction at `pc`. */
static uint32_t
ldr_d1(uintptr_t pc, uintptr_t target)
{
	return 0x5c000000u | (uint32_t)(((target - pc) >> 2) << 5) | 1u;
}

static void
longer_counted_watchdogs(uintptr_t slide)
{
	uint32_t *fmov10 = (uint32_t *)(WATCHDOG_FMOV10 + slide), *fmov30 = (uint32_t *)(WATCHDOG_FMOV30 + slide);
	vm_address_t page = (vm_address_t)fmov10 & ~(vm_address_t)(vm_page_size - 1);
	double *table = (double *)(WATCHDOG_TABLE + slide);

	if (*fmov10 != 0x1e649001u || *fmov30 != 0x1e67d001u || table[0] != 10 * WATCHDOG_SCALE ||
	    table[3] != 30 * WATCHDOG_SCALE || page != ((vm_address_t)fmov30 & ~(vm_address_t)(vm_page_size - 1))) {
		say("redwood: WidgetKit's watchdog code is not as expected; getTimelines keeps its deadline\n");
		return;
	}
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("redwood: WidgetKit's watchdog code: cannot make the page writable\n");
		return;
	}
	*fmov10 = ldr_d1((uintptr_t)fmov10, (uintptr_t)&table[0]);
	*fmov30 = ldr_d1((uintptr_t)fmov30, (uintptr_t)&table[3]);
	sys_icache_invalidate((void *)page, vm_page_size);
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) {
		say("redwood: WidgetKit's watchdog code: cannot make the page executable again\n");
		return;
	}
	say("redwood: getTimelines gets %dx its time as well\n", WATCHDOG_SCALE);
}

__attribute__((constructor))
static void
redwood_chrono_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	uintptr_t slide;

	if (name == NULL || strcmp(name, "chronod") != 0) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	longer_watchdogs(slide);
	longer_counted_watchdogs(slide);
}
