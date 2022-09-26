#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

char buf[1024];

int
append_arg(char* _argv[], int _argc)
{
  int n, m;
  char *p, *q;

  m = 0;
  while((n = read(0, buf+m, sizeof(buf)-m-1)) > 0){
    m += n;
    buf[m] = '\0';
    p = buf;
    while((q = strchr(p, '\n')) != 0){
      *q = 0;
      char* new_arg = malloc(q-p+1);
      strcpy(new_arg, p);
      _argv[_argc] = new_arg;
      _argc++;
      p = q+1;
    }
    if(m > 0){
      m -= p - buf;
      memmove(buf, p, m);
    }
  }
  return _argc;
}

int
main(int argc, char *argv[])
{
  if (argc<2) {
    const char* err = "Usage: xargs program ...args";
    write(1, err, strlen(err));
    exit(-1);
  }

  char* _argv[MAXARG];
  int _argc = argc-1;
  for (int i = 0; i < _argc; ++i) {
    _argv[i] = argv[i+1];
  }
  _argc = append_arg(_argv, _argc);
  if (fork()==0) {
    exec(_argv[0], _argv);
  } else {
    wait(0);
    for (int i = 0; i < _argc; ++i) {
      free(_argv[i]);
    }
    exit(0);
  }
  exit(0);
}
