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

void worker_run(pthread_t tid, int idx, unsigned long nr_samples) {
  if (idx % 2 == 0 && nr_samples % 50 == 0) {
    char* p = (char*)malloc(pagesize * idx * 100);
    memset(p, 1, pagesize * idx * 100);
    // printf("tid: %lu, index: %d, nr_samples: %lu\n", tid, idx, nr_samples);
  }
}

int main(int ac, char** av) {
  wlpthread_run(ac, av, worker_run);

  return 0;
}