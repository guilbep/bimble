import "@fontsource/sora/500.css";
import "@fontsource/sora/700.css";
import "@fontsource/source-sans-3/400.css";
import "@fontsource/source-sans-3/600.css";
import "@fontsource/dm-mono/400.css";
import "./css/tokens.css";
import { createApp } from "vue";
import App from "./App.vue";
import { start } from "./bimble";

createApp(App).mount("#app");
// The canvas #bimble is in the DOM now: start the viewer.
start().catch((e: unknown) => console.error("Bimble could not start:", e));
