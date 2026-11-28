# NUI3: tighter visibility, clusters, the still frame: synthetic A/B (committed)

Offscreen, `bimble --offscreen --size 1492x790 --scale 2` (2984x1580, the 3D view 2392x1084), Apple M1 Pro (Asahi, Mesa
Honeykrisp), 27 Nov 2026, other builders running (load 2-9: medians hold, p95 move by several ms between runs, more on
the few still frames of these small sets). Each table is one `--compare`, each set twice (the first run warms the file
cache). "NUI2" is NUI2's defaults: `occlusion_test=0,cluster_tris=0,occlusion_tiles=0,loaders=2,occlusion_cold=0,hiz_pair=0`;
"NUI3 defaults": element boxes (`occlusion_test` 2), clusters of 512 (`cluster_tris`), leaves under their box
(`occlusion_tiles`), the cold elements of a still frame under one gate (`occlusion_cold` 1), two pyramid levels a
dispatch (`hiz_pair`), four loader threads (`loaders`). The owner's project's tables: the ticket's report (private data).

## The truth test (each 3D frame without occlusion and back-face culling, then as set; ADR 0084 decision 15)

| set | settings | frames | frames with ids missing | ids missing | coplanar ties | of the missing, 8 px or fewer |
|---|---|---|---|---|---|---|
| 17 synthetic links | NUI3 defaults | 1,700 | 0 | 0 | 117,437 | 0 |
| 18 buildings | NUI3 defaults | 1,668 | 0 | 0 | 0 | 0 |
| 17 synthetic links | NUI3 defaults, cull_back on | 1,696 | 1,587 | 103,336 | 5,793 | 91 |

