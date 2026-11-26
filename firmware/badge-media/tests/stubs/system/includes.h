#ifndef BADGE_TEST_SYSTEM_INCLUDES_H
#define BADGE_TEST_SYSTEM_INCLUDES_H

#include <semaphore.h>
#include <pthread.h>

extern pthread_mutex_t badge_test_critical;
#define OS_ENTER_CRITICAL() pthread_mutex_lock(&badge_test_critical)
#define OS_EXIT_CRITICAL() pthread_mutex_unlock(&badge_test_critical)

typedef sem_t OS_SEM;

static inline int os_sem_create(OS_SEM *sem, int count) { return sem_init(sem, 0, count); }
static inline int os_sem_del(OS_SEM *sem, int unused) { (void)unused; return sem_destroy(sem); }
static inline int os_sem_pend(OS_SEM *sem, int unused) { (void)unused; return sem_wait(sem); }
static inline int os_sem_post(OS_SEM *sem) { return sem_post(sem); }

int task_create(void (*task)(void *), void *arg, const char *name);

#endif
