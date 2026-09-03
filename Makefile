CC        = gcc
CFLAGS    = -Wall -Wextra -O2
CPPFLAGS  = -I. -Iconfig -Iprotocol -Iwire -Iruntime
LDFLAGS   = -lpcap -lpthread
BUILD_DIR = build
TARGET    = $(BUILD_DIR)/inc
SRCS      = protocol/main.c config/app_config.c protocol/allreduce_workload.c wire/arbor_wire.c runtime/runtime_common.c protocol/host.c protocol/requester.c protocol/responder.c protocol/router.c config/arbor_fabric.c
OBJS      = $(SRCS:%.c=$(BUILD_DIR)/%.o)
CFG       = ranks.cfg
HOSTS     = host1 host2 host3 host4
ROUTERS   = router-root router-a router-a0 router-a1
N         = 4
TEST_ROOT = tests
TEST_CONFIG_DIR = topology/tree
TEST_SCRIPT_DIR = $(TEST_ROOT)/scripts
TEST_DATA_DIR = $(TEST_ROOT)/data/current
TEST_OUT_DIR = $(TEST_ROOT)/out

.PHONY: all clean setup_env setup_topo clean_topo kill test_allreduce test test_allreduce_loss test_allreduce_2star test_allreduce_loss_2star test_allreduce_loss_2star

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TEST_OUT_DIR)
	rm -f $(TEST_DATA_DIR)/input-*.data $(TEST_DATA_DIR)/expected-allreduce.data

setup_env:
	apt install -y libpcap-dev
	docker image build -t node .

setup_topo:
	./topology/tree/setup.sh setup

clean_topo:
	./topology/tree/setup.sh clean

kill:
	-@for c in $(ROUTERS) $(HOSTS); do docker exec $$c pkill -f $(TARGET) 2>/dev/null; done

define run-allreduce
	@bash $(TEST_SCRIPT_DIR)/run_allreduce_test.sh
endef

test_allreduce: $(TARGET)
	$(run-allreduce)

test_allreduce_loss: $(TARGET)
	bash $(TEST_SCRIPT_DIR)/run_allreduce_loss_test.sh

test_allreduce_2star: $(TARGET)
	bash $(TEST_SCRIPT_DIR)/run_allreduce_test_2star.sh

test_allreduce_loss_2star: $(TARGET)
	bash $(TEST_SCRIPT_DIR)/run_allreduce_loss_test_2star.sh
