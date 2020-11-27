# 唤醒延迟测试器

基于 [schbench](https://git.kernel.org/pub/scm/linux/kernel/git/mason/schbench.git/) 工具进行二次开发的，支持自定义负载类型的延迟测试工具

## 使用

1. 在 `run.c:worker_load()` 中添加自定义的负载，可以通过负载函数的参数控制执行，如特定进程的突发任务
2. make
3. `./run`

支持 schbench 的可配置参数，除了autobench、pipe_test、requests_per_sec，只需在第三步执行时传入

## 执行流程

由一个 run 线程生成 message_threads 个 messager 线程，再由每个 messager 分别生成 worker_threads 个 worker 线程。run 线程在生成所有 messager 线程之后调用 `sleep_for_runtime` 函数，首先判断前 warmuptime 秒的时间为热机时间，热机时间的统计信息不计入统计结果，接下去每 10 秒输出一次当前的所有线程统计信息，最后到用户设置的执行时间 runtime 时，再输出最终结果。

```
run -- messager1 -- worker1.1
    |           | 
    |            -- worker1.2
    |           | 
    |            -— worker1.3
    |           |
    |            ...
    |
    -- messager2 -- worker2.1
    |           | 
    |            -- worker2.2
    |           | 
    |            -- worker2.3
    |           |
    |            ...
    ...
```

messager 和 worker 之间采用 futex 同步，每个线程的同步变量记录在 thread_data::futex。messager 线程开始执行时首先创建其下所有 worker，然后执行一个死循环，循环第一步设置 futex 保证下面的 wait 总能陷入睡眠等待，然后唤醒所有等待他的 worker，最后睡眠等待 worker 唤醒；worker 进程执行死循环，循环开始时执行负载函数，再将自己加入到 messager 的等待队列，然后唤醒其 messager，睡眠等待其 messager，最后记录唤醒延迟。messager 在唤醒等待他的所有 worker 时，会记录唤醒时间 t_w，被唤醒的 worker 开始执行时，记录当前时间 t_r，则该 worker 的唤醒延迟 latency = t_r - t_w。每次的统计信息记录在  thread_data::stats。

``` 
messagers:                              workers:

  create workers                          while(1)
  while(1)                                   worker_load()
    set futex = BLOCKED                      add to messager's list
                                    -------- wake messager
    wake all list workers ---------|-------> wait its messager
    wait one worker  <-------------          record latency
```

## 进程模式

由于 Linux 下的多线程共享内存 mm_struct，导致每个线程的占用内存总是一样，故不能模拟每个线程占用资源（IO、内存）不一致的情况，故开发了多进程模式。使用进程模式需要切换到分支 `feature/threads2processer` 重新编译源码。进程模式相比于线程模式主要做了以下修改：

1. 用 fork 函数代替 pthread 的 API
2. 线程模式，全局的数据 message_threads_mem 和 stopping，储存在线程共享的全局变量中；进程模式，全局变量不能共享，全局数据使用 `mmap` 申请的共享内存
3. 线程模式的 futex API 使用带 `_PRIVATE` 后缀的 flag，保证只能在此线程所属的进程中访问

## 参考

[futex 简介与 demo](http://man.he.net/man2/futex)

[futex on process](https://github.com/eliben/code-for-blog/blob/master/2018/futex-basics/futex-basic-process.c)

[Gcc 内置原子操作__sync_系列函数简述及例程](https://zhuanlan.zhihu.com/p/32303037)

[Linux 段错误 Segfault 错误代码分析](https://blog.csdn.net/SweeNeil/article/details/84136290)
