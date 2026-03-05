# OpenCL GPU Compute Microbenchmark – Makefile
#
# Targets
#   all       build the benchmark executable (default)
#   clean     remove build artefacts
#
# Variables you may override on the command line:
#   CXX          C++ compiler        (default: g++)
#   OPENCL_DIR   OpenCL SDK root     (auto-detected when not set)
#   N            Matrix size for run (default: 1024)

CXX      ?= g++
CXXFLAGS  = -std=c++11 -O2 -Wall -Wextra -DCL_TARGET_OPENCL_VERSION=200

# ------------------------------------------------------------------ #
# OpenCL library and include detection                               #
# ------------------------------------------------------------------ #
ifdef OPENCL_DIR
  OPENCL_INC = -I$(OPENCL_DIR)/include
  OPENCL_LIB = -L$(OPENCL_DIR)/lib -lOpenCL
else
  # Try common system locations
  OPENCL_INC =
  OPENCL_LIB = -lOpenCL
endif

# ------------------------------------------------------------------ #
# Paths                                                              #
# ------------------------------------------------------------------ #
SRC_DIR    = src
KERNEL_DIR = kernels
BUILD_DIR  = build

SOURCES    = $(SRC_DIR)/main.cpp
TARGET     = $(BUILD_DIR)/matmul_bench

# ------------------------------------------------------------------ #
# Rules                                                              #
# ------------------------------------------------------------------ #
.PHONY: all clean run

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(SOURCES) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OPENCL_INC) $^ -o $@ $(OPENCL_LIB)
	@echo "Build successful: $@"

run: $(TARGET)
	./$(TARGET) $(N)

clean:
	rm -rf $(BUILD_DIR)
