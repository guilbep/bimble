// Boolean operations for Bimble in a browser (ADR 0020): the manifold kernel's own WebAssembly
// build (npm `manifold-3d`, copied next to this file by scripts/build-web.sh), exposed as
// `globalThis.bimbleBoolean`, which bimble-geom calls synchronously.
//
// bimbleBoolean(op, aPos, aTri, cutters) → [Float32Array positions, Uint32Array triangles]
//   op: 0 difference, 1 union, 2 intersection; a op (union of cutters)
//   aPos: Float32Array x,y,z per vertex; aTri: Uint32Array, 3 indices per triangle
//   cutters: array of [Float32Array, Uint32Array]
// Throws a message when an operand isn't a closed solid: Bimble then draws the element uncut.

import Module from "./manifold.js";

export async function loadManifold() {
  const wasm = await Module();
  wasm.setup();
  const { Manifold, Mesh } = wasm;

  // Manifold objects live in the kernel's memory: free every one we make.
  const solid = (pos, tri, made) => {
    const m = new Manifold(new Mesh({ numProp: 3, vertProperties: pos, triVerts: tri }));
    made.push(m);
    const status = m.status();
    if (status !== "NoError") throw new Error(`not a closed solid: ${status}`);
    return m;
  };

  globalThis.bimbleBoolean = (op, aPos, aTri, cutters) => {
    const made = [];
    try {
      const a = solid(aPos, aTri, made);
      let cut = solid(cutters[0][0], cutters[0][1], made);
      for (let i = 1; i < cutters.length; i++) {
        cut = cut.add(solid(cutters[i][0], cutters[i][1], made));
        made.push(cut);
      }
      const r = op === 0 ? a.subtract(cut) : op === 1 ? a.add(cut) : a.intersect(cut);
      made.push(r);
      const status = r.status();
      if (status !== "NoError") throw new Error(`boolean failed: ${status}`);
      const mesh = r.getMesh();
      // Positions are the first 3 of numProp properties per vertex.
      const n = mesh.numProp, props = mesh.vertProperties;
      const pos = new Float32Array((props.length / n) * 3);
      for (let v = 0, k = 0; v < props.length; v += n) {
        pos[k++] = props[v]; pos[k++] = props[v + 1]; pos[k++] = props[v + 2];
      }
      return [pos, new Uint32Array(mesh.triVerts)];
    } finally {
      for (const m of made) m.delete();
    }
  };
}
