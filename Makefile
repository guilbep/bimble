# Short names for the C core's usual commands (DEV.md). Each target is one line over ccore/ and
# web-ui/; the scripts there are the reference and run without make. `make help` lists the targets.

S ?=

.PHONY: help doctor deps tools dev dev-wasm build debug test gate full san lab release wasm lab-build weight perf parity flags clean

help: ## List the targets
	@awk -F':.*## ' '/^[a-z-]+:.*## / {printf "  make %-10s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

doctor: ## Show which prerequisites are found (DEV.md section 1)
	@for t in clang wasm-ld tcc wasm-opt cmake node npm inotifywait gdb; do \
	  printf '  %-12s %s\n' "$$t" "$$(command -v $$t || echo MISSING)"; done
	@printf '  %-12s %s\n' chromium "$${CHROMIUM:-$$(test -x /usr/bin/chromium && echo /usr/bin/chromium || echo 'MISSING (or set CHROMIUM)')}"
	@printf '  %-12s %s\n' manifold "$$(test -d ccore/out/deps/manifold/lib && echo ccore/out/deps/manifold || echo 'not built: make deps')"
	@printf '  %-12s %s\n' node_modules "$$(test -e web-ui/node_modules && echo web-ui/node_modules || echo 'missing: cd web-ui && npm ci')"
	@printf '  %-12s pinned %s\n' tcc "$$(ccore/ci/tcc.sh --commit)"

deps: ## Build the boolean kernel (manifold) into ccore/out/deps (minutes, once)
	ccore/deps.sh

tools: ## Install the pinned tcc and wasm-opt into ~/.local (then put their bin/ on PATH)
	ccore/ci/tcc.sh $(HOME)/.local/tcc
	ccore/ci/binaryen.sh $(HOME)/.local/binaryen

dev: ## The edit loop: host + Vite, rebuilt and restarted on every save in ccore/src|session|host
	ccore/dev.sh

dev-wasm: ## The lab on the C wasm module in the page (no server)
	cd web-ui && npm run dev:wasm

build: ## Library, test binaries, host and tools with tcc (ccore/out/tcc)
	ccore/build.sh tcc

debug: ## ccore/out/debug/bimble-ccore: the host with clang -O0 -g, for gdb (no kernel)
	mkdir -p ccore/out/debug
	cd ccore && clang -std=c11 -O0 -g -DRYU_ONLY_64_BIT_OPS -Isrc -Ivendor/ryu -Isession -Ihost \
	  $$(ls src/*.c | grep -v wasm_stub) session/*.c host/*.c -lm -pthread -o out/debug/bimble-ccore

test: ## The C suites with tcc; S="json fmt" for some of them
	ccore/test/run.sh $(S)

gate: ## The gate of every change: ccore/ci/run.sh fast
	ccore/ci/run.sh fast

full: ## Every suite once, as CI on Linux: ccore/ci/run.sh full
	ccore/ci/run.sh full

san: ## The C suites with clang, ASan and UBSan; S="json" for some
	CC=clang CFLAGS="-fsanitize=address,undefined" ccore/test/run.sh $(S)

lab: ## The Chromium lab suites: ccore/ci/run.sh lab
	ccore/ci/run.sh lab

release: ## The release archive (ccore/out/release), then its test
	ccore/build.sh release && node ccore/test/release_test.mjs

wasm: ## Both wasm modules, copied into web-ui/public/ccore
	cd web-ui && npm run ccore:wasm && npm run ccore:wasm-view

lab-build: ## The lab as GitHub Pages serves it under /c/ (into web-ui/dist)
	cd web-ui && npm run build:ccore

weight: ## ccore/results/weight.md (after make release and make wasm)
	node ccore/tools/weight.mjs

perf: ## ccore/results/perf.md in the short mode (node ccore/tools/perf.mjs --full for all)
	node ccore/tools/perf.mjs

parity: ## The C host against the committed Rust answers (node ccore/tools/parity.mjs --write)
	node ccore/tools/parity.mjs

flags: ## ccore/compile_commands.json, for editors that do not read ccore/compile_flags.txt
	ccore/tools/compile_commands.sh

clean: ## Delete ccore/out (the kernel too: make deps again)
	rm -rf ccore/out
