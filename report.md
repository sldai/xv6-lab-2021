First we need #CPU(N) kmems
```c
struct {
  struct spinlock lock;
  struct run *freelist;
  char namebuf[10];
} kmems[NCPU];

void
kinit()
{
  for (int i = 0; i < NCPU; ++i) {
    snprintf(kmems[i].namebuf, sizeof(kmems[i].namebuf), "kmem%d", i);
    initlock(&kmems[i].lock, kmems[i].namebuf);
  }
  freerange(end, (void*)PHYSTOP);
}
```

The `freerange` init all free pages to the cpu0, other cpus will steal them when needed.

Free the page to current cpu's mem list
```c
// free the page to kmems[cpuid]
push_off();
int id = cpuid();
acquire(&kmems[id].lock);
r->next = kmems[id].freelist;
kmems[id].freelist = r;
release(&kmems[id].lock);
pop_off();
```

Try to alloc page from each mem list
```c
push_off();
int id = cpuid();
for (int offset = 0; offset < NCPU; ++offset) {
int cur_id = (id+offset)%NCPU;
acquire(&kmems[cur_id].lock);
r = kmems[cur_id].freelist;
if(r) {
  kmems[cur_id].freelist = r->next;
  release(&kmems[cur_id].lock);
  break;
}
release(&kmems[cur_id].lock);
}
pop_off();
```