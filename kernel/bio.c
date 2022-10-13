// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

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

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf* res= lookup(dev, blockno);
  if (!res) res = evict(dev, blockno);
  acquiresleep(&res->lock);
  return res;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");
  b->last_used = ticks;
  releasesleep(&b->lock);

  acquire(&bcache.bklock[hash(b->blockno)]);
  b->refcnt--;
  release(&bcache.bklock[hash(b->blockno)]);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


