#ifndef SLING_MESSAGE_H
#define SLING_MESSAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define SLING_INTOPIC_RUN "run"
#define SLING_INTOPIC_STOP "stop"
#define SLING_INTOPIC_PING "ping"
#define SLING_INTOPIC_INPUT "input"

#define SLING_OUTTOPIC_STATUS "status"
#define SLING_OUTTOPIC_DISPLAY "display"

enum sling_message_display_type {
  sling_message_display_type_output = 0,
  sling_message_display_type_error = 1,
  sling_message_display_type_result = 2,
  sling_message_display_type_prompt = 3,
  sling_message_display_type_prompt_response = 4
};

struct __attribute__((packed)) sling_message_display {
  uint16_t message_type;
  uint16_t data_type;
  union {
    bool boolean;
    int32_t int32;
    float float32;
    // Excluding null terminator
    uint32_t string_length;
  };
  char string[];
};
_Static_assert(sizeof(struct sling_message_display) == 8, "Wrong sling_output size");

static inline char *sling_topic(const char *device_id, const char *topic) {
  char *ret = NULL;
  if (asprintf(&ret, "%s/%s", device_id, topic) == -1) {
    return NULL;
  }
  return ret;
}

#endif
