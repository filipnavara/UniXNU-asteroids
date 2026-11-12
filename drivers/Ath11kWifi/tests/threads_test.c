/* The runtime's optional thread model (lx_platform.module_lock and friends) with real pthreads: the main thread is the board
 * worker (dispatcher, inside lx_enter/lx_leave), module threads come from lx_thread_start. Checks the patterns qcacld needs:
 * a dispatcher wait completed by a module thread that waits for a message (HDD -> MC thread), a module thread flushing work
 * only the dispatcher runs, mutex contention across threads (both directions), mutual exclusion of module code, work queued
 * from a thread while the board worker sleeps outside the module, and thread exit. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "lx_plat.h"

static pthread_mutex_t g_module = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static u64 now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
static u64 now_ms(void) { return now_ns() / 1000000; }
static void sleep_ms(unsigned ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 }; nanosleep(&t, NULL); }
static void poll(void) {}
static void log_line(int level, const char *line) { (void)level; fprintf(stderr, "%s\n", line); }
static void halt(void) { abort(); }
static void *thread_self(void) { return (void *)pthread_self(); }
struct start { void (*fn)(void *); void *arg; };
static void *start_main(void *p) { struct start s = *(struct start *)p; free(p); s.fn(s.arg); return NULL; }
static int thread_start(void (*fn)(void *), void *arg) {
  struct start *s = malloc(sizeof *s); s->fn = fn; s->arg = arg;
  pthread_t t; if (pthread_create(&t, NULL, start_main, s)) { free(s); return -1; }
  pthread_detach(t); return 0;
}
static void thread_exit(void) { pthread_exit(NULL); }
static void module_lock(void) { pthread_mutex_lock(&g_module); }
static void module_unlock(void) { pthread_mutex_unlock(&g_module); }
static void module_wait(unsigned ms) {
  struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
  t.tv_nsec += (long)ms * 1000000; t.tv_sec += t.tv_nsec / 1000000000; t.tv_nsec %= 1000000000;
  pthread_cond_timedwait(&g_cond, &g_module, &t);   /* releases the module lock while it sleeps */
}
static unsigned wakes;
static void module_wake(void) { __atomic_fetch_add(&wakes, 1, __ATOMIC_RELAXED); pthread_cond_broadcast(&g_cond); }
static const struct lx_platform platform = {
  .now_ms = now_ms, .sleep_ms = sleep_ms, .poll = poll, .log = log_line, .halt = halt, .alloc = malloc, .free = free,
  .now_ns = now_ns,
  .thread_self = thread_self, .thread_start = thread_start, .thread_exit = thread_exit,
  .module_lock = module_lock, .module_unlock = module_unlock, .module_wait = module_wait, .module_wake = module_wake,
};

/* The board worker's loop: run the dispatcher inside the module, then sleep outside it. */
static void board_round(unsigned sleep) { lx_enter(); while (lx_pump()) { } lx_leave(); if (sleep) sleep_ms(sleep); }

/* 1: HDD posts a message to the MC thread and waits for its completion on the dispatcher. */
static int message, mc_stop; static struct completion reply; static unsigned mc_handled;
static void mc_thread(void *arg) {
  (void)arg;
  for (;;) {
    wait_event(0, READ_ONCE(message) || READ_ONCE(mc_stop));
    if (READ_ONCE(mc_stop) && !message) break;
    assert(!lx_on_dispatcher());
    assert(lx_pump() == 0);                 /* a module thread never runs dispatcher work */
    message = 0; mc_handled++; complete(&reply);
  }
}
/* 2: a module thread queues work and flushes it; only the dispatcher runs it. */
static struct work_struct flushed_work; static int work_ran_on_dispatcher, flusher_done;
static void flushed_fn(struct work_struct *w) { (void)w; work_ran_on_dispatcher = lx_on_dispatcher(); }
static void flusher(void *arg) { (void)arg; lx_queue_work(&flushed_work); lx_flush_work(&flushed_work); __atomic_store_n(&flusher_done, 1, __ATOMIC_RELEASE); }
/* 3: mutex contention in both directions. */
static struct mutex shared; static struct completion holding, release_it; static int holder_done;
static void holder(void *arg) {
  (void)arg; mutex_lock(&shared); complete(&holding);
  wait_for_completion(&release_it);         /* idles with the mutex held: the dispatcher must wait for it, not BUG */
  mutex_unlock(&shared); holder_done = 1;
}
static int waiter_got; static struct completion waiter_done;
static void waiter(void *arg) { (void)arg; mutex_lock(&shared); waiter_got = 1; mutex_unlock(&shared); complete(&waiter_done); }
/* 4: mutual exclusion of module code (non-atomic increments with waits in between). */
enum { WORKERS = 4, ROUNDS = 400 };
static unsigned counter; static struct completion counted;
static void incrementer(void *arg) {
  (void)arg;
  for (int i = 0; i < ROUNDS; i++) { unsigned v = counter; for (volatile int k = 0; k < 50; k++) { } counter = v + 1; if (i % 50 == 0) lx_idle(1); }
  complete(&counted);
}
/* 5: thread exit from inside (kthread_complete_and_exit). */
static struct completion exited;
static void exiter(void *arg) { (void)arg; complete(&exited); lx_thread_exit(); }

int main(void) {
  lx_plat = &platform;
  lx_enter();
  assert(lx_on_dispatcher());

  init_completion(&reply);
  assert(!lx_thread_start(mc_thread, NULL));
  for (int i = 0; i < 50; i++) {
    WRITE_ONCE(message, 1);
    wake_up(0);
    assert(wait_for_completion_timeout(&reply, 2000));
  }
  assert(mc_handled == 50);
  WRITE_ONCE(mc_stop, 1); wake_up(0);

  INIT_WORK(&flushed_work, flushed_fn);
  assert(!lx_thread_start(flusher, NULL));
  lx_leave();
  /* the worker mostly sleeps outside the module; it reads the flag outside the lock, hence atomically */
  for (int i = 0; i < 2000 && !__atomic_load_n(&flusher_done, __ATOMIC_ACQUIRE); i++) board_round(1);
  lx_enter();
  assert(flusher_done && work_ran_on_dispatcher);

  mutex_init(&shared); init_completion(&holding); init_completion(&release_it); init_completion(&waiter_done);
  assert(!lx_thread_start(holder, NULL));
  wait_for_completion(&holding);
  complete(&release_it);
  mutex_lock(&shared);                       /* contended: waits until the holder thread releases it */
  assert(holder_done);
  assert(!lx_thread_start(waiter, NULL));
  lx_delay_ms(20);
  assert(!waiter_got);                       /* the dispatcher holds it now: the thread waits */
  mutex_unlock(&shared);
  wait_for_completion(&waiter_done);
  assert(waiter_got);

  init_completion(&counted);
  for (int i = 0; i < WORKERS; i++) assert(!lx_thread_start(incrementer, NULL));
  for (int i = 0; i < ROUNDS; i++) { unsigned v = counter; counter = v + 1; if (i % 50 == 0) lx_idle(1); }
  for (int i = 0; i < WORKERS; i++) wait_for_completion(&counted);
  assert(counter == (WORKERS + 1) * ROUNDS);

  init_completion(&exited);
  assert(!lx_thread_start(exiter, NULL));
  wait_for_completion(&exited);
  lx_leave();
  printf("PASS: dispatcher/MC-thread messages (50), thread flush of dispatcher work, mutex contention both ways, "
         "mutual exclusion of %u increments over %d threads, thread exit; %u wakes\n", counter, WORKERS + 1, wakes);
  return 0;
}
