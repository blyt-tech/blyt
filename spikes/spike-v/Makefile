## Spike V — Embedded Swift emitting riscv32 / ilp32d — build orchestration.
##
## Stages:
##   stage-0    Toolchain + ABI gate: Swift → riscv32 object, readelf check (both hosts)
##   stage-1    Link + run: extract objects from Docker, link on host, run via blytplay
##   stage-2    ilp32d ABI witness (both hosts)
##   stage-3    ARC + allocator (both hosts)
##   stage-4    Digest equivalence vs C + Rust (both hosts)
##   stage-5    Footprint: blyt build + cart_load.c (host, needs SDK)
##   all        Stages 0–5
##   clean      Remove artefacts and Docker images
##
## Build context: the repo root (two levels up).
## Dockerfile is at spikes/spike-v/Dockerfile.

DOCKER_IMAGE_ARM64 ?= spike-v-arm64
DOCKER_IMAGE_AMD64 ?= spike-v-amd64

SWIFT_SNAPSHOT ?= swift-DEVELOPMENT-SNAPSHOT-2026-06-12-a

# Repo root (two directories up from spikes/spike-v/)
REPO_ROOT := $(realpath ../..)

# Main checkout SDK (borrowed for Stage 1+ host operations)
BLYT_SDK ?= /Users/tom/code/blyt/build/sdk
BLYTPLAY ?= $(BLYT_SDK)/bin/blytplay
BLYT_CLANG ?= $(BLYT_SDK)/bin/blyt-clang
BLYT_LD ?= $(BLYT_SDK)/bin/blyt-ld.lld
BLYT_OBJCOPY ?= $(BLYT_SDK)/bin/blyt-objcopy
BLYT_DEVTOOL ?= $(BLYT_SDK)/bin/blyt
BLYT_LIB_DIR ?= $(BLYT_SDK)/lib
BLYT_INC_DIR ?= $(BLYT_SDK)/include

BUILD_DIR := build

.PHONY: all clean \
        docker-build docker-build-arm64 docker-build-amd64 \
        stage-0 stage-0-arm64 stage-0-amd64 \
        stage-1 stage-1-compile stage-1-link stage-1-run \
        stage-2 stage-3 stage-4

# ─── docker build ─────────────────────────────────────────────────────────────

docker-build: docker-build-arm64 docker-build-amd64

docker-build-arm64:
	docker build \
	  --platform linux/arm64 \
	  --build-arg TARGETARCH=arm64 \
	  --build-arg SWIFT_SNAPSHOT=$(SWIFT_SNAPSHOT) \
	  --target stage1-compile \
	  -t $(DOCKER_IMAGE_ARM64) \
	  -f $(REPO_ROOT)/spikes/spike-v/Dockerfile \
	  $(REPO_ROOT)

docker-build-amd64:
	docker build \
	  --platform linux/amd64 \
	  --build-arg TARGETARCH=amd64 \
	  --build-arg SWIFT_SNAPSHOT=$(SWIFT_SNAPSHOT) \
	  --target stage1-compile \
	  -t $(DOCKER_IMAGE_AMD64) \
	  -f $(REPO_ROOT)/spikes/spike-v/Dockerfile \
	  $(REPO_ROOT)

# Stage 0 only (faster, skips Stage 1 compile)
docker-build-stage0-arm64:
	docker build \
	  --platform linux/arm64 \
	  --build-arg TARGETARCH=arm64 \
	  --build-arg SWIFT_SNAPSHOT=$(SWIFT_SNAPSHOT) \
	  --target stage0 \
	  -t $(DOCKER_IMAGE_ARM64)-stage0 \
	  -f $(REPO_ROOT)/spikes/spike-v/Dockerfile \
	  $(REPO_ROOT)

docker-build-stage0-amd64:
	docker build \
	  --platform linux/amd64 \
	  --build-arg TARGETARCH=amd64 \
	  --build-arg SWIFT_SNAPSHOT=$(SWIFT_SNAPSHOT) \
	  --target stage0 \
	  -t $(DOCKER_IMAGE_AMD64)-stage0 \
	  -f $(REPO_ROOT)/spikes/spike-v/Dockerfile \
	  $(REPO_ROOT)

# ─── Stage 0 — toolchain + ABI gate ──────────────────────────────────────────

stage-0: stage-0-arm64 stage-0-amd64
	@echo "STAGE 0: PASS (both hosts)"

stage-0-arm64: docker-build-stage0-arm64
	@echo "--- Stage 0 arm64 ---"
	docker run --rm --platform linux/arm64 $(DOCKER_IMAGE_ARM64)-stage0 \
	  sh -c "cat /spike-v/stage0-readelf.txt && echo '--- arm64 PASS ---'"

stage-0-amd64: docker-build-stage0-amd64
	@echo "--- Stage 0 amd64 ---"
	docker run --rm --platform linux/amd64 $(DOCKER_IMAGE_AMD64)-stage0 \
	  sh -c "cat /spike-v/stage0-readelf.txt && echo '--- amd64 PASS ---'"

# ─── Stage 1 — link + run on host ────────────────────────────────────────────
# Extracts cart.o from Docker, links with blyt SDK tools, runs via blytplay.
#
# FV-4: `blyt build` does not support Swift (CartLanguage enum lacks Swift).
# The manual steps below replicate what a `blyt build --lang swift` would do.
# This is the friction finding: the devtool link recipe is language-agnostic
# (obj files in, ELF out) but the *invocation path* is language-specific.

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

