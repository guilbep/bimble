# web-ui: the Vue lab

A Vite + Vue + TypeScript page around the egui canvas (ADR 0048), with the brand of ADR 0049.
Vue only shows things. All logic stays in Rust: `src/bimble.ts` is the one typed client of the wasm exports.

## Run

1. Build the wasm: `scripts/build-web.sh` (from the repository root). It writes `web/dist/`.
2. In `web-ui/`: `npm install`, then `npm run wasm` (copies the wasm files into `public/bimble/`).
3. `npm run dev`, then open http://localhost:5173/.
   To open a model, put it in `web-ui/public/` and open http://localhost:5173/?url=/frame.bimble.

## Checks

`npm run typecheck` (vue-tsc) and `npm run build` (vite).

## Rules

- Raw colours live only in `src/css/tokens.css`. Components use `var(--...)`.
- Components never import the wasm. They call `src/bimble.ts`.
- Wasm data is never deeply reactive (`shallowRef`).
- Fonts are self-hosted (`@fontsource`), no request to a font service.
