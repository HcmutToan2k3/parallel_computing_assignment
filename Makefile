# Parallel Computing - Assignment 1
#
#   make            build benchmarks + tests into bin/
#   make test       run correctness tests (optimised build)
#   make tsan       run correctness tests under ThreadSanitizer
#   make bench      run the full benchmark sweep -> results/*.csv
#   make clean
#
# OpenMP (omp_lock / omp_barrier baselines) is enabled automatically when
# available. On macOS: `brew install libomp`. Force off with `make OMP=0`.

CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -pthread -Iinclude
LDFLAGS  ?= -pthread

OMP ?= auto
UNAME := $(shell uname -s)
ifeq ($(OMP),auto)
  ifeq ($(UNAME),Darwin)
    LIBOMP := $(shell brew --prefix libomp 2>/dev/null)
    ifneq ($(wildcard $(LIBOMP)/include/omp.h),)
      OMP := 1
    else
      OMP := 0
    endif
  else
    OMP := 1
  endif
endif
ifeq ($(OMP),1)
  ifeq ($(UNAME),Darwin)
    CXXFLAGS += -Xpreprocessor -fopenmp -I$(LIBOMP)/include
    LDFLAGS  += -L$(LIBOMP)/lib -lomp
  else
    CXXFLAGS += -fopenmp
    LDFLAGS  += -fopenmp
  endif
endif

HEADERS := $(wildcard include/*.h)
BINS    := bin/bench_lock bin/bench_barrier bin/bench_counter bin/test_all

all: $(BINS)

bin/%: src/%.cpp $(HEADERS) | bin
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

bin/test_all: tests/test_all.cpp $(HEADERS) | bin
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# ThreadSanitizer build: no OpenMP (libomp is not TSan-instrumented).
bin/test_all_tsan: tests/test_all.cpp $(HEADERS) | bin
	$(CXX) -std=c++20 -O1 -g -pthread -Iinclude -fsanitize=thread $< -o $@

bin:
	mkdir -p bin

test: bin/test_all
	./bin/test_all

tsan: bin/test_all_tsan
	./bin/test_all_tsan

bench: $(BINS)
	./scripts/run_benchmarks.sh

clean:
	rm -rf bin

.PHONY: all test tsan bench clean
