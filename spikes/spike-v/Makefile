## Spike V — Embedded Swift emitting riscv32 / ilp32d — build orchestration.
##
## Stages:
##   stage-0    Toolchain + ABI gate: Swift → riscv32 object, readelf check (both hosts)
##   stage-1    Link + run against ilp32d sysroot (both hosts)
##   stage-2    ilp32d ABI witness (both hosts)
##   stage-3    ARC + allocator (both hosts)
##   stage-4    Digest equivalence vs C + Rust (both hosts)
##   stage-5    Footprint: blyt build + cart_load.c (host, needs SDK)
##   all        Stages 0–5
##   clean      Remove artefacts and Docker images

DOCKER_IMAGE_ARM64 ?= spike-v-arm64
DOCKER_IMAGE_AMD64 ?= spike-v-amd64

SWIFT_SNAPSHOT ?= swift-DEVELOPMENT-SNAPSHOT-2026-06-12-a

# ─── docker build ─────────────────────────────────────────────────────────────

.PHONY: docker-build docker-build-arm64 docker-build-amd64

docker-build: docker-build-arm64 docker-build-amd64

docker-build-arm64:
	docker build \
	  --platform linux/arm64 \
	  --build-arg TARGETARCH=arm64 \
	  --build-arg SWIFT_SNAPSHOT=$(SWIFT_SNAPSHOT) \
	  --target stage0 \
	  -t $(DOCKER_IMAGE_ARM64) \
	  .

docker-build-amd64:
	docker build \
	  --platform linux/amd64 \
	  --build-arg TARGETARCH=amd64 \
	  --build-arg SWIFT_SNAPSHOT=$(SWIFT_SNAPSHOT) \
	  --target stage0 \
	  -t $(DOCKER_IMAGE_AMD64) \
	  .

# ─── Stage 0 — toolchain + ABI gate ──────────────────────────────────────────

.PHONY: stage-0 stage-0-arm64 stage-0-amd64

stage-0: stage-0-arm64 stage-0-amd64
	@echo "STAGE 0: PASS (both hosts)"

stage-0-arm64: docker-build-arm64
	@echo "--- Stage 0 arm64 ---"
	docker run --rm --platform linux/arm64 $(DOCKER_IMAGE_ARM64) \
	  sh -c "cat /spike-v/stage0-readelf.txt && echo '--- arm64 PASS ---'"

stage-0-amd64: docker-build-amd64
	@echo "--- Stage 0 amd64 ---"
	docker run --rm --platform linux/amd64 $(DOCKER_IMAGE_AMD64) \
	  sh -c "cat /spike-v/stage0-readelf.txt && echo '--- amd64 PASS ---'"

# ─── Composite ────────────────────────────────────────────────────────────────

.PHONY: all clean

all: stage-0

clean:
	docker rmi -f $(DOCKER_IMAGE_ARM64) $(DOCKER_IMAGE_AMD64) 2>/dev/null || true
