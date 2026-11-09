//! `bimble bench`: how fast does the viewer's renderer run on this machine?
//!
//! For each model: build the scene, upload it, then render N frames while orbiting 360°.
//! Every frame waits for the GPU to finish, so times are real GPU+CPU frame times, not just
//! command submission. Uses the same renderer, resolution and MSAA as the viewer.

use bimble_core::demo::{FrameSpec, concrete_frame};
use bimble_geom::Scene;
use bimble_render::{Camera, DrawOptions, Headless, Layer, Overlay, Renderer, Style, Target};
use std::path::PathBuf;
use std::time::{Duration, Instant};

pub struct BenchArgs {
    pub files: Vec<PathBuf>,
    pub frames: usize,
    pub width: u32,
    pub height: u32,
    pub samples: u32,
    pub quick: bool,
    pub occlusion: bool,
    pub json: bool,
}

struct Case {
    name: String,
    elements: usize,
    triangles: u32,
    load: Duration,
    upload: Duration,
    first_frame: Duration,
    frames: Vec<Duration>,
    /// The same orbit drawn as while navigating: elements under `MOVING_PX` left out.
    moving: Vec<Duration>,
    /// The orbit at a fifth of the distance, around the model's centre: most of it is off
    /// screen, as when working inside a building (what culling saves).
    close: Vec<Duration>,
    /// Share of the triangles the close-up orbit draws (culling, ADR 0036), on average.
    close_drawn: f64,
    /// Geometry on the GPU (MB).
    gpu_mb: f64,
    /// Redrawing the highlight of the element under the cursor over the kept picture (what a
    /// hover change costs, ADR 0036 #3; it was a whole frame).
    hover: Vec<Duration>,
    /// Share of the drawn triangles hidden behind others (what perfect occlusion culling
    /// would leave out, ADR 0036 #1), whole model and close up, over 8 views.
    hidden: f64,
    hidden_close: f64,
    /// Face draw calls of a whole frame (merged clusters, then one per shared shape).
    draws: usize,
    memory_mb: Option<f64>,
}

/// Elements smaller than this on screen (bounding radius, pixels) are left out while moving.
const MOVING_PX: f32 = 3.0;

/// The close-up orbit's distance: the fitted one divided by this.
const CLOSE: f32 = 5.0;

fn sorted(d: &[Duration]) -> Vec<Duration> {
    let mut v = d.to_vec();
    v.sort();
    v
}

fn percentile(sorted: &[Duration], p: f64) -> Duration {
    if sorted.is_empty() {
        return Duration::ZERO;
    }
    let i = ((sorted.len() - 1) as f64 * p).round() as usize;
    sorted[i]
}

fn memory_mb() -> Option<f64> {
    memory_stats::memory_stats().map(|m| m.physical_mem as f64 / 1_048_576.0)
}

/// Generated concrete frames of about 1k, 5k, 20k and 50k elements (5k and up skipped with --quick).
fn fixtures(quick: bool) -> Vec<(String, FrameSpec)> {
    let spec = |n: usize, storeys: usize| FrameSpec {
        bays_x: n,
        bays_y: n,
        storeys,
        ..Default::default()
    };
    let mut v = vec![spec(10, 3)];
    if !quick {
        v.extend([spec(20, 4), spec(30, 7), spec(40, 10)]);
    }
    v.into_iter()
        .map(|s| (format!("frame {}x{}x{}", s.bays_x, s.bays_y, s.storeys), s))
        .collect()
}

