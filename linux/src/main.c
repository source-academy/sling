#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/prctl.h>

#include <mosquitto.h>

#include "../../common/sling_message.h"

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

enum device_state {
  device_state_undefined = 0,
  device_state_idle = 1,
  device_state_running = 2
};

struct sling_config {
  const char *host;
  const char *device_id;
  const char *server_ca_path;
  const char *client_key_path;
  const char *client_cert_path;

  int port;

  // Precomputed topic names

  char *outtopic_status;
  char *outtopic_display;

  char *intopic_run;
  char *intopic_stop;
  char *intopic_ping;
  char *intopic_input;

  size_t intopic_index;

  enum device_state state;
  int epollfd;
  int ipcfd;
};

enum main_loop_epoll_type {
  main_loop_epoll_mosq,
  main_loop_epoll_child,
  main_loop_epoll_ipc
};

static void main_loop_epoll_add(enum main_loop_epoll_type type, int fd);
static void change_state(enum device_state new_state);

static struct mosquitto *mosq;
static struct sling_config config;

#define fatal_error(...) do { fprintf(stderr, __VA_ARGS__); _Exit(1); } while (0)

#define fatal_errno(msg) do { perror(msg); _Exit(1); } while (0)

static void check_mosq(int error) {
  switch (error) {
  case MOSQ_ERR_SUCCESS:
    return;
  case MOSQ_ERR_ERRNO:
    perror("Mosquitto");
    _Exit(1);
  default:
    eprintf("Mosquitto: %s\n", mosquitto_strerror(error));
    _Exit(1);
  }
}

static int check_posix(int result, const char *msg) {
  if (result == -1) {
    fatal_errno(msg);
  }
  return result;
}

static int check_posix_nonblock(int result, const char *msg) {
  if (result == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
    fatal_errno(msg);
  }
  return result;
}

static void print_usage(char *argv0) {
  eprintf("Usage: %s <options>\n%s", argv0,
    "-h, --host, SLING_HOST:           The hostname of the MQTT server\n"
    "-p, --port, SLING_PORT:           The port of the MQTT server; defaults to 8883\n"
    "-i, --device-id, SLING_DEVICE_ID: The device ID\n"
    "-s, --server-ca, SLING_CA:        Path to the CA issuing the MQTT server's TLS certificate, in PEM format\n"
    "-k, --client-key, SLING_KEY:      Path to the private key for the client's TLS certificate, in PEM format\n"
    "-c, --client-cert, SLING_CERT:    Path to the client's TLS certificate, in PEM format\n"
    "\n"
    "Options can be passed in via environment variables. Command line options override environment variables.\n"
    "\n"
    "When specifying a boolean as an environment variable, specify 1 for true.\n"
  );
}

// TODO configurable
static char *program_filename = "program.svm";

static void begin_run_program(const char *program, size_t program_size) {
  FILE *program_file = fopen(program_filename, "w");
  if (!program_file) {
    check_posix(-1, "program file fopen");
  }
  if (!fwrite(program, program_size, 1, program_file)) {
    fatal_error("Failed to write program file");
  }
  check_posix(fclose(program_file), "program file fclose");

  int sv[2];
  check_posix(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), "socketpair");

  pid_t child_pid = check_posix(fork(), "fork");
  if (child_pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    dup2(sv[1], 998);
    close(sv[0]);
    close(sv[1]);

    check_posix(
      execl("./sinter_host",
        "./sinter_host", "--from-sling", program_filename, (char *) NULL),
      "exec sinter host");

    _Exit(1);
  }
  close(sv[1]);

  config.ipcfd = sv[0];
  change_state(device_state_running);
  main_loop_epoll_add(main_loop_epoll_ipc, config.ipcfd);
}

static void on_log(struct mosquitto *mosq, void *obj, int level, const char *message) {
  (void) mosq; (void) obj;
  eprintf("[%d]: %s\n", level, message);
}

