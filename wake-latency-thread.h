/*
 * schbench.c =》wake-lantency-thread
 * use porcess instead of thread
 *
 * Copyright (C) 2016 Facebook
 * Chris Mason <clm@fb.com>
 *
 * GPLv2, portions copied from the kernel and from Jens Axboe's fio
 *
 * gcc -Wall -O0 -W schbench.c -o schbench -lpthread
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/futex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PLAT_BITS 8
#define PLAT_VAL (1 << PLAT_BITS)
#define PLAT_GROUP_NR 19
#define PLAT_NR (PLAT_GROUP_NR * PLAT_VAL)
#define PLAT_LIST_MAX 20

/* when -p is on, how much do we send back and forth */
#define PIPE_TRANSFER_BUFFER (1 * 1024 * 1024)

#define USEC_PER_SEC (1000000)

/* -m number of message threads */
static int message_threads = 2;
/* -t  number of workers per message thread */
static int worker_threads = 16;
/* -r  seconds */
static int runtime = 30;  // 总的运行时间，决定统计的次数
/* -s  usec */
static int sleeptime = 10000;  // messager 每次执行的睡眠时间
/* -C  usec */
static int message_cputime = 30000;  // messager 每次执行的运行时间
/* -w  seconds */
static int warmuptime = 5;
/* -i  seconds */
static int intervaltime = 10;  // 定时统计输出的间隔时间
/* -z  seconds */
static int zerotime = 0;
/* -c  usec */
static unsigned long long cputime = 15000;  // worker 每次执行的运行时间
/* -a, bool */
static int autobench = 0;
/* -p bytes */
static int pipe_test = 0;
/* -R requests per sec */
static int requests_per_sec = 0;

/*
 * one stat struct per thread data, when the workers sleep this records the
 * latency between when they are woken up and when they actually get the
 * CPU again.  The message threads sum up the stats of all the workers and
 * then bubble them up to main() for printing
 * 该工具记录的调度延迟为，一个线程进入睡眠后，从其被唤醒到获得CPU运行的时间
 * 切计算延迟靠的是worker线程的循环
 * 睡眠-等待-被唤醒，工具提供可配置项cputime觉得了
 */
struct stats {
  unsigned int plat[PLAT_NR];
  unsigned long nr_samples;
  unsigned int max;
  unsigned int min;
};

/* this defines which latency profiles get printed */
#define PLIST_P99 4
#define PLIST_P95 3
static double plist[PLAT_LIST_MAX] = {50.0, 75.0, 90.0, 95.0, 99.0, 99.5, 99.9};

enum {
  HELP_LONG_OPT = 1,
};
char* option_string = "p:am:t:s:c:C:r:R:w:i:z:";
static struct option long_options[] = {
    {"auto", no_argument, 0, 'a'},
    {"pipe", required_argument, 0, 'p'},
    {"message-threads", required_argument, 0, 'm'},
    {"threads", required_argument, 0, 't'},
    {"runtime", required_argument, 0, 'r'},
    {"rps", required_argument, 0, 'R'},
    {"sleeptime", required_argument, 0, 's'},
    {"message_cputime", required_argument, 0, 's'},
    {"cputime", required_argument, 0, 'c'},
    {"warmuptime", required_argument, 0, 'w'},
    {"intervaltime", required_argument, 0, 'i'},
    {"zerotime", required_argument, 0, 'z'},
    {"help", no_argument, 0, HELP_LONG_OPT},
    {0, 0, 0, 0}};

static void print_usage(void) {
  fprintf(
      stderr,
      "schbench usage:\n"
      "\t-m (--message-threads): number of message threads (def: 2)\n"
      "\t-t (--threads): worker threads per message thread (def: 16)\n"
      "\t-r (--runtime): How long to run before exiting (seconds, def: 30)\n"
      "\t-s (--sleeptime): Message thread latency (usec, def: 30000\n"
      "\t-C (--message_cputime): Message thread think time (usec, def: 30000\n"
      "\t-c (--cputime): How long to think during loop (usec, def: 30000\n"
      "\t-a (--auto): grow thread count until latencies hurt (def: off)\n"
      "\t-p (--pipe): transfer size bytes to simulate a pipe test (def: 0)\n"
      "\t-R (--rps): requests per second mode (count, def: 0)\n"
      "\t-w (--warmuptime): how long to warmup before resettings stats "
      "(seconds, def: 5)\n"
      "\t-i (--intervaltime): interval for printing latencies (seconds, def: "
      "10)\n"
      "\t-z (--zerotime): interval for zeroing latencies (seconds, def: "
      "never)\n");
  exit(1);
}

