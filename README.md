# iOS 实时卡顿监控

>在移动设备上开发软件,性能一直是我们最为关心的话题之一,我们作为程序员除了需要努力提高代码质量之外,及时发现和监控软件中那些造成性能低下的”罪魁祸首”也是我们神圣的职责.

众所周知,iOS平台因为UIKit本身的特性,需要将所有的UI操作都放在主线程执行,所以也造成不少程序员都习惯将一些线程安全性不确定的逻辑,以及其它线程结束后的汇总工作等等放到了主线,所以主线程中包含的这些大量计算、IO、绘制都有可能造成卡顿.

在Xcode中已经集成了非常方便的调试工具Instruments,它可以帮助我们在开发测试阶段分析软件运行的性能消耗,但一款软件经过测试流程和实验室分析肯定是不够的,在正式环境中由大量用户在使用过程中监控、分析到的数据更能解决一些隐藏的问题.

<br />

### 寻找卡顿的切入点
>监控卡顿,最直接就是找到主线程都在干些啥玩意儿.我们知道一个线程的消息事件处理都是依赖于NSRunLoop来驱动,所以要知道线程正在调用什么方法,就需要从NSRunLoop来入手.CFRunLoop的代码是开源,可以在此处查阅到源代码http://opensource.apple.com/source/CF/CF-1151.16/CFRunLoop.c,其中核心方法CFRunLoopRun简化后的主要逻辑大概是这样的:
<br />

```
int32_t __CFRunLoopRun()
{
    //通知即将进入runloop
    __CFRunLoopDoObservers(KCFRunLoopEntry);
    
    do
    {
        // 通知将要处理timer和source
        __CFRunLoopDoObservers(kCFRunLoopBeforeTimers);
        __CFRunLoopDoObservers(kCFRunLoopBeforeSources);
        
        __CFRunLoopDoBlocks();  //处理非延迟的主线程调用
        __CFRunLoopDoSource0(); //处理UIEvent事件
        
        //GCD dispatch main queue
        CheckIfExistMessagesInMainDispatchQueue();
        
        // 即将进入休眠
        __CFRunLoopDoObservers(kCFRunLoopBeforeWaiting);
        
        // 等待内核mach_msg事件
        mach_port_t wakeUpPort = SleepAndWaitForWakingUpPorts();
        
        // Zzz...
        
        // 从等待中醒来
        __CFRunLoopDoObservers(kCFRunLoopAfterWaiting);
        
        // 处理因timer的唤醒
        if (wakeUpPort == timerPort)
            __CFRunLoopDoTimers();
        
        // 处理异步方法唤醒,如dispatch_async
        else if (wakeUpPort == mainDispatchQueuePort)
            __CFRUNLOOP_IS_SERVICING_THE_MAIN_DISPATCH_QUEUE__()
            
        // UI刷新,动画显示
        else
            __CFRunLoopDoSource1();
        
        // 再次确保是否有同步的方法需要调用
        __CFRunLoopDoBlocks();
        
    } while (!stop && !timeout);
    
    //通知即将退出runloop
    __CFRunLoopDoObservers(CFRunLoopExit);
}
```
<br />

>不难发现NSRunLoop调用方法主要就是在kCFRunLoopBeforeSources和kCFRunLoopBeforeWaiting之间,还有kCFRunLoopAfterWaiting之后,也就是如果我们发现这两个时间内耗时太长,那么就可以判定出此时主线程卡顿.

<br />

###量化卡顿的程度
>要监控NSRunLoop的状态,我们需要使用到CFRunLoopObserverRef,通过它可以实时获得这些状态值的变化,具体的使用如下:
>

<br />

```
static void runLoopObserverCallBack(CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
    MyClass *object = (__bridge MyClass*)info;
    object->activity = activity;
}

- (void)registerObserver
{
    CFRunLoopObserverContext context = {0,(__bridge void*)self,NULL,NULL};
    CFRunLoopObserverRef observer = CFRunLoopObserverCreate(kCFAllocatorDefault,
                                                            kCFRunLoopAllActivities,
                                                            YES,
                                                            0,
                                                            &runLoopObserverCallBack,
                                                            &context);
    CFRunLoopAddObserver(CFRunLoopGetMain(), observer, kCFRunLoopCommonModes);
}
```
<br />

>只需要另外再开启一个线程,实时计算这两个状态区域之间的耗时是否到达某个阀值,便能揪出这些性能杀手.
为了让计算更精确,需要让子线程更及时的获知主线程NSRunLoop状态变化,所以dispatch_semaphore_t是个不错的选择,另外卡顿需要覆盖到多次连续小卡顿和单次长时间卡顿两种情景,所以判定条件也需要做适当优化.将上面两个方法添加计算的逻辑如下:

<br />

```
static void runLoopObserverCallBack(CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
    MyClass *object = (__bridge MyClass*)info;
    
    // 记录状态值
    object->activity = activity;
    
    // 发送信号
    dispatch_semaphore_t semaphore = moniotr->semaphore;
    dispatch_semaphore_signal(semaphore);
}

- (void)registerObserver
{
    CFRunLoopObserverContext context = {0,(__bridge void*)self,NULL,NULL};
    CFRunLoopObserverRef observer = CFRunLoopObserverCreate(kCFAllocatorDefault,
                                                            kCFRunLoopAllActivities,
                                                            YES,
                                                            0,
                                                            &runLoopObserverCallBack,
                                                            &context);
    CFRunLoopAddObserver(CFRunLoopGetMain(), observer, kCFRunLoopCommonModes);
    
    // 创建信号
    semaphore = dispatch_semaphore_create(0);
    
    // 在子线程监控时长
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        while (YES)
        {
            // 假定连续5次超时50ms认为卡顿(当然也包含了单次超时250ms)
            long st = dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 50*NSEC_PER_MSEC));
            if (st != 0)
            {
                if (activity==kCFRunLoopBeforeSources || activity==kCFRunLoopAfterWaiting)
                {
                    if (++timeoutCount < 5)
                        continue;
                    
                    NSLog(@"好像有点儿卡哦");
                }
            }
            timeoutCount = 0;
        }
    });
}
```
###记录卡顿的函数调用

<br />

>监控到了卡顿现场,当然下一步便是记录此时的函数调用信息,此处可以使用一个第三方Crash收集组件PLCrashReporter,它不仅可以收集Crash信息也可用于实时获取各线程的调用堆栈,使用示例如下:

<br />

```
PLCrashReporterConfig *config = [[PLCrashReporterConfig alloc] initWithSignalHandlerType:PLCrashReporterSignalHandlerTypeBSD
                                                                   symbolicationStrategy:PLCrashReporterSymbolicationStrategyAll];
PLCrashReporter *crashReporter = [[PLCrashReporter alloc] initWithConfiguration:config];

NSData *data = [crashReporter generateLiveReport];
PLCrashReport *reporter = [[PLCrashReport alloc] initWithData:data error:NULL];
NSString *report = [PLCrashReportTextFormatter stringValueForCrashReport:reporter
                                                          withTextFormat:PLCrashReportTextFormatiOS];

NSLog(@"------------\n%@\n------------", report);
```
当检测到卡顿时,抓取堆栈信息,然后在客户端做一些过滤处理,便可以上报到服务器,通过收集一定量的卡顿数据后经过分析便能准确定位需要优化的逻辑,至此这个实时卡顿监控就大功告成了!