static void on_connect(struct mosquitto *mosq, void *obj, int ret) {
  (void) obj;
  if (ret) {
    fatal_error("Failed to connect: %d\n", ret);
  }

  mosquitto_subscribe(mosq, NULL, config.intopic_run, 0);
  mosquitto_subscribe(mosq, NULL, config.intopic_stop, 0);
  mosquitto_subscribe(mosq, NULL, config.intopic_ping, 0);
  mosquitto_subscribe(mosq, NULL, config.intopic_input, 0);
}

static void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message) {
  (void) mosq; (void) obj;
  // hack: the 4 topics we subscribe to all start with <device id>/, and then
  // the first letters of the message types are unique, so we only check those
  if (config.intopic_index >= strlen(message->topic)) {
    return;
  }

  switch (message->topic[config.intopic_index]) {
  case 'r': // run
    if (config.state == device_state_idle) {
      begin_run_program(message->payload, message->payloadlen);
    }
    break;
  case 's': // stop
    break;
  case 'p': // ping
    break;
  case 'i': // input
    // TODO
    break;
  }
}

static void change_state(enum device_state new_state) {
  config.state = new_state;
  bool publish_payload = new_state == device_state_running;
  check_mosq(mosquitto_publish(mosq, NULL, config.outtopic_status, sizeof(publish_payload), &publish_payload, 0, false));
}

static int main_loop_make_sigchldfd(void) {
  sigset_t sigchldmask;
  sigemptyset(&sigchldmask);
  sigaddset(&sigchldmask, SIGCHLD);
  check_posix(sigprocmask(SIG_BLOCK, &sigchldmask, NULL), "sigprocmask");
  return check_posix(signalfd(-1, &sigchldmask, SFD_CLOEXEC | SFD_NONBLOCK), "signalfd");
}

static void main_loop_epoll_add(enum main_loop_epoll_type type, int fd) {
  struct epoll_event ev = {
    .events = EPOLLIN,
    .data = {
      .u32 = type
    }
  };

  check_posix(epoll_ctl(config.epollfd, EPOLL_CTL_ADD, fd, &ev), "epoll_ctl");
}

static int main_loop(void) {
  size_t buffer_size = 0x4000;
  char *buffer = malloc(buffer_size);
  if (!buffer) {
    fatal_error("Failed to allocate buffer.");
  }

  const size_t max_events = 3;
  struct epoll_event events[max_events];

  const int mosqfd = mosquitto_socket(mosq),
    sigchldfd = main_loop_make_sigchldfd();

  config.epollfd = check_posix(epoll_create1(EPOLL_CLOEXEC), "epoll_create1");

  if (mosqfd == -1) {
    fatal_error("Failed to get mosquitto FD.");
  }

  main_loop_epoll_add(main_loop_epoll_mosq, mosqfd);
  main_loop_epoll_add(main_loop_epoll_child, sigchldfd);

  while (1) {
    int nfds = check_posix(epoll_wait(config.epollfd, events, max_events, 1000), "epoll_wait");
    for (int n = 0; n < nfds; ++n) {
      struct epoll_event *ev = events + n;
      switch (ev->data.u32) {
      case main_loop_epoll_mosq: {
        check_mosq(mosquitto_loop_read(mosq, 1));
        break;
      }

      case main_loop_epoll_ipc: {
        ssize_t recv_size = check_posix_nonblock(recv(config.ipcfd, buffer, 0, MSG_PEEK | MSG_TRUNC), "ipc recv");
        if (recv_size == -1) {
          continue;
        }
        if ((size_t) recv_size > buffer_size) {
          buffer_size = recv_size;
          buffer = realloc(buffer, recv_size);
          if (!buffer) {
            fatal_error("Failed to allocate buffer.");
          }
        }
        recv_size = check_posix(recv(config.ipcfd, buffer, buffer_size, 0), "ipc recv");
        check_mosq(mosquitto_publish(mosq, NULL, config.outtopic_display, recv_size, buffer, 0, false));
        break;
      }

      case main_loop_epoll_child: {
        while (read(sigchldfd, buffer, buffer_size) >= 0) {
          // do nothing, just clear it
        }
        while (waitpid(-1, NULL, WNOHANG) > 0) {
          // TODO check the pid perhaps
          // (now we just clean up the child i guess)
        }
        close(config.ipcfd);
        config.ipcfd = -1;
        change_state(device_state_idle);
        break;
      }

      }
    }

    check_mosq(mosquitto_loop_write(mosq, 1));
    check_mosq(mosquitto_loop_misc(mosq));
  }
}

