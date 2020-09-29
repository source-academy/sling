#include <stdio.h>

#include <sinter.h>
#include <sinter/display.h>
#include <sinter/heap_obj.h>
#include <sinter/program.h>
#include <sinter/vm.h>

static void display_object_result(sinter_value_t *res, _Bool is_error) {
  if (res->type == sinter_type_array || res->type == sinter_type_function) {
    sinanbox_t arr = NANBOX_WITH_I32(res->object_value);
    sidisplay_nanbox(arr, is_error);
  }
}

void print_result(sinter_value_t *result) {
  switch (result->type) {
  case sinter_type_undefined:
    printf("undefined");
    break;
  case sinter_type_null:
    printf("null");
    break;
  case sinter_type_boolean:
    printf("%s", result->boolean_value ? "true" : "false");
    break;
  case sinter_type_integer:
    printf("%d", result->integer_value);
    break;
  case sinter_type_float:
    printf("%f", result->float_value);
    break;
  case sinter_type_string:
    printf("%s", result->string_value);
    break;
  case sinter_type_array:
  case sinter_type_function:
    display_object_result(result, false);
    break;
  default:
    printf("(unable to print value)");
    break;
  }
}
