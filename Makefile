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

export IMAGE CPUS MEM PLATFORM

.PHONY: all build fixtures dev run-dev check-dev clean

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

# Generate the public 1M-event dataset (requires `generate` on PATH).
dev:
	generate --seed 1 --events 1000000 --clicks 250000 --output $(DEV)

# Run the pipeline on dev/ and time container execution.
run-dev: build
	rm -rf out && time env CPUS=8 MEM=10g scripts/docker_run.sh $(DEV) out

# Compare the previous run's Parquet output with dev-answer/.
check-dev:
	python3 scripts/compare.py out $(ANSWER)

clean:
	rm -f src/*.o src/*.d src/main
	rm -rf src/*.c.o src/*.s.o