stage-1: stage-1-run

stage-1-compile: docker-build-arm64 | $(BUILD_DIR)
	@echo "--- Stage 1: extracting cart.o from Docker ---"
	docker run --rm --platform linux/arm64 $(DOCKER_IMAGE_ARM64) \
	  cat /spike-v/cart.o > $(BUILD_DIR)/cart.o

stage-1-stubs: | $(BUILD_DIR)
	@echo "--- Stage 1: compiling blyt_entry + blyt_interp ---"
	# Linker script: minimal, C/Rust/Swift pattern (not the Lua hybrid script).
	printf 'ENTRY(_blyt_entry)\n' > $(BUILD_DIR)/cart.ld
	printf '/* Generated. */\nvoid blyt_main(void);\n__attribute__((noreturn)) void blyt_exit(int);\nvoid blyt_runtime_startup(void);\nvoid _blyt_entry(void) { blyt_runtime_startup(); blyt_main(); blyt_exit(0); }\n' \
	  > $(BUILD_DIR)/_blyt_entry.c
	printf '/* Generated. */\n__attribute__((section(".interp"), used))\nconst char blyt_interp[] = "/lib/ld-blyt.so.1";\n' \
	  > $(BUILD_DIR)/_blyt_interp.c
	$(BLYT_CLANG) -c \
	  --target=riscv32 -march=rv32imafdc -mabi=ilp32d \
	  -fpic -ffunction-sections -fdata-sections \
	  -I$(BLYT_INC_DIR) \
	  -o $(BUILD_DIR)/_blyt_entry.o $(BUILD_DIR)/_blyt_entry.c
	$(BLYT_CLANG) -c \
	  --target=riscv32 -march=rv32imafdc -mabi=ilp32d \
	  -fpic -ffunction-sections -fdata-sections \
	  -o $(BUILD_DIR)/_blyt_interp.o $(BUILD_DIR)/_blyt_interp.c

stage-1-link: stage-1-compile stage-1-stubs
	@echo "--- Stage 1: linking Swift cart ELF ---"
	# Link recipe mirrors devtool/src/build.rs link_cart():
	# blyt-clang is a symlink to Homebrew clang; blyt-ld.lld is a symlink to lld.
	# Only -lblyt32: it absorbs libblytc symbols; libblytc.so loads via DT_NEEDED
	# at runtime (blyt_cart runs without separate -lblytc/-lblytcommon here).
	$(BLYT_CLANG) \
	  --target=riscv32 -march=rv32imafdc -mabi=ilp32d -nostdlib \
	  -fuse-ld=$(BLYT_LD) \
	  -Wl,--pic-executable \
	  -Wl,-Bdynamic \
	  -Wl,-z,relro \
	  -Wl,-z,now \
	  -Wl,--build-id=none \
	  -Wl,--gc-sections \
	  -T$(BUILD_DIR)/cart.ld \
	  -Wl,-u,blyt_interp \
	  -Wl,-u,blyt_cart_init \
	  -Wl,-u,blyt_cart_update \
	  -Wl,-u,blyt_cart_draw \
	  $(BUILD_DIR)/_blyt_entry.o \
	  $(BUILD_DIR)/_blyt_interp.o \
	  $(BUILD_DIR)/cart.o \
	  -L$(BLYT_LIB_DIR) \
	  -lblyt32 \
	  -o $(BUILD_DIR)/cart_raw.elf
	@echo "--- Stage 1: generating .cart.info ---"
	# Borrow .cart.info from hello-c (same FlatBuffer API version; spike shortcut).
	# Production Swift support would run the packer to generate its own .cart.info.
	$(BLYT_DEVTOOL) build $(REPO_ROOT)/examples/hello-c 2>/dev/null || true
	$(BLYT_OBJCOPY) \
	  --dump-section .cart.info=$(BUILD_DIR)/cart.info.bin \
	  $(REPO_ROOT)/examples/hello-c/build/hello-c.blyt
	@echo "--- Stage 1: finalising cart ---"
	$(BLYT_OBJCOPY) \
	  --add-section .cart.info=$(BUILD_DIR)/cart.info.bin \
	  --set-section-flags .cart.info=alloc,readonly \
	  --remove-section=.riscv.attributes \
	  --remove-section=.comment \
	  --strip-debug \
	  --strip-unneeded \
	  --remove-section=.symtab \
	  --remove-section=.strtab \
	  $(BUILD_DIR)/cart_raw.elf \
	  $(BUILD_DIR)/cart.blyt
	# Note: .swift_modhash no longer needs stripping — cart_load.c now uses a
	# denylist (PR #74) and passes unknown sections through.

stage-1-run: stage-1-link
	@echo "--- Stage 1: running Swift cart via blytplay ---"
	$(BLYTPLAY) --headless --quit-after 10 $(BUILD_DIR)/cart.blyt
	@echo "STAGE 1 GATE: cart ran to completion — PASS"

# ─── Composite ────────────────────────────────────────────────────────────────

all: stage-0 stage-1

clean:
	rm -rf $(BUILD_DIR)
	docker rmi -f $(DOCKER_IMAGE_ARM64) $(DOCKER_IMAGE_AMD64) \
	             $(DOCKER_IMAGE_ARM64)-stage0 $(DOCKER_IMAGE_AMD64)-stage0 2>/dev/null || true
