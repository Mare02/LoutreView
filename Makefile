CC := clang
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDLIBS := -framework IOKit -framework CoreFoundation -framework CoreServices
TARGET := loutre-view

.PHONY: all clean run

all: $(TARGET)

$(TARGET): loutre-view.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
