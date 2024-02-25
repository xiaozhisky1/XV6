#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
// `usertrap`的作用是确定trap的原因，处理它，然后返回
void 
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);// 首先改变`stvec`，这样在内核中发生的trap将由`kernelvec`处理

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();// 保存`sepc`（用户PC）
  
  if(r_scause() == 8){ // 如果trap是系统调用，`syscall`会处理它
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4; // `usertrap`会把用户`pc`加4，因为RISC-V在执行系统调用时，会留下指向`ecall`指令的程序指针，而我们希望能返回到ecall的下一条指令

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){// 如果是设备中断，`devintr`会处理
    // ok
  } else { // 否则就是异常，内核会杀死故障进程
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed) // 在退出时，`usertrap`检查进程是否已经被杀死
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)  // 如果这个trap是一个定时器中断,应该让出CPU
  {
    // alm.count 即 tricks 数量不为0， 且 handler 不在运行中
    if(p->alm.count !=0 && p->alm.processing == 0 && --p->alm.ticks_count == 0)
    {
      p->alm.ticks_count = p->alm.count;
      p->alm.processing = 1;
      // 保存当前所有寄存器
      memmove(p->alm.trapframe, p->trapframe, sizeof (struct trapframe));
      // epc 是 sret 设置的 PC，更换之后就可以返回到 handler
      p->trapframe->epc = (uint64)p->alm.handler;
    }

    yield();
  }

  usertrapret();
}

//
// return to user space
// 回到用户空间的第一步是调用`usertrapret`
void 
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();// 关中断

  // 设置RISC-V控制寄存器，为以后用户空间trap做准备
  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));// 这包括改变`stvec`来引用`uservec`

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  // 准备`uservec`所依赖的`trapframe`字段
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc); // 并将`sepc`设置为先前保存的用户程序计数器

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline); 
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);// `usertrapret`在用户页表和内核页表中映射的trampoline页上调用`userret`，因为`userret`中的汇编代码会切换页表
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
// `kernelvec`在保存寄存器后跳转到这里；`kerneltrap`是为两种类型的trap准备的：设备中断和异常
void kerneltrap()
{ 
  int which_dev = 0; 
  uint64 sepc = r_sepc(); // 因为下面的`yield()`可能破坏保存的`sepc`和在`sstatus`中保存的之前的模式。`kerneltrap`在启动时保存它们
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){// 调用`devintr`检查和处理设备中断，若是中断就跳过下面三条语句
    printf("scause %p\n", scause);// 如果trap不是设备中断，那么它必须是异常,如果它发生在xv6内核中，则一定是一个致命错误
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");// 内核调用`panic`并停止执行
  }

  // give up the CPU if this is a timer interrupt.
  // 如果由于计时器中断而调用了`kerneltrap`，并且进程的内核线程正在运行（而不是调度程序线程）
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield(); // `kerneltrap`调用`yield`让出CPU，允许其他线程运行; 在某个时刻，其中一个线程将退出，并让我们的线程及其`kerneltrap`恢复

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  // 当`kerneltrap`的工作完成时，它需要返回到被中断的代码
  // 恢复`sepc`和`sstatus`控制寄存器并返回到`kernelvec`
  w_sepc(sepc); 
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

