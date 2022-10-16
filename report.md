Large files
-------

bmap is function map the file block index to the block address.
Suppose file block index is `x`, desired block address is `y`, inode has address array `addr[NDIRECT+2]`,
`NINDIRECT` is the total mapped blocks by one indirect entry, we can deduce the following equation:
```c
if 0<=x<NDIRECT, y = addr[x]
if NDIRECT<=x<NDIRECT+NINDIRECT, y = addr[NDIRECT][x-NDIRECT]
if NDIRECT+NINDIRECT<=x<NDIRECT+NINDIRECT+NINDIRECT^2, offset = x-(NDIRECT+NINDIRECT)
y = addr[NDIRECT+1][offset/NINDIRECT][offset%NINDIRECT]
```


Symbolic links
-------

Create a symbolic file with the target path content.

```c
uint64
sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH];

  if(argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0){
    return -1;
  }

  begin_op();
  struct inode* ip = create(path, T_SYMLINK, 0, 0);
  if(ip==0){
    end_op();
    return -1;
  }
  writei(ip, 0, (uint64) target, 0, MAXPATH);
  iunlockput(ip);
  end_op();
  return 0;
}
```

In sys_open, we need to exam whether the file is symbolic and no NO_FOLLOW flag, so that we need to track the link until find the target file.
```c
sys_open:
  // ... get the ilock of path
  
  if(ip->type == T_SYMLINK && !(omode & O_NOFOLLOW)){
    for(int i=0; i<10&&ip->type==T_SYMLINK; i++){
      readi(ip, 0, (uint64) path, 0, MAXPATH);
      iunlockput(ip); // no need link file
      if((ip = namei(path)) == 0){
        end_op();
        return -1;
      }
      ilock(ip);
    }
    if(ip->type == T_SYMLINK){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }
  
  // ... return file descriptor
```
