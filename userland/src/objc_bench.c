/*
 * The kind of work an iOS interface does on the processor, timed: Objective-C messages, CoreFoundation
 * collections, string formatting and sorting through callbacks -- calls and returns through the shared
 * cache all the time, unlike cpu_test's arithmetic. For measuring the emulator: the same work every run,
 * one thread, "objc_bench: N ms" (best of the rounds) on the console.
 *
 * usage: objc_bench [SECONDS [ROUNDS]]   wait SECONDS first (for a quiet system), then ROUNDS rounds (3)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static uint64_t
milliseconds(void)
{
	static mach_timebase_info_data_t timebase;

	if (timebase.denom == 0) {
		mach_timebase_info(&timebase);
	}
	return mach_absolute_time() * timebase.numer / timebase.denom / 1000000;
}

static CFComparisonResult
by_value(const void *a, const void *b, void *context)
{
	(void)context;
	return CFStringCompare((CFStringRef)a, (CFStringRef)b, kCFCompareNumerically);
}

static uint64_t
round_once(void)
{
	id (*send)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id (*send_id)(id, SEL, id) = (id (*)(id, SEL, id))objc_msgSend;
	id (*send_id_id)(id, SEL, id, id) = (id (*)(id, SEL, id, id))objc_msgSend;
	Class dictionary_class = objc_getClass("NSMutableDictionary");
	SEL alloc = sel_registerName("alloc"), init = sel_registerName("init"), release = sel_registerName("release");
	SEL set = sel_registerName("setObject:forKey:"), get = sel_registerName("objectForKey:");
	SEL count = sel_registerName("count");
	uint64_t began = milliseconds(), checksum = 0;

	for (int outer = 0; outer < 40; outer++) {
		CFMutableArrayRef strings = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
		id dictionary = send(send((id)dictionary_class, alloc), init);

		for (int i = 0; i < 500; i++) {
			CFStringRef key = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d"), (i * 7919 + outer) % 1000);
			CFStringRef value = CFStringCreateWithFormat(NULL, NULL, CFSTR("value %d of %d"), i, outer);

			send_id_id(dictionary, set, (id)value, (id)key);
			CFArrayAppendValue(strings, key);
			CFRelease(key);
			CFRelease(value);
		}
		CFArraySortValues(strings, CFRangeMake(0, CFArrayGetCount(strings)), by_value, NULL);
		for (CFIndex i = 0; i < CFArrayGetCount(strings); i++) {
			checksum += send_id(dictionary, get, (id)CFArrayGetValueAtIndex(strings, i)) != NULL;
		}
		checksum += (uint64_t)send(dictionary, count);
		send(dictionary, release);
		CFRelease(strings);
	}
	if (checksum == 0) {
		printf("objc_bench: nothing was found\n");
	}
	return milliseconds() - began;
}

int
main(int argc, char **argv)
{
	int wait = argc > 1 ? atoi(argv[1]) : 0, rounds = argc > 2 ? atoi(argv[2]) : 3;
	uint64_t best = UINT64_MAX;

	sleep((unsigned)wait);
	for (int i = 0; i < rounds; i++) {
		uint64_t took = round_once();

		printf("objc_bench: round %d, %llu ms\n", i + 1, took);
		best = took < best ? took : best;
	}
	printf("objc_bench: %llu ms\n", best);
	return 0;
}
