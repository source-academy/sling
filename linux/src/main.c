#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mosquitto.h>

#include "../../common/sling_message.h"

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

typedef enum sling_message_status_type device_status_t;

struct sling_config {
  const char *host;
  const char *device_id;
  const char *server_ca_path;
  const char *server_ca_dir;
  const char *client_key_path;
  const char *client_cert_path;
  const char *sinter_host_path;
  const char *program_path;

  int port;

  // Precomputed topic names

  char *outtopic_status;
  char *outtopic_display;
  char *outtopic_hello;
  char *outtopic_monitor;

  char *intopic_run;
  char *intopic_stop;
  char *intopic_ping;
  char *intopic_input;

  size_t intopic_index;

  device_status_t status;
  pid_t host_pid;
  int epollfd;
  int ipcfd;

  FILE *urandom;

  uint32_t message_counter;
  uint32_t display_start_counter;
  uint32_t last_display_flush_counter;
  uint32_t monitor_start_counter;
//  uint32_t last_monitor_flush_counter;

// MUST BE POWER OF 2
#define LAST_MESSAGE_ID_BUF_SIZE 4

  uint32_t last_message_ids[4];
  size_t last_message_id_index;
};

enum main_loop_epoll_type {
  main_loop_epoll_mosq,
  main_loop_epoll_child,
  main_loop_epoll_ipc
};

static void main_loop_epoll_add(enum main_loop_epoll_type type, int fd);
static void send_status(void);
static void send_hello_if_zero(void);
static void change_status(device_status_t new_status);

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
  eprintf("Usage: %s <options>\n\n%s", argv0,
    "  -v, --debug:                         Log verbosely\n"
    "  -h, --host, SLING_HOST:              The hostname of the MQTT server\n"
    "  -p, --port, SLING_PORT:              The port of the MQTT server; defaults to 8883\n"
    "  -i, --device-id, SLING_DEVICE_ID:    The device ID\n"
    "  -s, --server-ca, SLING_CA:           Path to the CA issuing the MQTT server's TLS certificate, in PEM format\n"
    "  -k, --client-key, SLING_KEY:         Path to the private key for the client's TLS certificate, in PEM format\n"
    "  -c, --client-cert, SLING_CERT:       Path to the client's TLS certificate, in PEM format\n"
    "  -H, --sinter-host, SINTER_HOST_PATH: Path to the Sinter host, or ./sinter_host by default\n"
    "  -P, --program, SLING_PROGRAM_PATH:   Path to the location at which to save received programs, or ./program.svm by default\n"
    "\n"
    "Options can be passed in via environment variables. Command line options override environment variables.\n"
    "\n"
    "When specifying a boolean as an environment variable, specify 1 for true.\n"
  );
}

static void begin_run_program(const char *program, size_t program_size) {
  if (config.status != sling_message_status_type_idle) {
    send_status();
    return;
  }

  FILE *program_file = fopen(config.program_path, "w");
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
      execl(config.sinter_host_path,
        config.sinter_host_path, "--from-sling", config.program_path, (char *) NULL),
      "exec sinter host");

    _Exit(1);
  } else if (child_pid > 0) {
    close(sv[1]);

    config.host_pid = child_pid;
    config.ipcfd = sv[0];
    fcntl(config.ipcfd, F_SETFL, O_NONBLOCK);
    change_status(sling_message_status_type_running);
    main_loop_epoll_add(main_loop_epoll_ipc, config.ipcfd);
  } else {
    fatal_errno("fork");
    change_status(sling_message_status_type_idle);
  }
}

static void stop_program(void) {
  if (config.status == sling_message_status_type_idle || config.host_pid <= 0) {
    send_status();
    return;
  }

  kill(config.host_pid, SIGTERM);
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

  send_hello_if_zero();
  send_status();
  mosquitto_subscribe(mosq, NULL, config.intopic_run, 1);
  mosquitto_subscribe(mosq, NULL, config.intopic_stop, 1);
  mosquitto_subscribe(mosq, NULL, config.intopic_ping, 1);
  mosquitto_subscribe(mosq, NULL, config.intopic_input, 1);
}

static void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message) {
  (void) mosq; (void) obj;
  // hack: the 4 topics we subscribe to all start with <device id>/, and then
  // the first letters of the message types are unique, so we only check those
  if (config.intopic_index >= strlen(message->topic) || message->payloadlen < 4) {
    return;
  }

  const uint32_t message_id = *(uint32_t *)message->payload;
  for (size_t i = 0; i < LAST_MESSAGE_ID_BUF_SIZE; ++i) {
    if (message_id == config.last_message_ids[i]) {
      return;
    }
  }
  config.last_message_ids[config.last_message_id_index] = message_id;
  config.last_message_id_index = (config.last_message_id_index + 1) & (LAST_MESSAGE_ID_BUF_SIZE - 1);

  switch (message->topic[config.intopic_index]) {
  case 'r': // run
    begin_run_program((const char *)message->payload + 4, message->payloadlen - 4);
    break;
  case 's': // stop
    stop_program();
    break;
  case 'p': // ping
    send_status();
    break;
  case 'i': // input
    // TODO
    break;
  }
}

