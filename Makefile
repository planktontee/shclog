BUILDS := debug asan tsan release

build/debug: Makefile
	meson setup --reconfigure $@ -Dbuildtype=debug -Db_ndebug=false -Db_sanitize=none

build/asan: Makefile
	meson setup --reconfigure $@ -Doptimization=3 -Ddebug=true -Db_ndebug=false -Db_sanitize=address,undefined

build/tsan: Makefile
	meson setup --reconfigure $@ -Doptimization=3 -Ddebug=true -Db_ndebug=false -Db_sanitize=thread

build/release: Makefile
	meson setup --reconfigure $@ -Dbuildtype=release -Ddebug=false -Db_ndebug=true -Db_sanitize=none

$(BUILDS): %: build/%
	meson test -C $< --verbose

TIDY_TARGETS := tidy_debug tidy_release

$(TIDY_TARGETS): tidy_%: build/%
	ninja -C $< clang-tidy

tidy: $(TIDY_TARGETS)

build: tidy $(BUILDS)

setup: $(addprefix build/,$(BUILDS))

clean:
	@if [ -L build ]; then echo "build is a symlink, refusing to remove" >&2; exit 1; fi
	rm -rf build

.PHONY: build setup $(BUILDS)