static void parse_options(int ac, char** av) {
  int c;
  int found_sleeptime = -1;
  int found_cputime = -1;
  int found_warmuptime = -1;
  int found_message_cputime = -1;

  while (1) {
    int option_index = 0;

    c = getopt_long(ac, av, option_string, long_options, &option_index);

    if (c == -1)
      break;

    switch (c) {
      case 'a':
        autobench = 1;
        warmuptime = 0;
        break;
      case 'p':
        pipe_test = atoi(optarg);
        if (pipe_test > PIPE_TRANSFER_BUFFER) {
          fprintf(stderr, "pipe size too big, using %d\n",
                  PIPE_TRANSFER_BUFFER);
          pipe_test = PIPE_TRANSFER_BUFFER;
        }
        sleeptime = 0;
        cputime = 0;
        warmuptime = 0;
        message_cputime = 0;
        break;
      case 's':
        found_sleeptime = atoi(optarg);
        break;
      case 'c':
        found_cputime = atoi(optarg);
        break;
      case 'C':
        found_message_cputime = atoi(optarg);
        break;
      case 'w':
        found_warmuptime = atoi(optarg);
        break;
      case 'm':
        message_threads = atoi(optarg);
        break;
      case 't':
        worker_threads = atoi(optarg);
        break;
      case 'r':
        runtime = atoi(optarg);
        break;
      case 'i':
        intervaltime = atoi(optarg);
        break;
      case 'z':
        zerotime = atoi(optarg);
        break;
      case 'R':
        requests_per_sec = atoi(optarg);
        break;
      case '?':
      case HELP_LONG_OPT:
        print_usage();
        break;
      default:
        break;
    }
  }

  /*
   * by default pipe mode zeros out cputime and sleep time.  This
   * sets them to any args that were actually passed in
   */
  if (found_sleeptime >= 0)
    sleeptime = found_sleeptime;
  if (found_cputime >= 0)
    cputime = found_cputime;
  if (found_warmuptime >= 0)
    warmuptime = found_warmuptime;
  if (found_message_cputime >= 0)
    message_cputime = message_cputime;

  if (optind < ac) {
    fprintf(stderr, "Error Extra arguments '%s'\n", av[optind]);
    exit(1);
  }
}

// 计算时间差值
void tvsub(struct timeval* tdiff, struct timeval* t1, struct timeval* t0) {
  tdiff->tv_sec = t1->tv_sec - t0->tv_sec;
  tdiff->tv_usec = t1->tv_usec - t0->tv_usec;
  if (tdiff->tv_usec < 0 && tdiff->tv_sec > 0) {
    tdiff->tv_sec--;
    tdiff->tv_usec += USEC_PER_SEC;
    if (tdiff->tv_usec < 0) {
      fprintf(stderr, "lat_fs: tvsub shows test time ran backwards!\n");
      exit(1);
    }
  }

  /* time shouldn't go backwards!!! */
  if (tdiff->tv_usec < 0 || t1->tv_sec < t0->tv_sec) {
    tdiff->tv_sec = 0;
    tdiff->tv_usec = 0;
  }
}

/*
 * returns the difference between start and stop in usecs.  Negative values
 * are turned into 0
 */
unsigned long long tvdelta(struct timeval* start, struct timeval* stop) {
  struct timeval td;
  unsigned long long usecs;

  tvsub(&td, stop, start);
  usecs = td.tv_sec;
  usecs *= USEC_PER_SEC;
  usecs += td.tv_usec;
  return (usecs);
}

/* mr axboe's magic latency histogram */
static unsigned int plat_val_to_idx(unsigned int val) {
  unsigned int msb, error_bits, base, offset;

  /* Find MSB starting from bit 0 */
  if (val == 0)
    msb = 0;
  else
    msb = sizeof(val) * 8 - __builtin_clz(val) - 1;

  /*
   * MSB <= (PLAT_BITS-1), cannot be rounded off. Use
   * all bits of the sample as index
   */
  if (msb <= PLAT_BITS)
    return val;

  /* Compute the number of error bits to discard*/
  error_bits = msb - PLAT_BITS;

  /* Compute the number of buckets before the group */
  base = (error_bits + 1) << PLAT_BITS;

  /*
   * Discard the error bits and apply the mask to find the
   * index for the buckets in the group
   */
  offset = (PLAT_VAL - 1) & (val >> error_bits);

  /* Make sure the index does not exceed (array size - 1) */
  return (base + offset) < (PLAT_NR - 1) ? (base + offset) : (PLAT_NR - 1);
}

