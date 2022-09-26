#include <stdbool.h>
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"


void recur_proc(int left_pipe[2]) {

  int p=0;
  read(left_pipe[0], &p, sizeof(p));
  printf("prime %d\n", p);

  bool first_prime = true;
  int right_pipe[2];
  int n=0;
  while (read(left_pipe[0], &n, sizeof(n))) {
    if (n%p!=0) {
      if (first_prime) {
        pipe(right_pipe);
        if (fork()==0) {
          close(left_pipe[0]);
          close(right_pipe[1]);
          recur_proc(right_pipe);
          exit(0);
        }
        close(right_pipe[0]);
        write(right_pipe[1], &n, sizeof(n));
        first_prime = false;
      } else {
        write(right_pipe[1], &n, sizeof(n));
      }
    }
  }
  close(left_pipe[0]);
  if (!first_prime) {
    close(right_pipe[1]);
  }
  wait(0);
  exit(0);
}

int
main(int argc, char *argv[])
{
  int right_pipe[2];
  pipe(right_pipe);
  if (fork()==0) {
    close(right_pipe[1]);
    recur_proc(right_pipe);
    exit(0);
  } else {
    close(pipe(0));
    int p = 2;
    for (int n = p; n <= 35; ++n) {
      if (n%p!=0) {
        write(right_pipe[1], &n, sizeof(n));
      }
    }
    close(right_pipe[1]);
    wait(0);
    exit(0);
  }
}