static int read_env_int(const char *name, int def) {
  const char *val = getenv(name);
  return val ? atoi(val) : def;
}

int main(int argc, char *argv[]) {
  config.state = device_state_idle;
  config.ipcfd = config.epollfd = -1;
  config.host = getenv("SLING_HOST");
  config.port = read_env_int("SLING_PORT", 0);
  config.device_id = getenv("SLING_DEVICE_ID");
  config.server_ca_path = getenv("SLING_CA");
  config.client_key_path = getenv("SLING_KEY");
  config.client_cert_path = getenv("SLING_CERT");

  while (1) {
    static struct option long_options[] = {
      {"host",        required_argument, 0, 'h' },
      {"port",        required_argument, 0, 'p' },
      {"use-tls",     no_argument,       0, 't' },
      {"device-id",   required_argument, 0, 'i' },
      {"server-ca",   required_argument, 0, 's' },
      {"client-key",  required_argument, 0, 'k' },
      {"client-cert", required_argument, 0, 'c' },
      {"help",        no_argument,       0, 0   },
      {0,             0,                 0, 0   }
    };

    int c = getopt_long(argc, argv, "h:p:i:s:k:c:", long_options, NULL);
    if (c == -1) {
      break;
    }

    switch (c) {
    case 'h':
      config.host = optarg;
      break;
    case 'p':
      config.port = atoi(optarg);
      break;
    case 'i':
      config.device_id = optarg;
      break;
    case 's':
      config.server_ca_path = optarg;
      break;
    case 'k':
      config.client_key_path = optarg;
      break;
    case 'c':
      config.client_cert_path = optarg;
      break;
    case '?':
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  bool fail = false;
  if (!config.host) {
    eprintf("No hostname specified.\n");
    fail = true;
  }
  if (!config.device_id) {
    eprintf("No device ID specified.\n");
    fail = true;
  }
  if (!config.server_ca_path) {
    eprintf("No server CA specified.\n");
    fail = true;
  }
  if (!config.client_key_path) {
    eprintf("No private key specified.\n");
    fail = true;
  }
  if (!config.client_cert_path) {
    eprintf("No certificate specified.\n");
    fail = true;
  }
  if (fail) {
    return 1;
  }

  if (config.port == 0) {
    config.port = 8883;
  }

  config.outtopic_display = sling_topic(config.device_id, SLING_OUTTOPIC_DISPLAY);
  config.outtopic_status = sling_topic(config.device_id, SLING_OUTTOPIC_STATUS);

  config.intopic_input = sling_topic(config.device_id, SLING_INTOPIC_INPUT);
  config.intopic_ping = sling_topic(config.device_id, SLING_INTOPIC_PING);
  config.intopic_run = sling_topic(config.device_id, SLING_INTOPIC_RUN);
  config.intopic_stop = sling_topic(config.device_id, SLING_INTOPIC_STOP);

  config.intopic_index = strlen(config.device_id) + 1;

  check_mosq(mosquitto_lib_init());
  mosq = mosquitto_new(config.device_id, true, NULL);
  if (!mosq) {
    fatal_error("Mosquitto instance initialisation failed.\n");
  }
  mosquitto_log_callback_set(mosq, on_log);
  mosquitto_connect_callback_set(mosq, on_connect);
  mosquitto_message_callback_set(mosq, on_message);
  mosquitto_tls_set(mosq, config.server_ca_path, NULL, config.client_cert_path, config.client_key_path, NULL);
  check_mosq(mosquitto_connect(mosq, config.host, config.port, 30));

  main_loop();

  mosquitto_destroy(mosq);

  return 0;
}
