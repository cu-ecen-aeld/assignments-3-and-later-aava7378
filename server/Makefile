# Used chatgpt for reference to build this. Provided is the link: https://chatgpt.com/share/6991636f-aaa8-8004-a62b-4535a61d3c93 

TARGET := aesdsocket
SRCS   := aesdsocket.c
OBJS   := $(SRCS:.c=.o)

CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc

CFLAGS   ?= -Wall -Wextra -Werror -O2 -g
CPPFLAGS ?=
LDFLAGS  ?=
LDLIBS   ?=

.PHONY: all default clean
default: all
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