static void send_hello_if_zero(void) {
  if (config.message_counter != 0) {
    return;
  }
  config.message_counter++;
  uint32_t payload[2] = { 0 };
  fread(payload + 1, sizeof(uint32_t), 1, config.urandom);
  check_mosq(mosquitto_publish(mosq, NULL, config.outtopic_hello, sizeof(payload), payload, 1, false));
}

static void send_status(void) {
  send_hello_if_zero();
  struct sling_message_status publish_payload = {
    .message_counter = config.message_counter++,
    .status = config.status
  };
  check_mosq(mosquitto_publish(mosq, NULL, config.outtopic_status, sizeof(publish_payload), &publish_payload, 1, false));
}

static void change_status(device_status_t new_state) {
  config.status = new_state;
  send_status();
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

static void get_peripherals() {
  FILE *fp;
  char buffer[256];

  /* Read motors */
  // for f in /sys/class/lego-sensor/*; do cat $f/{address,driver_name,mode,value0}; done
//  fp = popen("for f in /sys/class/tacho-motor/*; do cat $f/{address,driver_name,position,speed}; done", "r");
  fp = popen("for f in /sys/class/tacho-motor/*; do cat $f/{address,driver_name,position,speed}; done; for f in /sys/class/lego-sensor/*; do cat $f/{address,driver_name,mode,value0}; done", "r");
  if (fp == NULL) {
    return;
  }

  /* Read the output a line at a time. */
  uint8_t line_count = 0;
  while (fgets(buffer, sizeof(buffer), fp) != NULL) {
//    printf("%s", path);

    ++line_count;

    uint32_t recv_size = strlen(buffer);

    struct sling_message_monitor *to_send = (struct sling_message_monitor *) buffer;
    to_send->message_counter = config.message_counter;
    to_send->string_length = recv_size;

    if (line_count == 1) {
      config.monitor_start_counter = config.message_counter;
    }

    ++config.message_counter;
    check_mosq(mosquitto_publish(mosq, NULL, config.outtopic_monitor, recv_size, buffer, 1, false));

    if (line_count == 4) { // flush - expect 4 lines for each motor
      // TODO
      struct sling_message_monitor_flush *to_send = {0};
      to_send->message_counter = config.message_counter;
      to_send->starting_id = config.monitor_start_counter;

      check_mosq(mosquitto_publish(mosq, NULL, config.outtopic_monitor, recv_size, buffer, 1, false));

      ++config.message_counter;
      line_count = 0;
    }
  }

  /* close */
  pclose(fp);
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
    get_peripherals();

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
        if (recv_size == -1 || (size_t)recv_size < sizeof(struct sling_message_display_flush)) {
          // sanity check - skip if recv failed or message is smaller than expected
          continue;
        }

        struct sling_message_display *to_send = (struct sling_message_display *) buffer;
        send_hello_if_zero();
        to_send->message_counter = config.message_counter;
        if (to_send->display_type == sling_message_display_type_flush) {
          if (config.display_start_counter >= to_send->message_counter) {
            // skip empty flush
            continue;
          }
          struct sling_message_display_flush *to_send_flush = (struct sling_message_display_flush *) buffer;
          to_send_flush->starting_id = config.display_start_counter;
          config.last_display_flush_counter = to_send->message_counter;
          recv_size = sizeof(*to_send_flush);
        } else if (config.display_start_counter <= config.last_display_flush_counter) {
          config.display_start_counter = to_send->message_counter;
        }

        if (to_send->display_type & sling_message_display_type_self_flushing) {
          config.last_display_flush_counter = to_send->message_counter;
        }

        ++config.message_counter;
        check_mosq(mosquitto_publish(mosq, NULL, config.outtopic_display, recv_size, buffer, 1, false));
        break;
      }

      case main_loop_epoll_child: {
        if (recv(config.ipcfd, buffer, 0, MSG_PEEK | MSG_TRUNC) > 0) {
          // don't handle the child exit yet
          break;
        }

        while (read(sigchldfd, buffer, buffer_size) >= 0) {
          // do nothing, just clear it
        }
        while (waitpid(-1, NULL, WNOHANG) > 0) {
          // TODO check the pid perhaps
          // (now we just clean up the child i guess)
        }
        close(config.ipcfd);
        config.ipcfd = -1;
        change_status(sling_message_status_type_idle);
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
  bool debug_log = false;
  config.status = sling_message_status_type_idle;
  config.ipcfd = config.epollfd = -1;
  config.host = getenv("SLING_HOST");
  config.port = read_env_int("SLING_PORT", 0);
  config.device_id = getenv("SLING_DEVICE_ID");
  config.server_ca_path = getenv("SLING_CA");
  config.server_ca_dir = getenv("SLING_CA_DIR");
  config.client_key_path = getenv("SLING_KEY");
  config.client_cert_path = getenv("SLING_CERT");
  config.sinter_host_path = getenv("SINTER_HOST_PATH");
  config.program_path = getenv("SLING_PROGRAM_PATH");
  config.message_counter = config.last_display_flush_counter = config.display_start_counter = config.monitor_start_counter = 0;
//  config.message_counter = config.last_display_flush_counter = config.display_start_counter = config.monitor_start_counter = config.last_monitor_flush_counter = 0;

  while (1) {
    static struct option long_options[] = {
      {"host",        required_argument, 0, 'h' },
      {"port",        required_argument, 0, 'p' },
      {"program",     required_argument, 0, 'P' },
      // {"use-tls",     no_argument,       0, 't' },
      {"device-id",   required_argument, 0, 'i' },
      {"server-ca",   required_argument, 0, 's' },
      {"ca-dir",      required_argument, 0, 'S' },
      {"client-key",  required_argument, 0, 'k' },
      {"client-cert", required_argument, 0, 'c' },
      {"sinter-host", required_argument, 0, 'H' },
      {"debug",       no_argument,       0, 'v' },
      {"help",        no_argument,       0, 0   },
      {0,             0,                 0, 0   }
    };

    int c = getopt_long(argc, argv, "P:H:h:p:i:s:k:c:v", long_options, NULL);
    if (c == -1) {
      break;
    }

    switch (c) {
    case 'v':
      debug_log = true;
      break;
    case 'P':
      config.program_path = optarg;
      break;
    case 'H':
      config.sinter_host_path = optarg;
      break;
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
    case 'S':
      config.server_ca_dir = optarg;
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
  if (!config.client_key_path) {
    eprintf("No private key specified.\n");
    fail = true;
  }
  if (!config.client_cert_path) {
    eprintf("No certificate specified.\n");
    fail = true;
  }
  if (!config.server_ca_path && !config.server_ca_dir) {
    config.server_ca_dir = "/etc/ssl/certs";
  }
  if (!config.sinter_host_path) {
    config.sinter_host_path = "./sinter_host";
  }
  if (!config.program_path) {
    config.program_path = "program.svm";
  }
  if (fail) {
    return 1;
  }

  if (config.port == 0) {
    config.port = 8883;
  }

  config.outtopic_display = sling_topic(config.device_id, SLING_OUTTOPIC_DISPLAY);
  config.outtopic_status = sling_topic(config.device_id, SLING_OUTTOPIC_STATUS);
  config.outtopic_hello = sling_topic(config.device_id, SLING_OUTTOPIC_HELLO);
  config.outtopic_monitor = sling_topic(config.device_id, SLING_OUTTOPIC_MONITOR);

  config.intopic_input = sling_topic(config.device_id, SLING_INTOPIC_INPUT);
  config.intopic_ping = sling_topic(config.device_id, SLING_INTOPIC_PING);
  config.intopic_run = sling_topic(config.device_id, SLING_INTOPIC_RUN);
  config.intopic_stop = sling_topic(config.device_id, SLING_INTOPIC_STOP);

  config.intopic_index = strlen(config.device_id) + 1;

  config.urandom = fopen("/dev/urandom", "r");
  if (!config.urandom) {
    fatal_error("Could not open /dev/urandom.\n");
  }
  setvbuf(config.urandom, NULL, _IONBF, 0);

  check_mosq(mosquitto_lib_init());
  mosq = mosquitto_new(config.device_id, true, NULL);
  if (!mosq) {
    fatal_error("Mosquitto instance initialisation failed.\n");
  }
  if (debug_log) {
    mosquitto_log_callback_set(mosq, on_log);
  }
  mosquitto_connect_callback_set(mosq, on_connect);
  mosquitto_message_callback_set(mosq, on_message);
  mosquitto_tls_set(mosq, config.server_ca_path,
                    config.server_ca_path ? NULL : config.server_ca_dir, config.client_cert_path,
                    config.client_key_path, NULL);
  check_mosq(mosquitto_connect(mosq, config.host, config.port, 30));

  main_loop();

  mosquitto_destroy(mosq);

  return 0;
}
