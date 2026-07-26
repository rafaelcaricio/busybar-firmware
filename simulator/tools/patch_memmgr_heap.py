#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Protect furi's heap with a real mutex in the POSIX simulator.

The firmware allocator uses vTaskSuspendAll() as its critical section. That is
correct on the single-core target, but the POSIX port briefly has two pthreads
running during task handoff and the SDL main thread is outside FreeRTOS
entirely. The free-list can therefore be mutated concurrently while still
reporting hundreds of megabytes free.

The host copy uses a pthread mutex instead. A thread-local depth makes it
recursive without a dynamically initialized recursive-mutex attribute:
memmgr_heap_printf_free_blocks() allocates its own snapshot while holding the
heap guard.
"""

import argparse
import sys
from pathlib import Path

INCLUDE_ANCHOR = "#include <stdint.h>\n"
GLOBAL_ANCHOR = "static size_t xBlockAllocatedBit = 0;\n"
LOCK_IMPLEMENTATION = """

/* Host build: FreeRTOS scheduler suspension is not cross-pthread exclusion. */
static pthread_mutex_t xHeapMutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned xHeapMutexDepth;
static _Thread_local sigset_t xHeapPreviousSignalMask;

static void memmgr_heap_host_lock(void) {
    if(xHeapMutexDepth++ == 0) {
        sigset_t tickSignal;
        sigemptyset(&tickSignal);
        sigaddset(&tickSignal, SIGALRM);

        int result = pthread_sigmask(SIG_BLOCK, &tickSignal, &xHeapPreviousSignalMask);
        furi_check(result == 0);

        result = pthread_mutex_lock(&xHeapMutex);
        if(result != 0) {
            (void)pthread_sigmask(SIG_SETMASK, &xHeapPreviousSignalMask, NULL);
            furi_crash("Host heap mutex lock failed");
        }
    }
}

static void memmgr_heap_host_unlock(void) {
    furi_check(xHeapMutexDepth > 0);
    if(--xHeapMutexDepth == 0) {
        const int unlockResult = pthread_mutex_unlock(&xHeapMutex);
        const int maskResult =
            pthread_sigmask(SIG_SETMASK, &xHeapPreviousSignalMask, NULL);
        furi_check(unlockResult == 0);
        furi_check(maskResult == 0);
    }
}
"""


def fail(path: Path, message: str) -> int:
    print(
        f"{path}: {message}; memmgr_heap.c has drifted, re-check "
        "simulator/tools/patch_memmgr_heap.py",
        file=sys.stderr,
    )
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source = args.input.read_text()
    if source.count(INCLUDE_ANCHOR) != 1 or source.count(GLOBAL_ANCHOR) != 1:
        return fail(args.input, "could not locate include/global anchors")

    suspend_count = source.count("vTaskSuspendAll();")
    resume_void_count = source.count("(void)xTaskResumeAll();")
    resume_plain_count = source.count("xTaskResumeAll();") - resume_void_count
    if suspend_count != 8 or resume_void_count != 6 or resume_plain_count != 2:
        return fail(
            args.input,
            "expected 8 scheduler guard pairs "
            f"(found {suspend_count} suspend, {resume_void_count} void resume, "
            f"{resume_plain_count} plain resume)",
        )

    patched = source.replace(
        INCLUDE_ANCHOR,
        INCLUDE_ANCHOR + "#include <pthread.h>\n#include <signal.h>\n",
    )
    patched = patched.replace(GLOBAL_ANCHOR, GLOBAL_ANCHOR + LOCK_IMPLEMENTATION)
    patched = patched.replace("vTaskSuspendAll();", "memmgr_heap_host_lock();")
    patched = patched.replace("(void)xTaskResumeAll();", "memmgr_heap_host_unlock();")
    patched = patched.replace("xTaskResumeAll();", "memmgr_heap_host_unlock();")

    init_body = """        {
            prvHeapInit();
            memmgr_heap_init_trace();
        }"""
    init_body_guarded = """        {
            /* Another host thread may have initialized it while this one was
             * waiting for the heap mutex. */
            if(pxEnd == NULL) {
                prvHeapInit();
                memmgr_heap_init_trace();
            }
        }"""
    if patched.count(init_body) != 1:
        return fail(args.input, "could not locate the lazy heap initializer")
    patched = patched.replace(init_body, init_body_guarded)

    if "vTaskSuspendAll" in patched or "xTaskResumeAll" in patched:
        return fail(args.input, "a scheduler-based heap guard survived")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(patched)
    print(f"patched {args.input.name} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
