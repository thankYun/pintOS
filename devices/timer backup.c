#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip.
8254 타이머 칩의 하드웨어 세부 정보에 대한 내용은 [8254]를 참조 */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted.
OS 부팅 이후의 타이머 틱 수입니다. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate().
    타이머 틱당 루프 수입니다.
   timer_calibrate()에서 초기화됩니다.
    */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt.
   8254 Programmable Interval Timer (PIT)를 설정하여
   초당 PIT_FREQ번 인터럽트를 발생시키고 해당 인터럽트를 등록합니다. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to nearest.
	TIMER_FREQ로 나눈 8254 입력 주파수를 반올림합니다. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays.
루프당 loops_per_tick 값을 기반으로 지연을 구현하기 위해 loops_per_tick 값을 보정합니다. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two still less than one timer tick.
	   타이머 틱보다 작은 가장 큰 2의 거듭제곱으로 loops_per_tick 값을 근사화합니다. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick.
	loops_per_tick의 다음 8비트를 보정합니다. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted.
OS 부팅 이후의 타이머 틱 수를 반환합니다.  */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which should be a value once returned by timer_ticks(). 
이전에 timer_ticks()로 반환된 값인 THEN 이후로 경과한 타이머 틱 수를 반환합니다.*/
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* Suspends execution for approximately TICKS timer ticks. 
대략적으로 TICKS 타이머 틱 동안 실행을 중지합니다.*/
void
timer_sleep (int64_t ticks) {
	int64_t start = timer_ticks ();

	ASSERT (intr_get_level () == INTR_ON);
	while (timer_elapsed (start) < ticks)
		thread_yield ();
}

/* Suspends execution for approximately MS milliseconds.
대략적으로 MS 밀리초 동안 실행을 중지합니다. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds.
대략적으로 US 마이크로초 동안 실행을 중지합니다. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds.
대략적으로 NS 나노초 동안 실행을 중지합니다. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics.
타이머 통계를 출력합니다. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler.
타이머 인터럽트 핸들러 */
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_tick ();
}

/* Returns true if LOOPS iterations waits for more than one timer tick, otherwise false.
   만약 LOOPS 반복이 하나의 타이머 틱 이상 기다린다면 true를 반환하고, 그렇지 않으면 false를 반환합니다. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick.
	타이머 틱을 기다립니다. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops.
	LOOPS 반복 실행합니다. */
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long.
	타이머 틱 수가 변경되었다면 너무 오랫동안 반복했습니다. */
	barrier ();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. 
   간단한 루프를 LOOPS 횟수만큼 반복하여 짧은 지연을 구현합니다.
   코드 정렬은 타이밍에 큰 영향을 미치므로 NO_INLINE으로 표시되어
   이 함수가 서로 다른 위치에서 다르게 인라인되면 결과가 예측하기 어려워집니다. */

static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds.
NUM/DENOM 초 동안 대기합니다. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* NUM/DENOM 초를 타이머 틱으로 변환하고 내림합니다.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. 
		   적어도 하나의 완전한 타이머 틱을 기다리고 있습니다. 다른 프로세스에 CPU를 양보하기 위해
	   timer_sleep()를 사용합니다.*/
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow.
		   그렇지 않으면 더 정확한 서브 틱 타이밍을 위해 바쁜 대기 루프를 사용합니다.
	   분자와 분모를 1000으로 나누어 오버플로우 가능성을 피합니다. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
