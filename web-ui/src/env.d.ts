/// <reference types="vite/client" />
declare module "*.vue" {
  import type { DefineComponent } from "vue";
  const c: DefineComponent;
  export default c;
}
/** SET1: the commit the page was built from (vite.config.ts `define`; "" when git is not there). */
declare const __BB_COMMIT__: string;
