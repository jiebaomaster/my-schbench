#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "wake-latency-thread.h"

static const int pagesize = 1024 * 4;

/**
 * 钩子函数，worker 每次执行时执行一次
 * pid 当前执行的进程 id
 * idx 当前 work 进程是其父 msg 进程的第几个孩子
 * nr_run 当前进程第几次执行该函数
 */
void worker_load(pid_t pid, int idx, unsigned long nr_run) {}

int main(int ac, char** av) {
  wlpthread_run(ac, av, worker_load);

  return 0;
}