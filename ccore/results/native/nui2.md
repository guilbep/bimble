# NUI2: the native app by tiles, with exact occlusion: synthetic A/B (committed)

Offscreen, `bimble --offscreen --size 1492x790 --scale 2` (2984x1580, the 3D view 2392x1084), Apple M1 Pro (Asahi,
Mesa Honeykrisp), 27 Nov 2026, other builders running (load 2-7: medians hold, p95 move by a few ms between runs).
Each table is one `--compare` (runs one after the other, the first warms the file cache). "Moving frame" is a frame
drawn while the view moves: input to the GPU's fence of that frame. The owner's project's A/B is in the ticket's
report (private data).

Missing ids come from separate `--truth` runs (each 3D frame drawn without occlusion and without back-face culling,
then as set; an element is missing when a pixel of it shows something farther as set):

| set | settings | frames | frames with ids missing | ids missing | coplanar ties |
|---|---|---|---|---|---|
| 17 synthetic links | tiles, occlusion to groups (8) | 2,196 | 0 | 0 | 365,708 (the frame fixture's touching faces) |
| 18 buildings | tiles, defaults, frames in flight | 3,076 | 0 | 0 | 0 |
| 17 synthetic links | cull_back on, occlusion off | 1,060 | 21 | 42 (21 of 8 px or fewer) | 2 |
| 17 synthetic links | cull_back on, occlusion on | 1,565 | 1,513 | 191,157 | 8,779 |

## 17 synthetic links (`native/bench/make_links.mjs`: 17 x 400 unique closed meshes, 3.93 M triangles; the orbit-zoom gesture after `link_folder`)

Every element is in view and none hides another: occlusion has nothing to remove; the pages draw the same triangles
in 7-11 draws (parts: 35).

| | renderer_path=parts | renderer_path=tiles,draw_visible=false | renderer_path=tiles,occlusion_groups=false | renderer_path=tiles | renderer_path=tiles,still_px=1 | renderer_path=tiles,still_px=1,cull_back=true | 
|---|---|---|---|---|---|---|
| first picture (process start) | 53 ms | 65 ms | 61 ms | 80 ms | 64 ms | 79 ms | 
| complete | 765 ms | 959 ms | 1040 ms | 948 ms | 1048 ms | 903 ms | 
| moving frame median / p95 | 10.8 / 14.4 ms (n 235) | 8.9 / 11.6 ms (n 233) | 8.7 / 11.9 ms (n 235) | 9.3 / 13.1 ms (n 237) | 8.4 / 12.6 ms (n 237) | 8.7 / 11.7 ms (n 235) | 
| still frame median / p95 | 6.5 / 12.6 ms (n 41) | 7.1 / 13.1 ms (n 69) | 7.3 / 13.1 ms (n 67) | 7.1 / 13.2 ms (n 66) | 8.7 / 15.7 ms (n 64) | 7.1 / 11.7 ms (n 61) | 
| input -> frame median / p95 / max | 11.2 / 15.1 / 18.7 ms (n 238) | 9.5 / 12.7 / 17.2 ms (n 236) | 9.2 / 12.6 / 18.0 ms (n 238) | 9.7 / 13.7 / 15.8 ms (n 240) | 8.7 / 13.2 / 15.3 ms (n 240) | 9.4 / 12.7 / 14.9 ms (n 238) | 
| memory peak / now | 87 / 79 MB | 67 / 67 MB | 69 / 69 MB | 68 / 68 MB | 69 / 69 MB | 68 / 68 MB | 
| triangles moving / still (median) | 3931488 / 1173840 | 3931488 / 2182896 | 3931488 / 2142156 | 3913632 / 2103912 | 3913056 / 1974492 | 3913632 / 2261856 | 
| tiles drawn moving (median) | 0 | 289 | 289 | 289 | 289 | 289 | 
| GPU peak | 118 MB | 218 MB | 218 MB | 218 MB | 218 MB | 218 MB | 
| moving frame by stage, median ms: logic / acquire / UI build / 3D record / UI record / GPU | 0.00 / 0.00 / 0.33 / 2.00 / 0.09 / 8.27 | 0.00 / 0.00 / 0.48 / 0.66 / 0.12 / 7.90 | 0.01 / 0.00 / 0.35 / 0.68 / 0.09 / 7.84 | 0.00 / 0.00 / 0.34 / 0.98 / 0.10 / 8.08 | 0.00 / 0.00 / 0.23 / 0.49 / 0.07 / 7.59 | 0.00 / 0.00 / 0.58 / 1.42 / 0.13 / 7.29 | 

## 18 buildings (`TILES_SYNTH=6,3,10,400 test/run.sh tiles`: 73,260 elements, 6.69 M triangles, a view file opened as it is; preset orbit-zoom)

The parts path draws nothing of a view file opened as it is (no mesh answer for it); the tile path builds its
pyramid after the open (4-6 s: the first picture waits for it).

| | renderer_path=tiles,draw_visible=false | renderer_path=tiles,occlusion_groups=false | renderer_path=tiles | renderer_path=tiles,still_px=1 | renderer_path=tiles,still_px=1,cull_back=true | 
|---|---|---|---|---|---|
| first picture (process start) | 4840 ms | 5080 ms | 4903 ms | 4988 ms | 5006 ms | 
| complete | 4840 ms | 5080 ms | 4903 ms | 4988 ms | 5006 ms | 
| moving frame median / p95 | 12.9 / 16.9 ms (n 235) | 13.3 / 17.3 ms (n 243) | 13.9 / 19.2 ms (n 241) | 13.7 / 18.6 ms (n 246) | 14.6 / 18.2 ms (n 254) | 
| still frame median / p95 | 7.5 / 14.1 ms (n 83) | 9.1 / 14.8 ms (n 80) | 10.7 / 18.7 ms (n 72) | 10.2 / 15.6 ms (n 74) | 11.1 / 17.1 ms (n 71) | 
| input -> frame median / p95 / max | 13.5 / 18.0 / 18.6 ms (n 238) | 14.0 / 18.5 / 20.7 ms (n 230) | 14.2 / 20.1 / 37.0 ms (n 227) | 14.0 / 19.5 / 22.3 ms (n 231) | 15.3 / 18.8 / 26.4 ms (n 231) | 
| memory peak / now | 286 / 249 MB | 267 / 210 MB | 256 / 209 MB | 305 / 225 MB | 285 / 208 MB | 
| triangles moving / still (median) | 6514236 / 3583548 | 6203028 / 3572100 | 5312136 / 3155988 | 5356932 / 3134580 | 5347944 / 3116964 | 
| tiles drawn moving (median) | 529 | 493 | 529 | 529 | 529 | 
| GPU peak | 113 MB | 113 MB | 114 MB | 114 MB | 114 MB | 
| moving frame by stage, median ms: logic / acquire / UI build / 3D record / UI record / GPU | 0.00 / 0.00 / 0.47 / 3.55 / 0.12 / 8.34 | 0.01 / 0.01 / 0.48 / 3.76 / 0.12 / 8.89 | 0.00 / 0.00 / 0.31 / 5.02 / 0.09 / 8.22 | 0.00 / 0.00 / 0.23 / 5.29 / 0.08 / 8.37 | 0.00 / 0.00 / 0.35 / 7.11 / 0.10 / 7.33 | 

Elements a group (`occlusion_group`), still_px 1: every indirect command costs about 1 us of CPU here, the group's
size trades the commands against the triangles:

| | renderer_path=tiles,still_px=1,occlusion_group=8 | renderer_path=tiles,still_px=1,occlusion_group=16 | renderer_path=tiles,still_px=1,occlusion_group=32 | renderer_path=tiles,still_px=1,occlusion_group=64 | 
|---|---|---|---|---|
| first picture (process start) | 8010 ms | 5414 ms | 5979 ms | 6146 ms | 
| complete | 8010 ms | 5414 ms | 5979 ms | 6146 ms | 
| moving frame median / p95 | 11.5 / 17.4 ms (n 254) | 10.0 / 15.6 ms (n 249) | 9.3 / 14.9 ms (n 250) | 10.5 / 16.7 ms (n 252) | 
| still frame median / p95 | 8.5 / 15.4 ms (n 73) | 9.8 / 15.5 ms (n 74) | 7.5 / 14.4 ms (n 74) | 8.2 / 15.4 ms (n 71) | 
| input -> frame median / p95 / max | 12.0 / 17.7 / 21.1 ms (n 233) | 10.2 / 16.0 / 21.0 ms (n 237) | 9.4 / 15.4 / 18.4 ms (n 236) | 11.0 / 17.4 / 20.4 ms (n 232) | 
| memory peak / now | 302 / 209 MB | 271 / 230 MB | 303 / 206 MB | 285 / 210 MB | 
| triangles moving / still (median) | 5135604 / 3003132 | 5663400 / 3209760 | 5846808 / 3270900 | 6006840 / 3328284 | 
| tiles drawn moving (median) | 516 | 529 | 529 | 529 | 
| GPU peak | 114 MB | 114 MB | 114 MB | 114 MB | 
| moving frame by stage, median ms: logic / acquire / UI build / 3D record / UI record / GPU | 0.00 / 0.00 / 0.17 / 1.90 / 0.04 / 8.43 | 0.00 / 0.00 / 0.08 / 1.30 / 0.03 / 8.28 | 0.00 / 0.00 / 0.08 / 1.12 / 0.03 / 7.73 | 0.00 / 0.00 / 0.12 / 1.06 / 0.03 / 8.10 | 

Frames in flight (`frames_in_flight`, defaults otherwise):

| | frames_in_flight=1 | frames_in_flight=2 | 
|---|---|---|
| first picture (process start) | 4121 ms | 4259 ms | 
| complete | 4759 ms | 5105 ms | 
| moving frame median / p95 | 8.6 / 13.4 ms (n 268) | 8.9 / 12.4 ms (n 290) | 
| still frame median / p95 | 8.0 / 14.4 ms (n 74) | 7.1 / 11.6 ms (n 197) | 
| input -> frame median / p95 / max | 8.9 / 13.9 / 20.0 ms (n 239) | 9.0 / 12.4 / 16.9 ms (n 243) | 
| memory peak / now | 294 / 224 MB | 307 / 202 MB | 
| triangles moving / still (median) | 5741976 / 3216516 | 5830308 / 3014112 | 
| tiles drawn moving (median) | 522 | 529 | 
| GPU peak | 114 MB | 114 MB | 
| moving frame by stage, median ms: logic / acquire / UI build / 3D record / UI record / GPU | 0.00 / 0.00 / 0.15 / 1.35 / 0.04 / 7.01 | 0.00 / 0.00 / 0.15 / 0.00 / 0.04 / 6.62 | 

## Where the triangles go (18 buildings, six views, `BIMBLE_TILES_SURPLUS=1`)

Leaves' triangles drawn against those of the elements that own a pixel of the frame (the need), with the group's box
from its elements' boxes and the leaf's elements in space order (the commit after the spheres):

| view | need (elements shown) | group 1 | group 8 | group 32 |
|---|---|---|---|---|
| fit (outside) | 0.68 M | 1.26 M | 3.92 M | 5.15 M |
| far | 0.15 M | 0.25 M | 1.94 M | 3.04 M |
| close | 0.07 M | 0.08 M | 0.36 M | 0.65 M |
| inside 1 | 0.00 M | 0.01 M | 0.01 M | 0.01 M |
| inside 2 | 0.06 M | 0.11 M | 0.55 M | 1.03 M |
| inside 3 | 0.02 M | 0.19 M | 0.29 M | 0.23 M |

Every surplus triangle is in a pass 2 group whose box passed the depth pyramid while none of its elements shows
(loose boxes), or in a pass 1 group seen last frame and hidden now; none is a group sharing a shown element (pass 2
groups never hold an element seen the frame before). The groups' cost is the CPU's (a command each):

| | occlusion_group=1 | occlusion_group=2 | occlusion_group=8 | occlusion_group=32 | 
|---|---|---|---|---|
| moving frame median / p95 | 9.0 / 20.7 ms (n 245) | 9.3 / 14.1 ms (n 294) | 8.6 / 13.1 ms (n 303) | 9.5 / 13.3 ms (n 304) | 
| still frame median / p95 | 11.1 / 18.6 ms (n 98) | 7.8 / 14.1 ms (n 106) | 8.9 / 13.0 ms (n 110) | 8.1 / 12.4 ms (n 184) | 
| input -> frame median / p95 / max | 9.3 / 21.4 / 31.4 ms (n 217) | 9.5 / 13.7 / 27.5 ms (n 242) | 8.9 / 13.7 / 19.8 ms (n 240) | 10.0 / 13.8 / 17.9 ms (n 239) | 
| memory peak / now | 278 / 240 MB | 297 / 253 MB | 302 / 222 MB | 277 / 232 MB | 
| triangles moving / still (median) | 611182 / 477288 | 1080758 / 1042344 | 2549022 / 2728044 | 3733850 / 3018936 | 
| GPU peak | 123 MB | 118 MB | 114 MB | 114 MB | 
| moving frame by stage, median ms: logic / acquire / UI build / 3D record / UI record / GPU | 0.00 / 0.00 / 0.13 / 0.00 / 0.03 / 5.64 | 0.00 / 0.00 / 0.14 / 0.00 / 0.04 / 5.45 | 0.01 / 0.01 / 0.32 / 0.00 / 0.06 / 5.80 | 0.01 / 0.01 / 0.47 / 0.00 / 0.08 / 6.35 | 