fn run_case(
    gpu: &Headless,
    r: &Renderer,
    target: &Target,
    name: String,
    scene: Scene,
    load: Duration,
    args: &BenchArgs,
) -> Case {
    let t = Instant::now();
    let gs = r.upload(&gpu.device, &gpu.queue, &scene, |_, _| Style::Normal);
    // Make sure the upload itself is finished before timing frames.
    let _ = gpu
        .device
        .poll(bimble_render::wgpu::PollType::wait_indefinitely());
    let upload = t.elapsed();

    let mut cam = Camera::default();
    cam.fit(&scene, args.width as f32 / args.height as f32);
    let frame = |cam: &Camera, min_radius: f32| {
        let opts = DrawOptions {
            min_radius,
            ..DrawOptions {
                occlusion: args.occlusion,
                ..DrawOptions::default()
            }
        };
        let t = Instant::now();
        r.render(&gpu.device, &gpu.queue, target, &gs, cam, &opts);
        let _ = gpu
            .device
            .poll(bimble_render::wgpu::PollType::wait_indefinitely());
        t.elapsed()
    };
    let first_frame = frame(&cam, 0.0);
    let n = args.frames.max(1);
    let step = std::f32::consts::TAU / n as f32;
    let frames = (0..n)
        .map(|_| {
            cam.orbit(step, 0.0);
            frame(&cam, 0.0)
        })
        .collect();
    let moving = (0..n)
        .map(|_| {
            cam.orbit(step, 0.0);
            frame(&cam, cam.mm_per_pixel(target.size.1) * MOVING_PX)
        })
        .collect();
    let hidden_over = |cam: &mut Camera| {
        let mut sum = 0.0;
        for _ in 0..8 {
            cam.orbit(std::f32::consts::TAU / 8.0, 0.0);
            sum += r.hidden_share(
                &gpu.device,
                &gpu.queue,
                target,
                &gs,
                cam,
                &DrawOptions {
                    occlusion: args.occlusion,
                    ..DrawOptions::default()
                },
            );
        }
        sum / 8.0
    };
    let hidden = hidden_over(&mut cam);
    let fit = cam.distance;
    cam.distance = fit / CLOSE;
    let aspect = args.width as f32 / args.height as f32;
    let mut drawn = 0u64;
    let close = (0..n)
        .map(|_| {
            cam.orbit(step, 0.0);
            drawn += gs.visible_triangles(
                &cam,
                aspect,
                &DrawOptions {
                    occlusion: args.occlusion,
                    ..DrawOptions::default()
                },
            );
            frame(&cam, 0.0)
        })
        .collect();
    let close_drawn = drawn as f64 / n as f64 / gs.triangles().max(1) as f64;
    let hidden_close = hidden_over(&mut cam);
    // Hover: the middle element highlighted over the last close-up picture.
    let k = scene.elements.len() / 2;
    let hi = r.upload(&gpu.device, &gpu.queue, &scene, |j, _| {
        if j == k { Style::Normal } else { Style::Hidden }
    });
    let overlay = Overlay::new(&gpu.device, target.size, args.samples);
    let opts = DrawOptions {
        occlusion: args.occlusion,
        ..DrawOptions::default()
    };
    let hover = (0..n)
        .map(|_| {
            let t = Instant::now();
            r.render_overlay(
                &gpu.device,
                &gpu.queue,
                target,
                &overlay,
                &[Layer::highlight(&hi, bimble_render::Rgb([230, 20, 20]))],
                &gs.bounds,
                &cam,
                &opts,
            );
            let _ = gpu
                .device
                .poll(bimble_render::wgpu::PollType::wait_indefinitely());
            t.elapsed()
        })
        .collect();
    Case {
        name,
        elements: scene.elements.len(),
        triangles: gs.triangles(),
        load,
        upload,
        first_frame,
        frames,
        moving,
        close,
        close_drawn,
        hover,
        hidden,
        hidden_close,
        gpu_mb: gs.gpu_bytes() as f64 / 1_048_576.0,
        draws: gs.face_draws(),
        memory_mb: memory_mb(),
    }
}

