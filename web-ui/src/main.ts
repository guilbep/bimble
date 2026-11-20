import "./brand/fonts.css";
import "./brand/tokens.css";
import { createApp } from "vue";
import App from "./App.vue";
import { start } from "./bimble";

createApp(App).mount("#app");
// The canvas #bimble is in the DOM now: start the viewer.
start().catch((e: unknown) => console.error("Bimble could not start:", e));
