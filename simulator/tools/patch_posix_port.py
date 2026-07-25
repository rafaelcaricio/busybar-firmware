#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Make the FreeRTOS POSIX port's tick thread-directed instead of process-directed.

The vendored kernel is V10.5.1 (Nov 2022), whose POSIX port drives the tick
with setitimer(ITIMER_REAL). That raises SIGALRM at the *process*, so the
kernel delivers it to any thread that has not blocked it -- including threads
the simulator does not own. On macOS, AppKit's _NSEventThread (created by
SDL_Init) is such a thread. When it caught the tick it ran
vPortSystemTickHandler on a thread that is not a FreeRTOS task: it called
vTaskSwitchContext(), then suspended itself on the condvar belonging to
whichever task happened to be current. Nothing ever resumes it, and the task
it impersonated never runs again -- the whole scheduler wedges. Observed as a
hard hang with every FreeRTOS thread parked in prvSuspendSelf(), and it also
explains the sporadic taskSELECT_HIGHEST_PRIORITY_TASK and
xTaskPriorityDisinherit asserts, which are what a corrupted pxCurrentTCB looks
like from inside the kernel.

Upstream fixed this by replacing the interval timer with a dedicated tick
thread that aims the signal at exactly one thread:

    pthread_kill( prvGetThreadFromTask( xTaskGetCurrentTaskHandle() )->pthread,
                  SIGALRM );

That is what this patch backports. A foreign thread can then no longer receive
the tick, whatever its signal mask happens to be.

Upstream reference (FreeRTOS-Kernel, main):
  portable/ThirdParty/GCC/Posix/port.c, prvTimerTickHandler()
"""

import argparse
import sys
from pathlib import Path

# Anchored on the definitions; both also appear as forward declarations.
SETUP_START = "\nvoid prvSetupTimerInterrupt( void )\n{"
SETUP_END = "\nstatic void vPortSystemTickHandler( int sig )\n{"

# vPortEndScheduler stops the tick thread and is defined before it, so the
# state has to go up with the other file-scope globals.
GLOBALS_ANCHOR = "static volatile portBASE_TYPE uxCriticalNesting;\n"

GLOBALS_ADDITION = """
/* Host build: the tick is raised by a dedicated thread with pthread_kill()
 * rather than by an interval timer, so it can only ever land on the thread
 * running the current FreeRTOS task. See simulator/tools/patch_posix_port.py. */
static pthread_t hTimerTickThread;
static volatile BaseType_t xTimerTickThreadShouldRun = pdFALSE;

/* How far the tick may fall behind before simulated time is resynchronised to
 * the host clock, losing the ticks in between. */
#define portTICK_RESYNC_THRESHOLD_MS    250L
"""

SETUP_REPLACEMENT = """
/* Sleep until an absolute deadline. Upstream's tick thread does a plain
 * usleep( period ), which makes the real period period + the cost of raising
 * and handling the tick — around 30% slow here, so every animation and
 * timeout in the UI ran slow by that much. Deadlines keep simulated time on
 * wall time. macOS has no clock_nanosleep(), hence the subtraction. */
static void prvSleepUntil( const struct timespec * pxDeadline )
{
    struct timespec xNow, xSleep;

    clock_gettime( CLOCK_MONOTONIC, &xNow );

    xSleep.tv_sec = pxDeadline->tv_sec - xNow.tv_sec;
    xSleep.tv_nsec = pxDeadline->tv_nsec - xNow.tv_nsec;

    if( xSleep.tv_nsec < 0 )
    {
        xSleep.tv_nsec += 1000000000L;
        xSleep.tv_sec -= 1;
    }

    if( xSleep.tv_sec >= 0 )
    {
        while( ( nanosleep( &xSleep, &xSleep ) == -1 ) && ( errno == EINTR ) )
        {
        }
    }
}

