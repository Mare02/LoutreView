OS := $(shell uname -s)
ifeq ($(OS),Darwin)
PLATFORM := macos
PLATFORM_LIBS := -framework IOKit -framework CoreFoundation -framework CoreServices
else ifeq ($(OS),Linux)
PLATFORM := linux
PLATFORM_LIBS := -lm
# Match the empty source-level definition while enabling glibc's default API.
PLATFORM_CPPFLAGS := -D_DEFAULT_SOURCE= -D_POSIX_C_SOURCE=200809L
else
$(error Unsupported OS: $(OS); expected Darwin or Linux)
endif

ifeq ($(origin CC),default)
CC := cc
endif
CFLAGS ?= -O2
TARGET ?= loutre-view
STRICT_FLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror
INCLUDE_FLAGS := -Iinclude
# Compiler and flag changes must never reuse incompatible objects.
CONFIG := $(shell printf '%s\n' "$(CC)" "$(CPPFLAGS)" "$(PLATFORM_CPPFLAGS)" "$(CFLAGS)" "$(LDFLAGS)" "$(LDLIBS)" "$(STRICT_FLAGS)" "$(INCLUDE_FLAGS)" | cksum | awk '{print $$1}')
BUILD_DIR := build/$(OS)/$(CONFIG)
SOURCES := $(sort $(wildcard src/*.c src/ui/*.c src/usage/*.c src/providers/*.c platform/common/*.c platform/$(PLATFORM)/*.c))
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES))
LIB_OBJECTS := $(filter-out $(BUILD_DIR)/src/main.o,$(OBJECTS))
TEST_SOURCES := $(wildcard tests/test_*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SOURCES))

.PHONY: all build clean run test linux-test FORCE
all: build
build: $(TARGET)

# Relink when returning to a previously built configuration or TARGET path.
$(TARGET): $(OBJECTS) FORCE
	@mkdir -p "$(dir $(TARGET))"
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -o "$@" $(LDLIBS) $(PLATFORM_LIBS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p "$(@D)"
	$(CC) $(INCLUDE_FLAGS) $(PLATFORM_CPPFLAGS) $(CPPFLAGS) $(STRICT_FLAGS) $(CFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/tests/%: tests/%.c $(LIB_OBJECTS)
	@mkdir -p "$(@D)"
	$(CC) $(INCLUDE_FLAGS) $(PLATFORM_CPPFLAGS) $(CPPFLAGS) $(STRICT_FLAGS) $(CFLAGS) -MMD -MP -MF "$@.d" $(LDFLAGS) "$<" $(LIB_OBJECTS) -o "$@" $(LDLIBS) $(PLATFORM_LIBS)

test: $(TARGET) $(TEST_BINS)
	@set -e; for test_bin in $(TEST_BINS); do "$$test_bin"; done
	@if [ -f tests/test_installer.sh ]; then sh tests/test_installer.sh; fi
	@if [ -f tests/smoke.py ]; then python3 tests/smoke.py "$(abspath $(TARGET))"; fi

LINUX_TEST_IMAGE ?= loutre-view-linux-test:local

linux-test:
	docker build --file Dockerfile.linux-test --tag "$(LINUX_TEST_IMAGE)" .
	docker run --rm --name "loutre-view-linux-test-$$$$" \
		--label loutre-view.task=linux-test --cpus=2 --memory=2g --pids-limit=256 \
		--mount type=bind,source="$(CURDIR)",target=/source,readonly \
		"$(LINUX_TEST_IMAGE)" sh -c \
			'cp -a /source/. /work/ && cd /work && make CC=gcc test && make CC=clang clean && make CC=clang test'

run: $(TARGET)
	"$(abspath $(TARGET))"

clean:
	rm -rf build
	rm -f -- "$(TARGET)"

FORCE:
-include $(OBJECTS:.o=.d) $(addsuffix .d,$(TEST_BINS))
