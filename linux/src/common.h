#ifndef SLING_LINUX_COMMON_H
#define SLING_LINUX_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum child_exit {
  child_exit_normal = 0,
  child_exit_unknown_error = 1,
  child_exit_program_read_fail = 2,
  child_exit_malloc_fail = 3,
  child_exit_ipc_fail = 4,
};

#define IPC_FD 998

#endif
