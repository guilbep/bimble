import "./brand/fonts.css";
import "./brand/tokens.css";
import { createApp } from "vue";
import App from "./App.vue";
import { start } from "./bimble";
import { installFiles } from "./files"; // FS1: drop a file, the question before leaving with changes

createApp(App).mount("#app");
installFiles(); // FS1
// The canvas #bimble is in the DOM now: start the viewer.
start().catch((e: unknown) => console.error("Bimble could not start:", e));
