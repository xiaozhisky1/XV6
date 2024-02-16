#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
void
kvminit()
{
  kernel_pagetable = (pagetable_t) kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
// 映射内核页表
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable)); // 将根页表页的物理地址写入寄存器 `satp` 中, 之后，CPU 将使用内核页表翻译地址
  sfence_vma(); // 在从内核空间返回用户空间前，切换到用户页表的trampoline 代码中执行 `sfence.vma`, 以告诉 CPU 使相应的缓存 TLB 项无效
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
//通过虚拟地址得到页表项PTE,每次降低 9 位来查找三级页表
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc) 
{ 
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) { // 使用每一级的 9 位虚拟地址来查找下一级页表或最后一级
    pte_t *pte = &pagetable[PX(level, va)]; 
    if(*pte & PTE_V) {  
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {  // 如果 PTE 无效，那么所需的物理页还没有被分配
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)  // if(alloc == 0 || ...)
        return 0;
      // 如果 `alloc` 参数被设置 (即 alloc != 0)，`walk` 会分配一个新的页表页，并把它的物理地址放在 PTE 中
      memset(pagetable, 0, PGSIZE); 
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)]; // 返回三级页表中最低层PTE的地址
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
// 将指定范围的虚拟地址映射到一段物理地址
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  
  pte = walk(kernel_pagetable, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
//将虚拟地址映射到物理地址
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) //perm:权限
{                                                                 
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0) //找到该地址最后一级PTE的地址
      return -1; // 失败返回-1
    if(*pte & PTE_V) //如果页表已经被映射
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;  //配置 PTE，使其持有相关的物理页号、所需的权限(prem)
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
// 可以用于释放已经被映射的页
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // 使用 `walk` 来查找 PTE 
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);  // 使用 `kfree` 来释放它们所引用的物理内存
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// 通过 `kalloc` 分配物理内存，并使用 `mappages` 将 PTE 添加到用户页表中
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{ 
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc(); // 分配物理内存
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE); // 初始化内存区域
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){ // 将 PTE 添加到用户页表中，若失败则执行if{}
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
// 释放页表所占物理页，前提是页表项所指空间已被释放
void freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);//将所有用户空间的物理页释放
  freewalk(pagetable);// 将页表所占空间释放
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){// 虚拟地址从0开始
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
// 读取由用户指针指向的内存
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{// dst：目标缓冲区的指针，用于存储复制的数据。srcva：源虚拟地址，表示要复制的数据在用户空间的位置。
  return copyin_new(pagetable, dst, srcva, len);
  /*
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);// 将 srcva 向下对齐到页面边界，得到 va0。
    pa0 = walkaddr(pagetable, va0);//使用 walkaddr 函数查找 va0 对应的物理地址 pa0。
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);// 计算可以从当前页面复制的最大字节数 n，确保不越界。
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);//将从 pa0 开始的 n 字节数据复制到 dst。
    // 更新剩余的字节数 len 和目标缓冲区指针 dst。
    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;// 更新 srcva 到下一个页面的起始地址。
  }
  
  return 0;*/
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  return copyinstr_new(pagetable, dst, srcva, max);
  /*
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
  */
}

void vmprintIn(pagetable_t pagetable, int level){
  if(level > 3)return;
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if(pte & PTE_V){//页表项有效
      for (int j = 0; j < level; j++){
        if (j) printf(" ");
        printf("..");
    }
      uint64 child = PTE2PA(pte);// 从获取pa
      printf("%d: pte %p pa %p\n", i, pte, child);
      if((pte & (PTE_R|PTE_W|PTE_X)) == 0){
        //对于指向不能读写执行的地址的 PTE，其中存储的就是下一级页表的地址
        vmprintIn((pagetable_t)child, level + 1);
      }
    }
  }
}

void vmprint(pagetable_t pagetable){
  printf("page table %p\n", pagetable);
  vmprintIn(pagetable, 1);
}

void uvmmap(pagetable_t pagetable, uint64 va, uint64 pa, uint64 size, int perm){// 实现va->pa的映射
  if (mappages(pagetable, va, size, pa, perm) != 0)
    panic("uvmmap");
}

pagetable_t proc_kpt_init(){ // 创建一个和全局内核页表一样的页表
  // 创建一个新页表
  pagetable_t kernelpt ;
  kernelpt = (pagetable_t) kalloc();
  memset(kernelpt, 0, PGSIZE);
// xv6的用户进程地址空间不会超过页表第一项的范围，所以只需要从第二项开始拷贝内核页表项
  int i;
  for(i = 1; i < 512; i++){ // 内核页表是512*512*512,用户空间为1*512*512
    kernelpt[i] = kernel_pagetable[i];
  }
  // uart registers
  uvmmap(kernelpt, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  uvmmap(kernelpt, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  uvmmap(kernelpt, CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  uvmmap(kernelpt, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  return kernelpt;
}
// 将用户空间映射到内核页表
void setup_uvmkvm(pagetable_t pagetable, pagetable_t kernelpt, uint64 oldsz, uint64 newsz){
  pte_t *pte, *kpte;                                           // 起始位置，        终止位置
  uint64 va;

  if (newsz >= PLIC)
    panic("setupuvm2kvm: user process space is overwritten to kernel process space");

  for(va = oldsz; va < newsz; va += PGSIZE){
    // 找到对应页表项，如果不存在则创建
    if((kpte = walk(kernelpt, va, 1)) == 0)
      panic("setupuvm2kvm: kpte should exist");
    // 找到对应页表项
    if((pte = walk(pagetable, va, 0)) == 0)
      panic("setupuvm2kvm: pte should exist");
    // 不需要重新映射，页表项指向相同的物理页，并重新设置内核页表项标志
    *kpte = *pte;
    *kpte &= ~(PTE_U | PTE_W | PTE_X);
  }

  for(va = newsz; va < oldsz; va += PGSIZE){
    if((kpte = walk(kernelpt, va, 1)) == 0)
      panic("setupuvm2kvm: kpte should exist");
    *kpte &= ~PTE_V;
  }

}

