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

CFLAGS = -Wall -O2 -g
LDFLAGS = -lncurses

PROG = tetris

all: $(PROG)

%.o: %.c
	$(CC) -o $@ $(CFLAGS) -c -MMD -MF .$@.d $<

$(PROG): $(OBJS)
	gcc -o $(PROG) $(OBJS) $(LDFLAGS)

clean:
	rm -f $(PROG) $(OBJS) $(deps)

-include $(deps)
