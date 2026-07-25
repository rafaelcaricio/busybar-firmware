#pragma once

#include <stdint.h>

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0
#define configUSE_MALLOC_FAILED_HOOK            1

#define configTICK_RATE_HZ_RAW 1000
#define configTICK_RATE_HZ     ((TickType_t)configTICK_RATE_HZ_RAW)

#define configMAX_PRIORITIES         (32)
#define configMINIMAL_STACK_SIZE     ((uint16_t)PTHREAD_STACK_MIN)
#define configMAX_TASK_NAME_LEN      (32)
#define configUSE_16_BIT_TICKS       0
#define configIDLE_SHOULD_YIELD      1
#define configSTACK_DEPTH_TYPE       uint32_t
#define configRUN_TIME_COUNTER_TYPE  uint32_t

#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    0
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   3
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 2

#define configSUPPORT_STATIC_ALLOCATION  1
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configTOTAL_HEAP_SIZE            ((size_t)(8 * 1024 * 1024))
#define configAPPLICATION_ALLOCATED_HEAP 0

#define configUSE_TIMERS             1
#define configTIMER_TASK_PRIORITY    (configMAX_PRIORITIES - 2)
#define configTIMER_QUEUE_LENGTH     32
#define configTIMER_TASK_STACK_DEPTH (8 * 1024)

#define configIDLE_TASK_STACK_DEPTH (8 * 1024)

#define configUSE_TRACE_FACILITY            1
#define configUSE_STATS_FORMATTING_FUNCTIONS 0
#define configGENERATE_RUN_TIME_STATS       0

#define configCHECK_FOR_STACK_OVERFLOW 0
#define configRECORD_STACK_HIGH_ADDRESS 1

#define INCLUDE_vTaskPrioritySet            1
#define INCLUDE_uxTaskPriorityGet           1
#define INCLUDE_vTaskDelete                 1
#define INCLUDE_vTaskSuspend                1
#define INCLUDE_xTaskDelayUntil             1
#define INCLUDE_vTaskDelay                  1
#define INCLUDE_xTaskGetSchedulerState      1
#define INCLUDE_xTaskGetCurrentTaskHandle   1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetIdleTaskHandle      1
#define INCLUDE_eTaskGetState               1
#define INCLUDE_xTimerPendFunctionCall      1
#define INCLUDE_xTaskAbortDelay             1
#define INCLUDE_xQueueGetMutexHolder        1
#define INCLUDE_xSemaphoreGetMutexHolder    1
#define INCLUDE_xTaskGetHandle              1
#define INCLUDE_xTaskResumeFromISR          1

extern void vAssertCalled(const char* file, unsigned long line);
#define configASSERT(x) \
    if((x) == 0) vAssertCalled(__FILE__, __LINE__)

#include <limits.h>
#include <pthread.h>
#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 16384
#endif
