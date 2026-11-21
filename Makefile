# Native x86-64 implementation. C and GNU assembler sources live in src/.
# The target CPU is the scorer's Intel Xeon Cascade Lake (AVX-512 capable).

CC       = gcc
TARGET   := src/main
C_SRCS   := $(wildcard src/*.c)
ASM_SRCS := $(wildcard src/*.s)
C_OBJS   := $(patsubst src/%.c,src/%.c.o,$(C_SRCS))
ASM_OBJS := $(patsubst src/%.s,src/%.s.o,$(ASM_SRCS))
OBJS     := $(C_OBJS) $(ASM_OBJS)
DEPS     := $(OBJS:.o=.d)

CPPFLAGS ?=
CFLAGS   ?= -O3 -march=cascadelake -mtune=cascadelake -fno-pie -fno-plt -Wall -Wextra -Wpedantic
ASFLAGS  ?= -O3 -march=cascadelake -mtune=cascadelake -fno-pie
LDFLAGS  ?= -static -no-pie
LDLIBS   ?= -pthread -lm

IMAGE    ?= scratch_build:latest
CPUS     ?= 8
MEM      ?= 24g
PLATFORM ?= linux/amd64
DEV      ?= dev
FIXTURES ?= $(if $(wildcard fixtures),fixtures,../../fixtures)
ANSWER   ?= dev-answer
SEED     ?= 1
EVENTS   ?= 1000000
CLICKS   ?= 250000
RUN_MEM  ?= 10g
PY       ?= python3

export IMAGE CPUS MEM PLATFORM

.PHONY: all build fixtures dev run-dev check-dev check-dev-events clean clean-out clean-dataset unit check-fixtures

# Plain `make` builds the native executable. `make build` also builds its image.
all: $(TARGET)

$(TARGET): $(OBJS)
	@if [ -z "$(strip $(OBJS))" ]; then \
		echo "No src/*.c or src/*.s source files found." >&2; \
		exit 1; \
	fi
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.c.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

src/%.s.o: src/%.s
	$(CC) $(CPPFLAGS) $(ASFLAGS) -c $< -o $@

-include $(DEPS)

# Build the linux/amd64 image. The image contains only the static executable.
build: $(TARGET)
	docker buildx build --platform $(PLATFORM) --load -t $(IMAGE) .

# Run all hand-written edge-case fixtures through the image.
fixtures: build
	python3 scripts/run_fixtures.py $(FIXTURES) -- scripts/docker_run.sh

# Generate a dataset (requires `generate` on PATH). Defaults = the public 1M dev set;
# e.g. `make dev DEV=dev30m EVENTS=30000000 CLICKS=7500000`.
dev:
	generate --seed $(SEED) --events $(EVENTS) --clicks $(CLICKS) --output $(DEV)

# Run the pipeline on dev/ and time container execution.
run-dev: build
	rm -rf out && time env CPUS=8 MEM=$(RUN_MEM) scripts/docker_run.sh $(DEV) out

# Compare the previous run's Parquet output with dev-answer/.
check-dev:
	python3 scripts/compare.py out $(ANSWER)

# Compare out/user_daily with $(ANSWER) (events-only build: no campaign_daily).
check-dev-events:
	cd scripts && $(PY) check_events.py --compare ../out $(abspath $(ANSWER))

# Unit tests of the assembly primitives (not part of src/main).
UNIT := src/tests/unit
unit: $(ASM_OBJS) src/tests/unit.c src/evx.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(UNIT) src/tests/unit.c $(ASM_OBJS) $(LDLIBS)
	./$(UNIT)

# Events-only fixture check (user_daily only) without Docker. PY needs polars.
check-fixtures: $(TARGET)
	$(PY) scripts/check_events.py $(FIXTURES) -- $(TARGET)

clean:
	rm -f src/*.o src/*.d src/main $(UNIT)
	rm -rf src/*.c.o src/*.s.o

clean-out:
	rm -rf out

# Remove a generated dataset; refuses to delete the public dev/ set.
clean-dataset:
	@if [ "$(DEV)" = "dev" ]; then echo "refusing to delete dev/ (pass DEV=...)" >&2; exit 1; fi
	rm -rf $(DEV)
