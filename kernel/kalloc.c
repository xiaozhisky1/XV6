// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock; // 自旋锁
  struct run *freelist;  // 可供分配的物理内存页的**空闲链表**, 每个空闲页的链表元素是一个结构体 `struct run`
} kmem; // 分配器

void
kinit() // 初始化分配器
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP); // 初始化空闲页链表，以保存内核地址结束到`PHYSTOP` 之间的每一页 
  // `kinit` 通过调用`freerange` 来添加内存到空闲页链表，`freerange` 则对每一页都调用 `kfree`
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start); // 使用 `PGROUNDUP` 来确保它只添加对齐的物理地址到空闲链表中
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) // 对每一页都调用 `kfree`
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// 释放某pa对应的页，将其加入freelist
void kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE); // 将被释放的内存中的每个字节设置为1, 这将使得释放内存后使用内存的代码（使用悬空引用）将会读取垃圾而不是旧的有效内容
  // 接下来将页面预存入释放列表
  r = (struct run*)pa; // 将 `pa`（物理地址）转为指向结构体 `run` 的指针
  // 将其加入到freelist中（不带头结点的头插法）
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// `kalloc`移除并返回空闲链表中的第一个元素
void *
kalloc(void) 
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next; // 移除队头元素
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;//
}
