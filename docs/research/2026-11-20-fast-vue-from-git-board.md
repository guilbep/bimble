# Research: fast Vue and performance checks, taken from git-board

_20 Nov 2026. STUDY station (Opus), read only, in the owner's git-board repository. Paths below
are in that repository. For the Vue page of ADR 0048._

## What git-board does

- Stack: Vue 3.5, Vite 8, TypeScript strict (`exactOptionalPropertyTypes`,
  `noUncheckedIndexedAccess`), vue-router with lazy routes. 3 runtime dependencies. No component
  library: Quasar was removed, because it sent 281 KB of CSS for 14 dialogs. Dialogs use `<dialog>`
  (docs/adr/0023-v3-local-first-traced.md).
- Styles: plain CSS on tokens. Raw colour values live in one file (web/src/css/tokens/values.css).
- State: module-level `shallowRef(new Map)` with `triggerRef`, no Pinia (web/src/local/store.ts).
- Every pull request counts the work and does not time it:
  - Playwright checks requests per act and the KB sent per page (web/e2e/budget.spec.ts).
  - It also checks that optimistic acts show while answers are held back (web/e2e/interactions.spec.ts).
- Timing runs nightly or on a `perf` label only, because a shared runner's clock is noise
  (.github/workflows/perf.yml).
  - Every act runs in a "Worst" setup: CPU 6x slower through CDP (web/perf/interactions.perf.ts).
  - Medians are compared with testdata/perf/budgets.txt. A median more than 15 % over budget fails.
  - If code gets faster, its budget goes down. A budget goes up only with a reason on its line.
- In the app, each act is timed from input to the paint after its last request (web/src/interaction.ts).
- docs/guide/performance.md keeps "Tried, and left": ideas that made things slower, with numbers.

## Take for Bimble's Vue page

1. An act registry (arrange, act, done) with a maximum number of `bimble-api` calls per act.
2. Counted checks on every pull request: API calls per act, KB of JS and CSS (the wasm counted apart).
3. In-page timing of acts, plus a "first frame of the model" mark, instead of Lighthouse LCP.
4. The 6x CPU throttle as the "old machine" setup (ROADMAP principle 1).
5. A budgets file with a reason per line, 15 % tolerance, timed nightly only.
6. `shallowRef` and `markRaw` for anything from wasm: never deep reactivity on model data.
7. Lazy routes, no component library, one tokens file (the brand chosen on the design canvas).
8. A "Tried, and left" list with numbers. `node --test` for pure TypeScript. `vue-tsc` before commit.

## Do not take

The network layer (SSE, IndexedDB copy, ETag), Lighthouse page metrics, server traces, the hash
router for a Go binary. Bimble's API is an in-process wasm call, not a server (inference).
