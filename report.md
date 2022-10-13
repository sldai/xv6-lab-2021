### parallel kmem

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

### parallel bcache

Contention of bcache lock can be reduced by split bufs into different lists.
This leads to a hash table, where each bucket is a chain of bufs.

```c
struct entry {
  struct buf* buf;
  struct entry *next;
};
struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct entry entry[NBUF];
#define buf2entry(b) (bcache.entry+(b-bcache.buf))
#define entry2buf(e) (bcache.buf+(e-bcache.entry))
#define NBUCKET 13
#define hash(n) (n % NBUCKET)
  struct entry *table[NBUCKET];
  struct spinlock bklock[NBUCKET];
} bcache;

static void insert(struct entry *table[NBUCKET], int bucket, struct entry* ent)
{
  ent->next = table[bucket];
  table[bucket] = ent;
}
static struct entry* remove_recur(struct entry* head, struct entry *ent)
{
  if (head==0) return 0;
  if (head==ent) return head->next;
  head->next = remove_recur(head->next, ent);
  return head;
}
static void remove(struct entry *table[NBUCKET], int bucket, struct entry* ent)
{
  table[bucket] = remove_recur(table[bucket], ent);
}

void
binit(void)
{

  initlock(&bcache.lock, "bcache.evict");
  for (int i = 0; i < NBUCKET; ++i) {
    initlock(&bcache.bklock[i], "bcache.bucket");
  }

  for (uint i=0; i < NBUF; ++i){
    struct buf *b = &bcache.buf[i];
    initsleeplock(&b->lock, "buffer");
    b->blockno = i;
    struct entry *e = buf2entry(b);
    e->buf = b;
    insert(bcache.table, hash(b->blockno), e);
  }
}
```

Each bucket has its own lock. One thread need hold the lock according to the target blockno.
If look up success, release the lock and return. 
Otherwise, we need evict LRU buf.

The hard part in eviction is that we may remove one buf, LRU, from one bucket, and insert it into another bucket, which means holding two bucket locks and potential dead lock.
Take an example: thread a hold bucket lock 0, and want to access bucket 1, thread b does the reverse.
```text
thread a         thread b
   |                |
   |                |
lock 0           lock 1
   |                |
   |                |
lock 1           lock 0
```

To solve this problem, we need the global bcache evict lock. And make sure that it's the first lock in bget.
To achieve that, the normal lookup process and evict process run independently.

```c
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf* res= lookup(dev, blockno);
  if (!res) res = evict(dev, blockno);
  acquiresleep(&res->lock);
  return res;
}
```

As just saied, if lookup failed, all lock are released, nothing changes, then evict runs without knowing a lookup already happens.
```c
static struct buf*
lookup(uint dev, uint blockno)
{
  int bkind = hash(blockno);
  acquire(&bcache.bklock[bkind]);
  // Is the block already cached?
  for(struct entry *ent = bcache.table[bkind]; ent != 0; ent = ent->next){
    if(ent->buf->dev == dev && ent->buf->blockno == blockno){
      ent->buf->refcnt++;
      release(&bcache.bklock[bkind]);
      return ent->buf;
    }
  }
  release(&bcache.bklock[bkind]);
  return 0;
}

static struct buf*
evict(uint dev, uint blockno)
{
  acquire(&bcache.lock); // only one eviction at a time

  // check potential update
  struct buf* res;
  res = lookup(dev, blockno);
  if (res) {
    release(&bcache.lock);
    return res;
  }

  // find buf to evict and hold the bucket lock
  struct buf* evict;
  for(;;){
    // The infinite loop is used to handle case that:
    // After find the LRU and hold the lock, that buf is referred by another thread.
    // So we need redo the process, until we own LRU.

    int lru = -1;
    uint64 lru_tick = -1;
    for (int i = 0; i < NBUF; ++i) {
      if (bcache.buf[i].refcnt == 0 && bcache.buf[i].last_used < lru_tick) {
        lru = i;
        lru_tick = bcache.buf[i].last_used;
      }
    }
    if (lru == -1)  panic("bget: no buffers");

    evict = &bcache.buf[lru];
    int lru_bkind = hash(evict->blockno);
    acquire(&bcache.bklock[lru_bkind]);
    if (evict->refcnt == 0) break; // LRU is valid
    release(&bcache.bklock[lru_bkind]);
  }

  int old_bkind = hash(evict->blockno);
  int new_bkind = hash(blockno);
  evict->dev = dev;
  evict->blockno = blockno;
  evict->valid = 0;
  evict->refcnt = 1;
  if (old_bkind != new_bkind) {
    acquire(&bcache.bklock[new_bkind]);
    remove(bcache.table, old_bkind, buf2entry(evict));
    insert(bcache.table, new_bkind, buf2entry(evict));
    release(&bcache.bklock[new_bkind]);
  }
  release(&bcache.bklock[old_bkind]);
  release(&bcache.lock);
  return evict;
}
```

When evict begins, it holds the global lock. Then it must check whether target buf exists.
Then it search the oldest used free buf. Finally, it updates the buf contents and hash table.

If you inspect closely, if we only use evict in bget, it still works, because evict just works independently.
```c
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf* res = evict(dev, blockno);
  acquiresleep(&res->lock);
  return res;
}
```
We can think evict is a strict sequential bcache alloc process, it is as correct as the original xv6 bget.
The optimization is introduced by successful lookup ahead, while failure case has no side effect and fall back to evict.