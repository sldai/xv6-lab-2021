The process address space is composed of normal part (text, global data, stack, heap), trampoline, trapframe and virtual memory area.
To make the normal part unaffected by our VMA, we place VMA at the bottom of the trapframe:
```text
trampoline
trapframe
VMA0
VMA1
...
heap
stack
...
```

VMA data structure contains the virtual address, file and permission. Each process has pre-allocated VMA array.
```c
struct vma {
  uint64 addr;
  uint64 length;
  char shared;
  char readable;
  char writeable;
  struct file *ofile;
  uint64 offset;
};

struct proc {
  //...
  struct vma mvma[NVMA];       // mapped vma
};
};
```

In mmap, a free address area is find below all allocated VMA. Then an empty VMA entry init with the address,
increase file ref and check permission.
```c
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
```

The actual page allocation is invoked by page fault. According to the fault address, we find VMA
```c
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
```
Then validate the read or write operation with VMA permission. Allocate and map a physical page and read file content from inode.
```c
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
```

Assume munmap is made from low to high.
munmap is divided into 3 parts:
1. update mapped address and length.
2. free and unmap allocated pages. If mapping is shared and writable, write back one page content.
3. If the VMA is all cleared, close the file.

```c
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
```

In exit, just call unmap
```c
  for(int i=0; i<NVMA; i++){
    if(p->mvma[i].addr!=0){
      munmap((void*) p->mvma[i].addr, p->mvma[i].length);
    }
  }
```

In fork, iterate valid vma entry of parent process, copy vma, dup file and make mapping to new physical pages.
```c
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
```