static void * prvTimerTickHandler( void * arg )
{
    struct timespec xDeadline;

    ( void ) arg;

    clock_gettime( CLOCK_MONOTONIC, &xDeadline );

    while( xTimerTickThreadShouldRun != pdFALSE )
    {
        Thread_t * pxThread = prvGetThreadFromTask( xTaskGetCurrentTaskHandle() );

        ( void ) pthread_kill( pxThread->pthread, SIGALRM );

        xDeadline.tv_nsec += portTICK_RATE_MICROSECONDS * 1000L;

        if( xDeadline.tv_nsec >= 1000000000L )
        {
            xDeadline.tv_nsec -= 1000000000L;
            xDeadline.tv_sec += 1;
        }

        prvSleepUntil( &xDeadline );

        /* Each wake-up overshoots the deadline slightly -- a 1 ms nanosleep()
         * measures 1.25 ms here -- so the next sleep has to be shortened by
         * the overshoot for the tick rate to come out right. That happens by
         * itself as long as the deadline keeps advancing by exactly one
         * period. Only give up and resynchronise once the backlog is large
         * enough that it cannot be recovered a fraction of a millisecond at a
         * time, e.g. after the host suspends: replaying seconds of ticks
         * back-to-back would be worse than dropping them. */
        {
            struct timespec xNow;
            long lBehindMs;

            clock_gettime( CLOCK_MONOTONIC, &xNow );

            lBehindMs = ( long ) ( ( xNow.tv_sec - xDeadline.tv_sec ) * 1000L ) +
                        ( ( xNow.tv_nsec - xDeadline.tv_nsec ) / 1000000L );

            if( lBehindMs > portTICK_RESYNC_THRESHOLD_MS )
            {
                xDeadline = xNow;
            }
        }
    }

    return NULL;
}

void prvSetupTimerInterrupt( void )
{
    sigset_t xBlockAll, xPrevious;

    /* The tick thread must never handle the tick itself. */
    ( void ) sigfillset( &xBlockAll );
    ( void ) pthread_sigmask( SIG_SETMASK, &xBlockAll, &xPrevious );

    xTimerTickThreadShouldRun = pdTRUE;

    if( pthread_create( &hTimerTickThread, NULL, prvTimerTickHandler, NULL ) != 0 )
    {
        prvFatalError( "pthread_create", errno );
    }

    ( void ) pthread_sigmask( SIG_SETMASK, &xPrevious, NULL );

    prvStartTimeNs = prvGetTimeNs();
}
/*-----------------------------------------------------------*/

"""

# vPortEndScheduler tears the interval timer down; it has to stop the thread.
END_START = "    /* Stop the timer and ignore any pending SIGALRMs that would end"
END_STOP = "    sigtick.sa_flags = 0;"

END_REPLACEMENT = """    /* Stop the tick thread and ignore any pending SIGALRMs that would end
     * up running on the main thread when it is resumed. */
    xTimerTickThreadShouldRun = pdFALSE;
    ( void ) pthread_join( hTimerTickThread, NULL );

"""


def fail(path: Path, message: str) -> int:
    print(
        f"{path}: {message}; the POSIX port has drifted, re-check "
        "simulator/tools/patch_posix_port.py against it",
        file=sys.stderr,
    )
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source = args.input.read_text()

    if "prvTimerTickHandler" in source:
        return fail(args.input, "already has a tick thread, so this patch is obsolete")

    setup_start = source.find(SETUP_START)
    setup_end = source.find(SETUP_END)

    if setup_start < 0 or setup_end < 0 or setup_end <= setup_start:
        return fail(args.input, "could not locate prvSetupTimerInterrupt")

    setup = source[setup_start:setup_end]
    if "setitimer" not in setup or "ITIMER_REAL" not in setup:
        return fail(args.input, "prvSetupTimerInterrupt no longer uses setitimer")

    patched = source[:setup_start] + SETUP_REPLACEMENT + source[setup_end:]

    if patched.count(GLOBALS_ANCHOR) != 1:
        return fail(args.input, "could not locate the file-scope globals")
    patched = patched.replace(GLOBALS_ANCHOR, GLOBALS_ANCHOR + GLOBALS_ADDITION)

    end_start = patched.find(END_START)
    end_stop = patched.find(END_STOP, end_start if end_start >= 0 else 0)

    if end_start < 0 or end_stop < 0 or end_stop <= end_start:
        return fail(args.input, "could not locate the teardown in vPortEndScheduler")

    if "setitimer" not in patched[end_start:end_stop]:
        return fail(args.input, "vPortEndScheduler no longer stops an interval timer")

    patched = patched[:end_start] + END_REPLACEMENT + patched[end_stop:]

    # nanosleep() comes from the existing <time.h>; errno is used unguarded by
    # the original but never included there.
    if "errno.h" not in patched:
        patched = patched.replace("#include <time.h>", "#include <errno.h>\n#include <time.h>", 1)

    # vPortEndScheduler declared the itimerval the removed code used.
    stale_declaration = "    struct itimerval itimer;\n"
    if patched.count(stale_declaration) != 1:
        return fail(args.input, "expected exactly one orphaned itimerval declaration")
    patched = patched.replace(stale_declaration, "")

    if "setitimer( ITIMER_REAL" in patched:
        return fail(args.input, "an interval timer survived the patch")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(patched)

    print(f"patched {args.input.name} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
