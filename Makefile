.PHONY: build shell compile clean

IMAGE_NAME := lswtcs-arm64-toolchain
WORKSPACE_DIR := $(shell pwd)

build: Dockerfile
	docker build -t $(IMAGE_NAME) .

shell: build
	docker run -it --rm -v "$(WORKSPACE_DIR)":/workspace $(IMAGE_NAME) bash

compile: build
	docker run --rm -v "$(WORKSPACE_DIR)":/workspace $(IMAGE_NAME) \
		sh -c "cd /workspace && mkdir -p build && cd build && cmake .. && make -j$$(nproc 2>/dev/null || echo 4)"

clean:
	rm -rf build
	docker rmi $(IMAGE_NAME) 2>/dev/null || true
