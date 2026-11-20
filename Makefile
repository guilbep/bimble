# Needs Rust stable, CMake and a C++ compiler. `make help` lists the targets.
# ponytail: Windows has no make by default; the cargo commands in the README work there.

WASM_BINDGEN = $(shell awk '/^name = "wasm-bindgen"$$/ {getline; gsub(/version = |"/, ""); print; exit}' Cargo.lock)
ORACLE = target/oracle

.PHONY: build run install test check fmt web-setup web serve oracle perf-web clean help

build: ## Build the bimble binary (target/release/bimble)
	cargo build --release -p bimble-cli

run: ## Run bimble, e.g. make run ARGS="view model.ifc"
	cargo run --release -p bimble-cli -- $(ARGS)

install: ## Install bimble into ~/.cargo/bin
	cargo install --path crates/bimble-cli --locked

test: ## Run the tests
	cargo test --workspace

check: ## What CI's rust job runs; must pass before a pull request
	cargo fmt --all --check
	cargo clippy --workspace --all-targets -- -D warnings
	cargo test --workspace

fmt: ## Format the code
	cargo fmt --all

web-setup: ## Install what the browser build needs (wasm32 target, wasm-bindgen-cli)
	rustup target add wasm32-unknown-unknown
	cargo install wasm-bindgen-cli --version $(WASM_BINDGEN) --locked

web: ## Build the browser version into web/dist (also needs npm)
	scripts/build-web.sh

serve: web ## Build the browser version and serve it on http://localhost:8000
	python3 -m http.server -d web/dist 8000

oracle: build ## Compare with IfcOpenShell (pip install ifcopenshell)
	mkdir -p $(ORACLE)
	target/release/bimble demo $(ORACLE)/frame.bimble
	target/release/bimble export-ifc $(ORACLE)/frame.bimble $(ORACLE)/frame.ifc
	python3 -I scripts/check_ifc.py $(ORACLE)/frame.bimble $(ORACLE)/frame.ifc
	target/release/bimble info $(ORACLE)/frame.ifc --elements-json > $(ORACLE)/ours.jsonl
	python3 -I scripts/compare_ifcopenshell.py $(ORACLE)/frame.ifc $(ORACLE)/ours.jsonl

perf-web: ## Time the Vue lab page with Lighthouse (mobile, simulated), held to web-ui/perf/budgets.txt
	scripts/build-web.sh
	cargo build --release -p bimble-cli
	mkdir -p web-ui/public
	target/release/bimble demo web-ui/public/perf-small.bimble
	target/release/bimble demo web-ui/public/perf-large.bimble --bays-x 20 --bays-y 20 --storeys 10
	target/release/bimble info web-ui/public/perf-small.bimble
	target/release/bimble info web-ui/public/perf-large.bimble
	cd web-ui && npm ci && npm run wasm && npm run build
	cd web-ui && node perf/lighthouse.mjs $(or $(PERF_WEB_RUNS),3)
	node web-ui/perf/check.mjs perf-results/web.txt web-ui/perf/budgets.txt 0.15

clean: ## Delete build output
	cargo clean
	rm -rf web/dist

help: ## List the targets
	@awk -F':.*## ' '/^[a-z-]+:.*## / {printf "  %-10s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