/*
 * Convert the given index of the bucket array to the value
 * represented by the bucket
 */
static unsigned int plat_idx_to_val(unsigned int idx) {
  unsigned int error_bits, k, base;

  if (idx >= PLAT_NR) {
    fprintf(stderr, "idx %u is too large\n", idx);
    exit(1);
  }

  /* MSB <= (PLAT_BITS-1), cannot be rounded off. Use
   * all bits of the sample as index */
  if (idx < (PLAT_VAL << 1))
    return idx;

  /* Find the group and compute the minimum value of that group */
  error_bits = (idx >> PLAT_BITS) - 1;
  base = 1 << (error_bits + PLAT_BITS);

  /* Find its bucket number of the group */
  k = idx % PLAT_VAL;

  /* Return the mean of the range of the bucket */
  return base + ((k + 0.5) * (1 << error_bits));
}

static unsigned int calc_percentiles(unsigned int* io_u_plat,
                                     unsigned long nr,
                                     unsigned int** output,
                                     unsigned long** output_counts) {
  unsigned long sum = 0;
  unsigned int len, i, j = 0;
  unsigned int oval_len = 0;
  unsigned int* ovals = NULL;
  unsigned long* ocounts = NULL;
  unsigned long last = 0;
  int is_last;

  len = 0;
  while (len < PLAT_LIST_MAX && plist[len] != 0.0)
    len++;

  if (!len)
    return 0;

  /*
   * Calculate bucket values, note down max and min values
   */
  is_last = 0;
  for (i = 0; i < PLAT_NR && !is_last; i++) {
    sum += io_u_plat[i];
    while (sum >= (plist[j] / 100.0 * nr)) {
      if (j == oval_len) {
        oval_len += 100;
        ovals = realloc(ovals, oval_len * sizeof(unsigned int));
        ocounts = realloc(ocounts, oval_len * sizeof(unsigned long));
      }

      ovals[j] = plat_idx_to_val(i);
      ocounts[j] = sum;
      is_last = (j == len - 1);
      if (is_last)
        break;
      j++;
    }
  }

  for (i = 1; i < len; i++) {
    last += ocounts[i - 1];
    ocounts[i] -= last;
  }
  *output = ovals;
  *output_counts = ocounts;
  return len;
}

static void calc_p99(struct stats* s, int* p95, int* p99) {
  unsigned int* ovals = NULL;
  unsigned long* ocounts = NULL;
  int len;

  len = calc_percentiles(s->plat, s->nr_samples, &ovals, &ocounts);
  if (len && len > PLIST_P99)
    *p99 = ovals[PLIST_P99];
  if (len && len > PLIST_P99)
    *p95 = ovals[PLIST_P95];
  if (ovals)
    free(ovals);
  if (ocounts)
    free(ocounts);
}

/**
 * stats s, 待输出的统计数据
 * long long runtime, 表示这次是在运行了多长时间之后的输出
 */
static void show_latencies(struct stats* s, unsigned long long runtime) {
  unsigned int* ovals = NULL;     // latency usec
  unsigned long* ocounts = NULL;  // samples
  unsigned int len, i;

  len = calc_percentiles(s->plat, s->nr_samples, &ovals, &ocounts);
  if (len) {
    fprintf(stderr,
            "Latency percentiles (usec) runtime %llu (s) (%lu total samples)\n",
            runtime, s->nr_samples);
    for (i = 0; i < len; i++)
      fprintf(stderr, "\t%s%2.1fth: %u (%lu samples)\n",
              i == PLIST_P99 ? "*" : "",  // 第 5 行输出 99.0th，前面加 *
              plist[i], ovals[i], ocounts[i]);
  }

  if (ovals)
    free(ovals);
  if (ocounts)
    free(ocounts);

  fprintf(stderr, "\tmin=%u, max=%u\n", s->min, s->max);
}

