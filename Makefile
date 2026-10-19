CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
BUILD_DIR ?= build

C_STANDARD_FLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror
CONTROLLER_OBJECT = $(BUILD_DIR)/charging-controller.o
CONTROLLER_LIBRARY = $(BUILD_DIR)/libcharging-controller.a
TEST_BINARY = $(BUILD_DIR)/charging-controller-test

.PHONY: all test sanitize clean
.DELETE_ON_ERROR:

all: $(CONTROLLER_LIBRARY)

$(BUILD_DIR):
	mkdir -p "$@"

$(CONTROLLER_OBJECT): src/charging-controller.c src/charging-controller.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(C_STANDARD_FLAGS) -Isrc -c "$<" -o "$@"

$(CONTROLLER_LIBRARY): $(CONTROLLER_OBJECT)
	$(AR) rcs "$@" "$<"

$(TEST_BINARY): test/charging-controller.test.c src/charging-controller.h $(CONTROLLER_LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(C_STANDARD_FLAGS) -Isrc "$<" "$(CONTROLLER_LIBRARY)" $(LDFLAGS) $(LDLIBS) -o "$@"

test: $(TEST_BINARY)
	"$(TEST_BINARY)"

sanitize:
	$(MAKE) BUILD_DIR="$(BUILD_DIR)/sanitized" CFLAGS="$(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" test

clean:
	$(RM) "$(CONTROLLER_OBJECT)" "$(CONTROLLER_LIBRARY)" "$(TEST_BINARY)"