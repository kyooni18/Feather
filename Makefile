.PHONY: build run clean

build:
	cmake -E rm -rf build
	cmake -S . -B build
	cmake --build build -j

run: build
	./build/feather_demo

clean:
	rm -rf build