/* fold latency info from s into d */
void combine_stats(struct stats* d, struct stats* s) {
  int i;
  for (i = 0; i < PLAT_NR; i++)
    d->plat[i] += s->plat[i];
  d->nr_samples += s->nr_samples;
  if (s->max > d->max)
    d->max = s->max;
  if (d->min == 0 || s->min < d->min)
    d->min = s->min;
}

/* record a latency result into the histogram */
static void add_lat(struct stats* s, unsigned int us) {
  int lat_index = 0;

  if (us > s->max)
    s->max = us;
  if (s->min == 0 || us < s->min)
    s->min = us;

  lat_index = plat_val_to_idx(us);
  __sync_fetch_and_add(&s->plat[lat_index], 1);
  __sync_fetch_and_add(&s->nr_samples, 1);
}

struct request {
  struct timeval start_time;
  struct request* next;
};

/*
 * every thread has one of these, it comes out to about 19K thanks to the
 * giant stats struct
 */
struct thread_data {
  pid_t pid;
  /* ->next is for placing us on the msg_thread's list for waking */
  struct thread_data* next;

  /* ->request is all of our pending request */
  struct request* request;

  /* our parent thread and messaging partner */
  struct thread_data* msg_thread;

  /*
   * the msg thread stuffs gtod in here before waking us, so we can
   * measure scheduler latency
   */
  struct timeval wake_time;

  /* keep the futex and the wake_time in the same cacheline */
  int futex;

  int thread_index;  // message/worker thread index

  /* worker 线程运行的自定义函数，每次唤醒时执行一次 */
  void (*worker_rtn)(pid_t,  // 线程号
                     int,    // 线程索引，是 message 的第几个孩子
                     unsigned long);  // 运行的次数

  /* mr axboe's magic latency histogram */
  struct stats stats;
  unsigned long long loop_count;
  unsigned long long runtime;

  char pipe_page[PIPE_TRANSFER_BUFFER];
};

/* we're so fancy we make our own futex wrappers */
/*
 * futex = Fast Userspace muTEXes，快速用户空间互斥体
 * https://cloud.tencent.com/developer/article/1176832
 * https://developer.aliyun.com/article/6043
 */
#define FUTEX_BLOCKED 0  // 锁被占用
#define FUTEX_RUNNING 1  // 锁可用

