# Parallel Computing - Assignment 1
#
#   make            build benchmarks + tests into bin/
#   make test       run the 3 correctness tests (locks, barrier, counters)
#   make tsan       run the 3 correctness tests under ThreadSanitizer
#   make test-locks / test-barrier / test-counters   run just one of them
#   make debug      -O0 builds for the debugger -> bin/debug/
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

HEADERS   := $(wildcard include/*.h) tests/test_common.h
TESTS     := test_locks test_barrier test_counters
BINS      := bin/bench_lock bin/bench_barrier bin/bench_counter $(TESTS:%=bin/%)
TSAN_BINS := $(TESTS:%=bin/%_tsan)
DBG_BINS  := $(BINS:bin/%=bin/debug/%)

all: $(BINS)

bin/%: src/%.cpp $(HEADERS) | bin
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

bin/test_%: tests/test_%.cpp $(HEADERS) | bin
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# ThreadSanitizer builds: no OpenMP (libomp is not TSan-instrumented).
bin/test_%_tsan: tests/test_%.cpp $(HEADERS) | bin
	$(CXX) -std=c++20 -O1 -g -pthread -Iinclude -fsanitize=thread $< -o $@

# Debug builds: no optimisation, so breakpoints, stepping and local
# variables behave. Never benchmark these.
DBG_CXXFLAGS := $(filter-out -O%,$(CXXFLAGS)) -O0

bin/debug/%: src/%.cpp $(HEADERS) | bin/debug
	$(CXX) $(DBG_CXXFLAGS) $< -o $@ $(LDFLAGS)

bin/debug/test_%: tests/test_%.cpp $(HEADERS) | bin/debug
	$(CXX) $(DBG_CXXFLAGS) $< -o $@ $(LDFLAGS)

debug: $(DBG_BINS)

bin bin/debug:
	mkdir -p $@

# Runs all three even if one fails, then fails if any of them failed.
test: $(TESTS:%=bin/%)
	@status=0; for t in $(TESTS); do ./bin/$$t || status=1; echo; done; exit $$status

tsan: $(TSAN_BINS)
	@status=0; for t in $(TESTS); do ./bin/$${t}_tsan || status=1; echo; done; exit $$status

test-locks: bin/test_locks
	./bin/test_locks
test-barrier: bin/test_barrier
	./bin/test_barrier
test-counters: bin/test_counters
	./bin/test_counters

bench: $(BINS)
	./scripts/run_benchmarks.sh

clean:
	rm -rf bin

.PHONY: all test tsan test-locks test-barrier test-counters bench debug clean
