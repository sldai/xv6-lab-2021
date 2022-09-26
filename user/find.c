#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "user/user.h"

char*
fmtname(char *path)
{
  static char buf[DIRSIZ+1];
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  if(strlen(p) >= DIRSIZ)
    return p;
  memmove(buf, p, strlen(p));
  memset(buf+strlen(p), '\0', DIRSIZ-strlen(p));
  return buf;
}

void find(char* name, char* path) {
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if((fd = open(path, 0)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
    case T_DEVICE:
    case T_FILE:
      if (strcmp(fmtname(path), name)==0) {
        printf("%s\n", path);
      }
      break;

    case T_DIR:
      if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
        printf("ls: path too long\n");
        break;
      }
      strcpy(buf, path);
      p = buf+strlen(buf);
      *p++ = '/';
      while(read(fd, &de, sizeof(de)) == sizeof(de)){
        if(de.inum == 0)
          continue;
        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;
        if(strcmp(p, ".")==0 || strcmp(p, "..")==0){
          continue;
        }
        find(name, buf);
      }
      break;
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  if (argc!=2 && argc!=3) {
    const char* err = "Usage: find <dir> <file>";
    write(1, err, strlen(err));
    exit(-1);
  }

  char* name;
  char* path;
  if (argc==2) {
    name = argv[1];
    path = ".";
  } else {
    name = argv[2];
    path = argv[1];
  }

  find(name, path);
  exit(0);
}