pub fn run(args: BenchArgs) -> Result<(), String> {
    let gpu = Headless::new().map_err(|e| e.to_string())?;
    let max = gpu.max_target_size();
    if args.width == 0 || args.height == 0 || args.width > max || args.height > max {
        return Err(format!(
            "--width and --height must be between 1 and {max} on this GPU"
        ));
    }
    let r = Renderer::new(&gpu.device, args.samples);
    let target = Target::new(&gpu.device, (args.width, args.height), args.samples);

    let mut cases = Vec::new();
    for (name, spec) in fixtures(args.quick) {
        let t = Instant::now();
        let scene = Scene::from_model(&concrete_frame(&spec));
        let load = t.elapsed();
        eprintln!("running {name} ({} elements)…", scene.elements.len());
        cases.push(run_case(&gpu, &r, &target, name, scene, load, &args));
    }
    for f in &args.files {
        let t = Instant::now();
        let scene = crate::load_scene(f)?;
        let load = t.elapsed();
        // Report the file by its extension only: real project names stay private when pasted.
        let name = format!(
            "file #{} ({})",
            cases.len() + 1,
            f.extension().and_then(|e| e.to_str()).unwrap_or("?")
        );
        eprintln!("running {name} ({} elements)…", scene.elements.len());
        cases.push(run_case(&gpu, &r, &target, name, scene, load, &args));
    }

    let info = &gpu.adapter_info;
    let system = format!(
        "{} {} · {} CPU threads · GPU: {} ({:?}, {:?}) · driver: {} {}",
        std::env::consts::OS,
        std::env::consts::ARCH,
        std::thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(0),
        info.name,
        info.device_type,
        info.backend,
        info.driver,
        info.driver_info
    );

    if args.json {
        let cases_json: Vec<_> = cases
            .iter()
            .map(|c| {
                let mut s = c.frames.clone();
                s.sort();
                let ms = |d: Duration| d.as_secs_f64() * 1000.0;
                serde_json::json!({
                    "name": c.name, "elements": c.elements, "triangles": c.triangles,
                    "load_ms": ms(c.load), "upload_ms": ms(c.upload), "first_frame_ms": ms(c.first_frame),
                    "frame_median_ms": ms(percentile(&s, 0.5)), "frame_p95_ms": ms(percentile(&s, 0.95)),
                    "frame_max_ms": ms(*s.last().unwrap()), "memory_mb": c.memory_mb,
                    "moving_median_ms": ms(percentile(&sorted(&c.moving), 0.5)),
                    "close_median_ms": ms(percentile(&sorted(&c.close), 0.5)),
                    "close_drawn": c.close_drawn, "hidden": c.hidden, "hidden_close": c.hidden_close,
                    "hover_median_ms": ms(percentile(&sorted(&c.hover), 0.5)), "gpu_mb": c.gpu_mb, "draws": c.draws,
                })
            })
            .collect();
        let j = serde_json::json!({
            "bimble": env!("CARGO_PKG_VERSION"), "system": system,
            "resolution": [args.width, args.height], "msaa": args.samples, "frames": args.frames,
            "cases": cases_json,
        });
        println!("{}", serde_json::to_string_pretty(&j).unwrap());
        return Ok(());
    }

    println!("bimble {} benchmark", env!("CARGO_PKG_VERSION"));
    println!("{system}");
    println!(
        "{}×{}, MSAA {}, {} frames per model (orbit 360°, GPU waited each frame)\n",
        args.width, args.height, args.samples, args.frames
    );
    println!(
        "{:<20} {:>9} {:>10} {:>9} {:>9} {:>10} {:>10} {:>8} {:>10} {:>9} {:>8} {:>8} {:>7} {:>7} {:>8} {:>6} {:>9}",
        "model",
        "elements",
        "triangles",
        "load ms",
        "upload",
        "median ms",
        "p95 ms",
        "fps",
        "moving ms",
        "close ms",
        "drawn",
        "hover ms",
        "hidden",
        "hid.cl",
        "GPU MB",
        "draws",
        "proc MB"
    );
    for c in &cases {
        let mut s = c.frames.clone();
        s.sort();
        let med = percentile(&s, 0.5);
        let p95 = percentile(&s, 0.95);
        let ms = |d: Duration| d.as_secs_f64() * 1000.0;
        println!(
            "{:<20} {:>9} {:>10} {:>9.1} {:>9.1} {:>10.2} {:>10.2} {:>8.0} {:>10.2} {:>9.2} {:>7.0}% {:>8.2} {:>6.0}% {:>6.0}% {:>8.0} {:>6} {:>9}",
            c.name,
            c.elements,
            c.triangles,
            ms(c.load),
            ms(c.upload),
            ms(med),
            ms(p95),
            1000.0 / ms(med).max(1e-3),
            ms(percentile(&sorted(&c.moving), 0.5)),
            ms(percentile(&sorted(&c.close), 0.5)),
            c.close_drawn * 100.0,
            ms(percentile(&sorted(&c.hover), 0.5)),
            c.hidden * 100.0,
            c.hidden_close * 100.0,
            c.gpu_mb,
            c.draws,
            c.memory_mb.map_or("n/a".into(), |m| format!("{m:.0}")),
        );
    }
    println!(
        "\nfps = 1000 / median frame time. moving ms = median frame drawn as while navigating \
         (elements under {MOVING_PX} px left out). close ms = the orbit at 1/{CLOSE} of the distance, \
         around the centre; drawn = share of the triangles it draws (culling). \
         hover ms = redrawing one element's highlight over the kept picture. \
         hidden / hid.cl = share of the drawn triangles hidden behind others, whole and close up \
         (what perfect occlusion culling would leave out). GPU MB = geometry \
         buffers; draws = face draw calls of a whole frame. proc MB = memory of this process after the model \
         (cumulative)."
    );
    println!("The viewer only redraws on change, so idle costs nothing.");
    Ok(())
}
