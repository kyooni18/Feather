.PHONY: build run clean

build:
	cmake -S . -B build
	cmake --build build -j

run: build
	./build/feather_demo

clean:
	rm -rf build