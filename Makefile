CC := clang
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDLIBS := -framework IOKit -framework CoreFoundation
TARGET := sysview

.PHONY: all clean run

all: $(TARGET)

$(TARGET): sysview.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
