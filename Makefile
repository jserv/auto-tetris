CFLAGS = -Wall -O2 -g
LDFLAGS =

PROG = tetris
TEST_PROG = tests/test-runner

# Main program source files
MAIN_SRCS = main.c

# Common source files shared between main program and tests
COMMON_SRCS = \
    nalloc.c \
    block.c  \
    shape.c  \
    grid.c \
    move.c \
    tui.c \
    game.c

# Test source files
TEST_SRCS = \
    tests/test-types.c \
    tests/test-nalloc.c \
    tests/test-block.c \
    tests/test-shape.c \
    tests/test-grid.c \
    tests/test-move.c \
    tests/test-game.c \
    tests/driver.c

# Object files
MAIN_OBJS = $(MAIN_SRCS:.c=.o)
COMMON_OBJS = $(COMMON_SRCS:.c=.o)
TEST_OBJS = $(TEST_SRCS:.c=.o)

# All object files for dependency tracking
ALL_OBJS = $(MAIN_OBJS) $(COMMON_OBJS) $(TEST_OBJS)
deps := $(ALL_OBJS:%.o=.%.o.d)

# Control the build verbosity
ifeq ("$(VERBOSE)","1")
    Q :=
    VECHO = @true
else
    Q := @
    VECHO = @printf
endif

all: $(PROG)

# Generic compilation rule for source files
%.o: %.c
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF .$@.d $<

# Main program
$(PROG): $(COMMON_OBJS) $(MAIN_OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)

check: $(TEST_PROG)
	$(VECHO) "  RUN\t$<\n"
	$(Q)./$(TEST_PROG)

$(TEST_PROG): $(TEST_OBJS) $(COMMON_OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)

# Benchmark target
bench: $(PROG)
	$(VECHO) "  BENCH\t$<\n"
	$(Q)./$(PROG) -b

clean:
	$(RM) $(PROG) $(ALL_OBJS) $(deps)
	$(RM) $(TEST_PROG)
	-$(RM) -r .tests

.PHONY: all check bench clean

-include $(deps)
