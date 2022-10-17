#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

/*
 * VMA is allocated from high address to low address.
 *
 */
#define VMA_TOP TRAPFRAME
static struct vma*
vma_alloc(struct proc *p, uint64 *addr, uint64 *length)
{
  uint64 min = *addr;
  uint64 max = *addr + *length;
  *addr = PGROUNDDOWN(min);
  *length = PGROUNDUP(max)-*addr;
  uint64 vma_top = VMA_TOP;
  struct vma* free = 0;
  for (struct vma* i=p->mvma; i < p->mvma+NVMA; ++i) {
    if (i->addr && i->addr < vma_top) {
      vma_top = i->addr;
    }
    if (i->addr==0 && free==0) {
      free = i;
    }
  }
  *addr = vma_top - *length;
  return free;
}

struct vma*
vma_hit(struct proc* p, uint64 addr)
{
  for (struct vma* i=p->mvma; i < p->mvma+NVMA; ++i) {
    if (i->addr && i->addr <=addr && addr <i->addr+i->length) {
      return i;
    }
  }
  return 0;
}

void*
mmap(void *addr, uint64 length, int prot, int flags,
           int fd, uint64 offset)
{
  struct proc* p = myproc();
  struct vma *free = vma_alloc(p, (uint64*)&addr, &length);
  if(!free) return (void*) -1;
  free->addr = (uint64) addr;
  free->length = length;
  free->ofile = p->ofile[fd];
  free->readable = (prot & PROT_READ)!=0;
  free->writeable = (prot & PROT_WRITE)!=0;
  free->shared = (flags & MAP_SHARED)!=0;
  free->offset = offset;
  if (!free->ofile) goto bad;
  if (free->shared) {
    if (!free->ofile->readable && free->readable) goto bad;
    if (!free->ofile->writable && free->writeable) goto bad;
  }
  if (!free->shared) {
    if (!free->ofile->readable) goto bad;
  }
  filedup(free->ofile);
  return addr;
bad:
  memset(free, 0, sizeof(*free));
  return (void*) -1;
}

static int
vma_write(struct inode* ip, uint64 addr, uint64 off, int n)
{
  int r, ret = 0;
  int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
  int i = 0;
  while(i < n){
    int n1 = n - i;
    if(n1 > max)
      n1 = max;

    begin_op();
    ilock(ip);
    if ((r = writei(ip, 1, addr + i, off, n1)) > 0)
      off += r;
    iunlock(ip);
    end_op();

    if(r != n1){
      // error from writei
      break;
    }
    i += r;
  }
  ret = (i == n ? n : -1);
  return ret;
}

int
munmap(void *addr, uint64 length)
{
  uint64 _addr = (uint64) addr;
  struct proc *p = myproc();
  struct vma* target = vma_hit(p, _addr);
  if (!target) return -1;
  uint64 min = target->addr;
  uint64 max = PGROUNDUP(_addr+length);
  if (max>(target->addr+length)) return -1;
  for (uint64 cur_addr = min; cur_addr < max; cur_addr+=PGSIZE) {
    if (walkaddr(p->pagetable, cur_addr)!=0) {
      if (target->shared&&target->writeable) {
        if(vma_write(target->ofile->ip, cur_addr, cur_addr-target->addr+target->offset, PGSIZE)==-1) return -1;
      }
      uvmunmap(p->pagetable, cur_addr, 1, 1);
    }
    target->addr += PGSIZE;
    target->offset += PGSIZE;
    target->length -= PGSIZE;
  }

  if(target->length==0){
    fileclose(target->ofile);
    memset(target, 0, sizeof(*target));
  }
  return 0;
}

// handle page fault of mmap, return the handled page addr, otherwise return 0
uint64
mmap_trap(uint64 addr, char read)
{
  struct proc *p = myproc();
  struct vma* target = vma_hit(p, addr);
  if (!target) return 0;
  if ((read && !target->readable) || (!read && !target->writeable)) return 0;
  uint64 va = PGROUNDDOWN(addr);
  uint64 mem = (uint64) kalloc();
  if(mem == 0) return 0;
  memset((void*) mem, 0, PGSIZE);
  int perm = PTE_U;
  if(target->writeable) perm |= PTE_W;
  if(target->readable) perm |= PTE_R;
  if(mappages(p->pagetable, va, PGSIZE, mem, perm) != 0){
    kfree((void*) mem);
    return 0;
  }
  struct inode *ip = target->ofile->ip;
  ilock(ip);
  int tot = readi(ip, 0, mem, va-target->addr+target->offset, PGSIZE);
  iunlock(ip);
  if (tot==-1){
    uvmunmap(p->pagetable, va, 1, 1);
    return 0;
  }
  return va;
}

void
mcopy(struct proc* p, struct proc* np)
{
  for(int i = 0; i < NVMA; i++){
    if(p->mvma[i].addr){
      np->mvma[i] = p->mvma[i];
      struct vma *target = &np->mvma[i];
      filedup(target->ofile);
      for(uint64 va=target->addr; va<target->addr+target->length; va+=PGSIZE){
        uint64 pa = walkaddr(p->pagetable, va);
        if(!pa) continue;
        uint64 new_pa = (uint64) kalloc();
        if (new_pa==0) goto err;
        memmove((void*) new_pa, (const void*) pa, PGSIZE);
        int perm = PTE_U;
        if(target->writeable) perm |= PTE_W;
        if(target->readable) perm |= PTE_R;
        mappages(np->pagetable, va, PGSIZE, new_pa, perm);
      }
    }
  }
  return;
err:
  panic("mcopy");
}