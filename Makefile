CC ?= gcc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -lm -lpthread

SRC_DIR := src
BIN_DIR := bin

TARGETS := $(BIN_DIR)/sender $(BIN_DIR)/receiver

.PHONY: all clean

all: $(TARGETS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/sender: $(SRC_DIR)/sender.c $(SRC_DIR)/rate_estimator.c $(SRC_DIR)/estimator_fix.c $(SRC_DIR)/estimator_naive_ewma.c $(SRC_DIR)/estimator_camel.c $(SRC_DIR)/estimator_gcc_REMB.c \
$(SRC_DIR)/proto.h $(SRC_DIR)/rate_estimator.h $(SRC_DIR)/estimator_fix.h \
$(SRC_DIR)/estimator_naive_ewma.h $(SRC_DIR)/estimator_camel.h $(SRC_DIR)/estimator_gcc_REMB.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_DIR)/sender.c $(SRC_DIR)/rate_estimator.c $(SRC_DIR)/estimator_fix.c \
$(SRC_DIR)/estimator_naive_ewma.c $(SRC_DIR)/estimator_camel.c $(SRC_DIR)/estimator_gcc_REMB.c $(LDLIBS)

$(BIN_DIR)/receiver: $(SRC_DIR)/receiver.c $(SRC_DIR)/proto.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_DIR)/receiver.c $(LDLIBS)

clean:
	rm -rf $(BIN_DIR)
