#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <sinter.h>

#include "common.h"
#include "../../common/sling_sinter.h"

#ifdef SLING_SINTERHOST_CUSTOM
#include SLING_SINTERHOST_CUSTOM
#endif

static bool from_sling = false;

static char sinter_heap[0x400000];

static char display_buf[0x1000];
static size_t display_buf_index = 0;
static bool display_buf_fragmented = false;

static unsigned char *program = NULL;
static size_t program_size = 0;

static void send_ipc_message(sinter_value_t *value, enum sling_message_display_type type) {
  size_t message_size = 0;
  struct sling_message_display *result_message = sling_sinter_value_to_message(value, &message_size);
  if (!result_message) {
    _Exit(child_exit_malloc_fail);
  }
  result_message->display_type = type;

  ssize_t sendres = send(IPC_FD, result_message, message_size, 0);
  if (sendres == -1) {
    _Exit(child_exit_ipc_fail);
  }
  free(result_message);
}

static inline enum sling_message_display_type print_type(bool is_error) {
  return is_error
    ? sling_message_display_type_error
    : sling_message_display_type_output;
}

__attribute__((format(printf, 2, 3))) static bool printf_buf(bool is_error, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const size_t can_write = sizeof(display_buf) - display_buf_index;
  int written = vsnprintf(display_buf + display_buf_index, can_write, format, args);
  va_end(args);
  if (written >= 0 && (size_t)written < can_write) {
    display_buf_index += written;
  } else if ((size_t)written >= can_write) {
    // send off this part first
    display_buf_fragmented = true;
    display_buf[display_buf_index] = '\0';
    sinter_value_t value = { .type = sinter_type_string, .string_value = display_buf };
    send_ipc_message(&value, print_type(is_error));

    display_buf_index = 0;
    if ((size_t)written < sizeof(display_buf)) {
      // now repeat the printf, if it can fit in the buffer
      va_start(args, format);
      int written = vsnprintf(display_buf, sizeof(display_buf), format, args);
      va_end(args);
      display_buf_index += written;
    } else {
      // give up, just send the value as-is
      return false;
    }
  }

  return true;
}

static void print_string(const char *str, bool is_error) {
  if (!printf_buf(is_error, "%s", str)) {
    sinter_value_t value = { .type = sinter_type_string, .string_value = str };
    send_ipc_message(&value, print_type(is_error));
  }
}

static void print_integer(int32_t intv, bool is_error) {
  if (!printf_buf(is_error, "%"PRId32, intv)) {
    sinter_value_t value = { .type = sinter_type_integer, .integer_value = intv };
    send_ipc_message(&value, print_type(is_error));
  }
}

static void print_float(float floatv, bool is_error) {
  if (!printf_buf(is_error, "%f", floatv)) {
    sinter_value_t value = { .type = sinter_type_float, .float_value = floatv };
    send_ipc_message(&value, print_type(is_error));
  }
}

static void print_flush(bool is_error) {
  if (display_buf_fragmented) {
    struct sling_message_display_flush flush_message = {
      .message_type = sling_message_display_type_flush
    };
    ssize_t sendres = send(IPC_FD, &flush_message, sizeof(flush_message), 0);
    if (sendres == -1) {
      _Exit(child_exit_ipc_fail);
    }
    display_buf_fragmented = false;
    display_buf_index = 0;
    return;
  }

  sinter_value_t value = { .type = sinter_type_string, .string_value = display_buf };
  send_ipc_message(&value, print_type(is_error) | sling_message_display_type_self_flushing);
  display_buf_index = 0;
}

void read_program(const char *filename) {
  FILE *input_file = fopen(filename, "rb");
  if (!input_file) {
    _Exit(child_exit_program_read_fail);
  }

  size_t alloc_size = 0x1000, read_size = 0;
  program = malloc(alloc_size);
  if (!program) {
    _Exit(child_exit_malloc_fail);
  }

  while (true) {
    if (read_size + 0x100 >= alloc_size) {
      alloc_size += 0x1000;
      program = realloc(program, alloc_size);
    }

    size_t this_read = fread(program + read_size, 1, alloc_size - read_size, input_file);
    if (this_read > 0) {
      read_size += this_read;
    } else if (feof(input_file)) {
      program_size = read_size;
      program = realloc(program, program_size);
      return;
    } else {
      _Exit(child_exit_program_read_fail);
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    return child_exit_unknown_error;
  }

  from_sling = argc >= 3 && !strcmp("--from-sling", argv[1]);

  read_program(from_sling ? argv[2] : argv[1]);

  sinter_setup_heap(sinter_heap, sizeof(sinter_heap));

  sinter_printer_string = print_string;
  sinter_printer_integer = print_integer;
  sinter_printer_float = print_float;
  sinter_printer_flush = print_flush;

#ifdef SLING_SINTERHOST_PRERUN
#include SLING_SINTERHOST_PRERUN
#endif

  sinter_value_t value = { 0 };
  sinter_fault_t result = sinter_run(program, program_size, &value);

  if (result != sinter_fault_none) {
    value.type = sinter_type_string;
    value.string_value = "Runtime error";
  }

  send_ipc_message(&value, (result == sinter_fault_none ? sling_message_display_type_result
                                                        : sling_message_display_type_error) |
                               sling_message_display_type_self_flushing);

  return 0;
}
