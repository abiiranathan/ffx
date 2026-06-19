CC = gcc
CFLAGS := -Wall -Wextra -Werror -O3 -g -static
LDFLAGS := -lsolidc -lm
SRCS := main.c ffx_core.c ffx_probe.c ffx_cmds.c
TARGET := ffx

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) 

clean:
	rm -rf $(TARGET)
	