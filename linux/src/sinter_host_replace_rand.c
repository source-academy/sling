#include <stdlib.h>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <sinter/internal_fn.h>
#include <sinter/nanbox.h>

static sinanbox_t linux_math_random(uint8_t argc, sinanbox_t *argv) {
  (void)argc;
  (void)argv;
  return NANBOX_OFFLOAT((float)drand48());
}

void setup_linux_rand(void) {
  unsigned short seed16v[3];
  int urandom = open("/dev/urandom", O_RDONLY);
  if (urandom == -1 || read(urandom, seed16v, sizeof(seed16v)) < (long)sizeof(seed16v)) {
    // seeding from urandom failed, seed using time instead
    // uninitialised: if clock_gettime fails we seed using random stack garbage :-)
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    srand48(t.tv_sec ^ t.tv_nsec);
  } else {
    seed48(seed16v);
  }
  sivmfn_primitives[0x3a] = linux_math_random;
}
