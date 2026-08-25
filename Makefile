.PHONY: build test quick standard clean

build:
	cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build --parallel

test: build
	ctest --test-dir build --output-on-failure
	python3 -m unittest discover -s tests -v

quick:
	./system-decap run --profile quick

standard:
	./system-decap run --profile standard

clean:
	cmake -E remove_directory build