static int futex(int* uaddr,
                 int futex_op,
                 int val,
                 const struct timespec* timeout,
                 int* uaddr2,
                 int val3) {
  return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

/*
 * wakeup a process waiting on a futex, making sure they are really waiting
 * first
 * 唤醒
 */
static void fpost(int* futexp) {
  int s;

  // bool __sync_bool_compare_and_swap (type *ptr, type oldval type newval, ...)
  // 比较*ptr与oldval的值，如果两者相等，则将newval更新到*ptr并返回true
  if (__sync_bool_compare_and_swap(futexp, FUTEX_BLOCKED, FUTEX_RUNNING)) {
    // 多进程使用 FUTEX_WAKE，多线程使用 FUTEX_WAKE_PRIVATE
    s = futex(futexp, FUTEX_WAKE, 1, NULL, NULL, 0);
    if (s == -1) {
      perror("FUTEX_WAKE");
      exit(1);
    }
  }
}

/*
 * wait on a futex, with an optional timeout.  Make sure to set
 * the futex to FUTEX_BLOCKED beforehand.
 *
 * This will return zero if all went well, or return -ETIMEDOUT if you
 * hit the timeout without getting posted
 * 等待
 */
static int fwait(int* futexp, struct timespec* timeout) {
  int s;
  while (1) {
    /* Is the futex available? */
    if (__sync_bool_compare_and_swap(futexp, FUTEX_RUNNING, FUTEX_BLOCKED)) {
      break; /* Yes */
    }
    /* Futex is not available; wait */
    s = futex(futexp, FUTEX_WAIT, FUTEX_BLOCKED, timeout, NULL, 0);
    if (s == -1 && errno != EAGAIN) {
      if (errno == ETIMEDOUT)
        return -ETIMEDOUT;
      perror("futex-FUTEX_WAIT");
      exit(1);
    }
  }
  return 0;
}

/*
 * cmpxchg based list prepend
 */
static void xlist_add(struct thread_data* head, struct thread_data* add) {
  struct thread_data* old;
  struct thread_data* ret;

  while (1) {
    old = head->next;
    add->next = old;
    ret = __sync_val_compare_and_swap(&head->next, old, add);
    if (ret == old)
      break;
  }
}

/*
 * xchg based list splicing.  This returns the entire list and
 * replaces the head->next with NULL
 */
static struct thread_data* xlist_splice(struct thread_data* head) {
  struct thread_data* old;
  struct thread_data* ret;

  while (1) {
    old = head->next;
    ret = __sync_val_compare_and_swap(&head->next, old, NULL);
    if (ret == old)
      break;
  }
  return ret;
}

/*
 * Wake everyone currently waiting on the message list, filling in their
 * thread_data->wake_time with the current time.
 *
 * It's not exactly the current time, it's really the time at the start of
 * the list run.  We want to detect when the scheduler is just preempting the
 * waker and giving away the rest of its timeslice.  So we gtod once at
 * the start of the loop and use that for all the threads we wake.
 *
 * Since pipe mode ends up measuring this other ways, we do the gtod
 * every time in pipe mode
 * 唤醒 messager 的所有 worker，并记录当前时间
 */
static void xlist_wake_all(struct thread_data* td) {
  struct thread_data* list;
  struct thread_data* next;
  struct timeval now;

  list = xlist_splice(td);
  gettimeofday(&now, NULL);
  while (list) {
    next = list->next;
    list->next = NULL;
    if (pipe_test) {
      memset(list->pipe_page, 1, pipe_test);
      gettimeofday(&list->wake_time, NULL);
    } else {
      memcpy(&list->wake_time, &now, sizeof(now));
    }
    fpost(&list->futex);
    list = next;
  }
}

/*
 * called by worker threads to send a message and wait for the answer.
 * In reality we're just trading one cacheline with the gtod and futex in
 * it, but that's good enough.  We gtod after waking and use that to
 * record scheduler latency.
 */
static struct request* msg_and_wait(struct thread_data* work_td,
                                    int* stopping) {
  struct timeval now;
  unsigned long long delta;

  if (pipe_test)
    memset(work_td->pipe_page, 2, pipe_test);

  /* set ourselves to blocked */
  work_td->futex = FUTEX_BLOCKED;
  gettimeofday(&work_td->wake_time, NULL);

  /* add us to the list */
  // 关联 messager 和 workers，这样就可以通过 messager 唤醒 worker
  xlist_add(work_td->msg_thread, work_td);

  // 唤醒 messager 进程，即将 messager 进程加入运行队列
  fpost(&work_td->msg_thread->futex);

  /*
   * don't wait if the main threads are shutting down,
   * they will never kick us fpost has a full barrier, so as long
   * as the message thread walks his list after setting stopping,
   * we shouldn't miss the wakeup
   */
  if (!*stopping) {
    /* if he hasn't already woken us up, wait */
    fwait(&work_td->futex, NULL);  // 等待 msg 进程唤醒，即从运行队列删除
  }

  if (!requests_per_sec) {
    gettimeofday(&now, NULL);
    // 调度延迟为当前时间减去 work 进程被 messager 唤醒加入运行队列的时间
    delta = tvdelta(&work_td->wake_time, &now);
    if (delta > 0)
      add_lat(&work_td->stats, delta);  // 记录调度时延
  }
  return NULL;
}

#if defined(__x86_64__) || defined(__i386__)
#define nop __asm__ __volatile__("rep;nop" : : : "memory")
#elif defined(__aarch64__)
#define nop __asm__ __volatile__("yield" ::: "memory")
#elif defined(__powerpc64__)
#define nop __asm__ __volatile__("nop" : : : "memory")
#else
#error Unsupported architecture
#endif

// 原地自旋一定长度的时间
static void usec_spin(unsigned long spin_time) {
  struct timeval now;
  struct timeval start;
  unsigned long long delta;

  if (spin_time == 0)
    return;

  gettimeofday(&start, NULL);
  while (1) {
    gettimeofday(&now, NULL);
    delta = tvdelta(&start, &now);
    if (delta > spin_time)
      return;
    nop;
  }
}

/*
 * once the message thread starts all his children, this is where he
 * loops until our runtime is up.  Basically this sits around waiting
 * for posting by the worker threads, replying to their messages after
 * a delay of 'sleeptime' + some jitter.
 */
static void run_msg_thread(struct thread_data* msg_td, int* stopping) {
  unsigned int seed = getpid();
  int max_jitter = sleeptime / 4;
  int jitter = 0;

  while (1) {
    msg_td->futex = FUTEX_BLOCKED;
    xlist_wake_all(msg_td);  // 唤醒所有 worker 线程，即 work 线程被加入运行队列

    if (*stopping) {
      xlist_wake_all(msg_td);
      break;
    }
    if (sleeptime)  // 等待 worker 线程唤醒
      fwait(&msg_td->futex, NULL);

    /*
     * messages shouldn't be instant, sleep a little to make them
     * wait
     */
    usec_spin(message_cputime);
    if (!pipe_test && sleeptime) {
      jitter = rand_r(&seed) % max_jitter;
      usleep(sleeptime + jitter);  // 挂起
    }
  }
}

/*
 * the worker thread is pretty simple, it just does a single spin and
 * then waits on a message from the message thread
 */
void* worker_thread(void* arg, int* stopping) {
  struct thread_data* work_td = arg;
  struct timeval now;
  struct timeval start;

  gettimeofday(&start, NULL);
  while (1) {
    if (*stopping)
      break;

    usec_spin(cputime);
    work_td->worker_rtn(work_td->pid, work_td->thread_index,
                        work_td->stats.nr_samples);  // 运行自定义函数
    work_td->loop_count++;
    gettimeofday(&now, NULL);
    work_td->runtime = tvdelta(&start, &now);

    msg_and_wait(work_td, stopping);
  }
  gettimeofday(&now, NULL);
  work_td->runtime = tvdelta(&start, &now);

  return NULL;
}

/*
 * the message thread starts his own gaggle of workers and then sits around
 * replying when they post him.  He collects latency stats as all the threads
 * exit
 */
void* message_thread(void* arg, int msg_index, int* stopping) {
  struct thread_data* msg_td =
      (struct thread_data*)arg + msg_index;  // 当前 msg 进程数据
  struct thread_data* worker_threads_mem = NULL;
  int i;
  int ret;

  worker_threads_mem = msg_td + 1;

  if (!worker_threads_mem) {
    perror("unable to allocate ram");
    pthread_exit((void*)-ENOMEM);
  }

  for (i = 0; i < worker_threads; i++) {
    pid_t pid;

    if ((pid = fork()) < 0) {
      perror("fork error!");
      exit(1);
    } else if (pid == 0) {  // child
      worker_thread(worker_threads_mem + i, stopping);
      return;
    } else {  // father
      worker_threads_mem[i].pid = pid;
      worker_threads_mem[i].thread_index = i;
      worker_threads_mem[i].worker_rtn = msg_td->worker_rtn;
      worker_threads_mem[i].msg_thread = msg_td;
    }
  }

  run_msg_thread(msg_td, stopping);

  // 唤醒所有 work 进程，并等待他们运行结束
  for (i = 0; i < worker_threads; i++) {
    fpost(&worker_threads_mem[i].futex);
    while (waitpid(worker_threads_mem[i].pid, NULL, 0) > 0)
      ;
  }
  return NULL;
}

// 合并进程的统计信息
static void combine_message_thread_stats(struct stats* stats,
                                         struct thread_data* thread_data,
                                         unsigned long long* loop_count,
                                         unsigned long long* loop_runtime) {
  struct thread_data* worker;
  int i;
  int msg_i;
  int index = 0;

  *loop_count = 0;
  *loop_runtime = 0;
  for (msg_i = 0; msg_i < message_threads; msg_i++) {
    index++;
    for (i = 0; i < worker_threads; i++) {
      worker = thread_data + index++;
      combine_stats(stats, &worker->stats);
      *loop_count += worker->loop_count;
      *loop_runtime += worker->runtime;
    }
  }
}

static void reset_thread_stats(struct thread_data* thread_data) {
  struct thread_data* worker;
  int i;
  int msg_i;
  int index = 0;

  for (msg_i = 0; msg_i < message_threads; msg_i++) {
    index++;
    for (i = 0; i < worker_threads; i++) {
      worker = thread_data + index++;
      memset(&worker->stats, 0, sizeof(worker->stats));
    }
  }
}

/* runtime from the command line is in seconds.  Sleep until its up */
static void sleep_for_runtime(struct thread_data* message_threads_mem,
                              int* stopping) {
  struct timeval now;
  struct timeval zero_time;
  struct timeval last_calc;
  struct timeval start;
  struct stats stats;
  unsigned long long loop_count;
  unsigned long long loop_runtime;
  unsigned long long delta;
  unsigned long long runtime_delta;
  unsigned long long runtime_usec = runtime * USEC_PER_SEC;
  unsigned long long warmup_usec = warmuptime * USEC_PER_SEC;
  unsigned long long interval_usec =
      intervaltime *
      USEC_PER_SEC;  // 两个命令行输出之间的间隔时间，默认是 10 秒
  unsigned long long zero_usec = zerotime * USEC_PER_SEC;
  int warmup_done = 0;

  memset(&stats, 0, sizeof(stats));
  gettimeofday(&start, NULL);  // 启动时间
  last_calc = start;
  zero_time = start;

  while (1) {
    gettimeofday(&now, NULL);
    runtime_delta = tvdelta(&start, &now);  // 当前时间

    if (runtime_usec &&
        runtime_delta >= runtime_usec)  // 超过用户规定的运行时间，退出
      break;

    if (!requests_per_sec && !pipe_test && runtime_delta > warmup_usec &&
        !warmup_done &&
        warmuptime) {  // 运行一段热机时间，结束时清空热机统计数据
      warmup_done = 1;
      fprintf(stderr, "warmup done, zeroing stats\n");
      zero_time = now;
      reset_thread_stats(message_threads_mem);
    } else if (!pipe_test && !requests_per_sec) {
      delta = tvdelta(&last_calc, &now);
      if (delta >= interval_usec) {  // 每次运行一定时间就产生一次输出
        memset(&stats, 0, sizeof(stats));
        combine_message_thread_stats(&stats, message_threads_mem, &loop_count,
                                     &loop_runtime);
        show_latencies(&stats, runtime_delta / USEC_PER_SEC);
        last_calc = now;
      }
    }
    if (zero_usec) {
      unsigned long long zero_delta;
      zero_delta = tvdelta(&zero_time, &now);
      if (zero_delta > zero_usec) {
        zero_time = now;
        reset_thread_stats(message_threads_mem);
      }
    }
    sleep(1);
  }
  __sync_synchronize();  // 读写内存屏障
  *stopping = 1;
}

/* 运行测试 */
int wlpthread_run(int ac,
                  char** av,
                  void (*worker_rtn)(pthread_t,  // worker 函数
                                     int,
                                     unsigned long)) {
  int i;
  int ret;
  struct thread_data* message_threads_mem = NULL;
  int* stopping = NULL;  // 控制全局运行状态
  struct stats stats;
  unsigned long long loop_count;
  unsigned long long loop_runtime;

  // 申请共享内存，fork 自动共享，参考 http://man.he.net/man2/futex
  message_threads_mem = (struct thread_data*)mmap(
      NULL,
      sizeof(struct thread_data) *
          (message_threads * worker_threads + message_threads),
      PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
  stopping = (int*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_SHARED, -1, 0);
  *stopping = 0;

  parse_options(ac, av);  // 将命令行参数赋值给全局变量
  memset(&stats, 0, sizeof(stats));

  for (i = 0; i < message_threads; i++)  // 创建 message 线程
  {
    // 父进程位置，数组 [(message, workers...)
    int msg_index = i * worker_threads + i;
    pid_t pid;

    if ((pid = fork()) < 0) {
      perror("fork error!");
      exit(1);
    } else if (pid == 0) {  // child
      message_thread(message_threads_mem, msg_index, stopping);
      return;
    } else {  // father
      message_threads_mem[msg_index].pid = pid;
      message_threads_mem[msg_index].thread_index = i;
      message_threads_mem[msg_index].worker_rtn = worker_rtn;
    }
  }

  sleep_for_runtime(message_threads_mem, stopping);
  // 唤醒所有 message 进程，并等待他们运行结束
  for (int i = 0; i < message_threads; i++) {
    int index = i * worker_threads + i;
    fpost(&message_threads_mem[index].futex);
    while (waitpid(message_threads_mem[index].pid, NULL, 0) > 0)
      ;
  }

  memset(&stats, 0, sizeof(stats));
  // 所有的进程运行完毕之后，将所有的进程延迟时间的统计信息合并到一个统计信息结构体中
  combine_message_thread_stats(&stats, message_threads_mem, &loop_count,
                               &loop_runtime);

  // 显示最终统计信息，系统中总的输出有定时（per 10s）输出，还有一次最终输出
  show_latencies(&stats, runtime);
}