Back faces (`cull_back`) stay off: with the element-level occlusion they lose elements on the synthetic links (above); with
occlusion by tile only (`occlusion_groups=false`) or none they lose about 140 element-frames in 1,600-1,700 frames, every
one of 8 px or fewer (the rasterizer's tie on silhouettes: ADR 0084 decision 23).

## Where the triangles go (18 buildings, six views, `BIMBLE_TILES_SURPLUS=1`): leaves' triangles drawn, M

| view | need (elements shown) | NUI2 | NUI3 defaults |
|---|---|---|---|
| fit | 0.68 | 1.96 | 1.26 |
| far | 0.15 | 0.61 | 0.25 |
| close | 0.07 | 0.15 | 0.08 |
| in1 | 0.00 | 0.01 | 0.01 |
| in2 | 0.06 | 0.26 | 0.19 |
| in3 | 0.02 | 0.21 | 0.02 |

## 17 synthetic links (`native/bench/make_links.mjs`: 17 x 400 unique closed meshes; the orbit-zoom gesture after `link_folder`)

Every element is in view and none hides another: occlusion has little to remove.

| | NUI2 (see above) | NUI3 defaults | NUI2 (see above) | NUI3 defaults | 
|---|---|---|---|---|
| first picture (process start) | 72 ms | 94 ms | 88 ms | 71 ms | 
| complete | 1105 ms | 937 ms | 1130 ms | 980 ms | 
| moving frame median / p95 | 9.2 / 12.4 ms (n 240) | 9.8 / 13.3 ms (n 240) | 8.9 / 12.3 ms (n 240) | 9.9 / 13.4 ms (n 242) | 
| still frame median / p95 | 7.5 / 13.7 ms (n 135) | 9.1 / 17.7 ms (n 83) | 7.0 / 13.4 ms (n 138) | 9.3 / 27.3 ms (n 84) | 
| input -> frame median / p95 / max | 9.7 / 12.9 / 18.2 ms (n 243) | 10.2 / 13.7 / 21.0 ms (n 243) | 9.4 / 12.7 / 15.8 ms (n 243) | 10.4 / 14.0 / 18.0 ms (n 245) | 
| memory peak / now | 100 / 100 MB | 104 / 104 MB | 98 / 98 MB | 104 / 104 MB | 
| GPU | 219 MB | 221 MB | 219 MB | 221 MB | 
| triangles / draws (last frame) | 3519816 / 10 | 3299088 / 10 | 3519816 / 10 | 3299088 / 10 | 
| triangles moving / still (median) | 3235440 / 1768428 | 2580700 / 1674724 | 3313776 / 1695156 | 2587740 / 1539252 | 
| tiles drawn moving (median) | 289 | 289 | 289 | 289 | 
| GPU peak | 219 MB | 221 MB | 219 MB | 221 MB | 
| moving frame by stage, median ms: logic / acquire / UI build / 3D record / UI record / GPU | 0.16 / 0.01 / 0.21 / 0.00 / 0.13 / 6.95 | 0.02 / 0.02 / 0.25 / 0.00 / 0.12 / 6.63 | 0.01 / 0.01 / 0.20 / 0.00 / 0.10 / 6.72 | 0.02 / 0.03 / 0.21 / 0.00 / 0.12 / 6.81 | 

## 18 buildings (`TILES_SYNTH=6,3,10,400 test/run.sh tiles`: 73,260 elements, 6.69 M triangles, a view file opened as it is; preset orbit-zoom)

| | NUI2 (see above) | NUI3 defaults | NUI2 (see above) | NUI3 defaults | 
|---|---|---|---|---|
| first picture (process start) | 7194 ms | 7074 ms | 7253 ms | 7184 ms | 
| complete | 7928 ms | 7527 ms | 7949 ms | 7631 ms | 
| moving frame median / p95 | 9.3 / 16.2 ms (n 267) | 9.3 / 17.0 ms (n 265) | 9.2 / 14.6 ms (n 277) | 9.4 / 16.5 ms (n 264) | 
| still frame median / p95 | 10.9 / 14.1 ms (n 125) | 9.1 / 19.0 ms (n 84) | 10.6 / 18.2 ms (n 124) | 9.6 / 22.2 ms (n 79) | 
| input -> frame median / p95 / max | 9.6 / 16.4 / 27.4 ms (n 240) | 9.7 / 16.7 / 30.2 ms (n 241) | 9.5 / 14.1 / 25.5 ms (n 241) | 9.8 / 14.0 / 31.1 ms (n 241) | 
| memory peak / now | 325 / 252 MB | 324 / 318 MB | 336 / 300 MB | 317 / 273 MB | 
| GPU | 120 MB | 125 MB | 120 MB | 125 MB | 
| triangles / draws (last frame) | 3748488 / 8 | 3701748 / 8 | 3873516 / 8 | 3862680 / 8 | 
| triangles moving / still (median) | 779562 / 1452720 | 727724 / 1019664 | 749792 / 1422576 | 697506 / 1018884 | 
| tiles drawn moving (median) | 135 | 138 | 134 | 134 | 
| GPU peak | 120 MB | 125 MB | 120 MB | 125 MB | 
| moving frame by stage, median ms: logic / acquire / UI build / 3D record / UI record / GPU | 0.01 / 0.02 / 0.18 / 0.00 / 0.10 / 5.64 | 0.01 / 0.01 / 0.20 / 1.74 / 0.11 / 5.63 | 0.01 / 0.01 / 0.18 / 0.00 / 0.10 / 5.44 | 0.01 / 0.01 / 0.17 / 0.00 / 0.11 / 5.55 | 

On these small sets the still frames are few (80-140 a run): their p95 is three or four frames, and the cold elements'
runs (drawn whole when one of them shows) cost them more than they save. On the owner's project (numbers: the ticket's
report) the NUI3 defaults drew fewer triangles moving and still, finished loading sooner and lowered the moving and still
p95; the truth test found 0 missing inside and outside.

## The georeferenced test (`test/native_geo_test.mjs`)

The same synthetic building linked at 0, and at 2,600 km beside a small link in a local frame: 0.13 % of pixels differ with
the scene origin where the triangles are, 1.73 % with the old origin (the first link ready); Measure 18,300.000 mm at 0 and
at 2,600 km (MEAS2).
