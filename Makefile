CFLAGS = -Wall -O2 -g
LDFLAGS =

PROG = tetris

SRCS = \
       nalloc.c \
       block.c  \
       shape.c  \
       grid.c \
       move.c \
       tui.c \
       game.c \
       main.c

OBJS = $(SRCS:.c=.o)
deps := $(OBJS:%.o=.%.o.d)

# Control the build verbosity
ifeq ("$(VERBOSE)","1")
    Q :=
    VECHO = @true
else
    Q := @
    VECHO = @printf
endif


all: $(PROG)

%.o: %.c
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF .$@.d $<

$(PROG): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)

clean:
	$(RM) $(PROG) $(OBJS) $(deps)

-include $(deps)
