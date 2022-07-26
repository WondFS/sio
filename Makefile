SPDK_ROOT_DIR := $(abspath $(CURDIR)/spdk)
APP = sio
include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk