#!/usr/bin/env python
"""
Standalone projector-film renderer.

Applies the game's projector look (gate weave + film-transport jitter, curtain
ripple, chromatic aberration, halation / focus breathing, lamp flicker, gate
dust, hair in the gate, emulsion scratch, vignette) plus the feathered ragged
projector gate and a full-frame film grain to any image or video, and writes
out a new file at FHD / 2K / 4K (or any custom size).

The source is upscaled with plain bilinear filtering to fit the projected
plate -- picture detail is whatever the source had; the *shader* runs at the
full output resolution, so the grain, gate edge and dust are crisp.

Requires: moderngl, numpy, and ffmpeg/ffprobe on PATH.

Examples
--------
    # 720p clip -> 4K projected film
    python projector_render.py in.mp4 -o out.mp4 --res 4k

    # a still, held for 8 seconds at 24 fps
    python projector_render.py still.png -o out.mp4 --res 2k --duration 8

    # a still -> a single PNG (no motion effects animate, but the look applies)
    python projector_render.py still.png -o out.png

    # tighter plate, letterboxed instead of cropped, brighter wall
    python projector_render.py in.mov -o out.mp4 --res fhd \
        --margin 0.88 --fit contain --wall '#0a0a0c'
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys

import numpy as np

try:
    import moderngl
except ImportError:  # pragma: no cover
    sys.exit("moderngl is required:  pip install moderngl")


# --------------------------------------------------------------------------- #
# presets
# --------------------------------------------------------------------------- #

RES_PRESETS = {
    "hd": (1280, 720),
    "fhd": (1920, 1080),
    "1080p": (1920, 1080),
    "2k": (2560, 1440),
    "1440p": (2560, 1440),
    "dci2k": (2048, 1080),
    "4k": (3840, 2160),
    "2160p": (3840, 2160),
    "dci4k": (4096, 2160),
}

IMAGE_EXT = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".webp", ".tga"}


def parse_res(text: str) -> tuple[int, int]:
    key = text.strip().lower()
    if key in RES_PRESETS:
        return RES_PRESETS[key]
    for sep in ("x", "X", ":", ","):
        if sep in key:
            w, _, h = key.partition(sep)
            return int(w), int(h)
    raise argparse.ArgumentTypeError(f"unrecognised resolution: {text!r}")


def parse_color(text: str) -> tuple[float, float, float]:
    s = text.strip().lstrip("#")
    if len(s) == 3:
        s = "".join(c * 2 for c in s)
    if len(s) != 6:
        raise argparse.ArgumentTypeError(f"bad colour: {text!r}")
    return tuple(int(s[i:i + 2], 16) / 255.0 for i in (0, 2, 4))  # type: ignore[return-value]


# --------------------------------------------------------------------------- #
# shaders
# --------------------------------------------------------------------------- #

VERTEX = """
#version 330
in vec2 in_pos;
out vec2 v_uv;
void main() {
    v_uv = in_pos * 0.5 + 0.5;          // 0..1, y = 0 at bottom
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
"""

# Pass 1 -- the projected plate. Ported from game/projector.rpy ("somber.projector")
# with the CPU-side externs of game/projector.frag (time / flicker / frame /
# frameSize) derived from u_time exactly as the Ren'Py port does.
PLATE_FRAG = """
#version 330

in vec2 v_uv;
out vec4 f_color;

uniform sampler2D tex0;
uniform vec2  u_out_size;      // full output framebuffer, px
uniform vec2  u_plate_org;     // plate rect origin (left, TOP), px, y down
uniform vec2  u_plate_size;    // plate rect size, px
uniform vec2  u_src_scale;     // source fit: uv -> texture uv
uniform vec2  u_src_offset;
uniform float u_time;
uniform float u_intensity;     // 0..1 master mix of the whole look
// Per-effect amounts; 1.0 is the look as written, 0 removes that effect alone.
uniform float u_weave;
uniform float u_ripple;
uniform float u_aber;
uniform float u_halation;
uniform float u_flicker;
uniform float u_dust;
uniform float u_hair;
uniform float u_scratch;
uniform float u_vignette;

float hash1(float n) { return fract(sin(n) * 43758.5453); }

float hash2(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// The plate covers only part of the frame; every Texel()/texture2D() call in the
// original sampled the plate's own 0..1 space, so route them all through here.
vec4 sampleSrc(vec2 uv) {
    return texture(tex0, clamp(uv, 0.0, 1.0) * u_src_scale + u_src_offset);
}

void main() {
    // Plate-local uv with y = 0 at the TOP, matching Ren'Py's a_tex_coord.
    vec2 px = vec2(v_uv.x * u_out_size.x, (1.0 - v_uv.y) * u_out_size.y);
    vec2 uv0 = (px - u_plate_org) / u_plate_size;
    if (uv0.x < 0.0 || uv0.x > 1.0 || uv0.y < 0.0 || uv0.y > 1.0) {
        f_color = vec4(0.0);
        return;
    }

    vec2  frameSize = u_plate_size;
    float time      = u_time;

    // Discrete projector frame index (film runs ~18 fps).
    float frame = floor(time * 18.0);

    // Lamp brightness this frame.
    float dip = step(0.985, hash1(frame * 7.1 + 2.0));
    float flicker = 0.992 + 0.008 * hash1(frame * 12.9898)
                  + 0.002 * sin(time * 0.7) * sin(time * 1.7);
    flicker -= 0.005 * dip;
    flicker = mix(1.0, flicker, u_flicker);

    float aspect = frameSize.x / max(frameSize.y, 1.0);

    // --- gate weave -------------------------------------------------------
    vec2 uv = uv0;
    uv += vec2(sin(time * 3.0) * 0.00004,
               (sin(time * 5.0) * 0.5 + sin(time * 2.1)) * 0.00008) * u_weave;

    // Discrete film-transport jitter: fresh sub-pixel offset each frame.
    vec2 jitter = vec2(hash1(frame * 4.17 + 1.3),
                       hash1(frame * 6.91 + 8.7)) - 0.5;
    uv += jitter * vec2(0.00065, 0.00050) * u_weave;
    uv.y += dip * (hash1(frame * 3.9) - 0.5) * 0.00025 * u_weave;

    // --- curtain ripple ---------------------------------------------------
    vec2 cuv = uv;
    float rippleScaleX = 1.0 / max(aspect, 1.0);
    cuv.x += (sin(uv.x * 3.0 + time * 0.65) * 0.00035
            + sin(uv.x * 1.6 - time * 0.38) * 0.00045) * rippleScaleX * u_ripple;
    cuv.y += sin(uv.x * 2.2 + time * 0.50) * 0.00015 * u_ripple;

    vec4 t = sampleSrc(cuv);
    vec3 clean = t.rgb;
    float a = t.a;

    // --- lens chromatic aberration ----------------------------------------
    vec2 rc  = (cuv - 0.5) * vec2(aspect, 1.0);
    vec2 cav = (cuv - 0.5) * dot(rc, rc) * 0.0035 * u_aber;
    t.r = sampleSrc(cuv - cav).r;
    t.b = sampleSrc(cuv + cav).b;

    // --- soft ring blur: halation + focus breathing -----------------------
    vec3 blur = t.rgb;
    for (int i = 0; i < 6; i++) {
        float ang = float(i) * 1.0471976;      // 2*pi / 6
        vec2  o   = vec2(cos(ang), sin(ang)) * 4.5 / frameSize;
        blur += sampleSrc(cuv + o).rgb;
    }
    blur /= 7.0;

    float breath = 0.5 + 0.5 * sin(time * 0.31 + sin(time * 0.127) * 2.0);
    t.rgb = mix(t.rgb, blur, (0.05 + 0.10 * breath + 0.05 * dip) * u_halation);

    float lum = dot(blur, vec3(0.299, 0.587, 0.114));
    t.rgb += blur * vec3(1.06, 1.0, 0.90) * smoothstep(0.55, 0.95, lum)
           * 0.18 * u_halation;

    // --- luminance lift + lamp flicker ------------------------------------
    t.rgb = t.rgb * 1.12 + 0.035;
    t.rgb *= flicker;

    // --- gate dust: dark specks that live for exactly one frame -----------
    vec2 duv  = vec2(cuv.x * aspect, cuv.y);
    vec2 cell = floor(duv * 14.0);
    float seed = hash2(cell + vec2(frame * 0.613, frame * 0.269));
    if (seed > 0.955 && u_dust > 0.0) {
        vec2 sp = vec2(hash2(cell + vec2(frame * 0.83, 1.7)),
                       hash2(cell + vec2(2.9, frame * 0.51)));
        float rr = 0.04 + 0.10 * hash2(cell + vec2(frame * 0.37, 5.3));
        float speck = 1.0 - smoothstep(rr * 0.1, rr, length(fract(duv * 14.0) - sp));
        t.rgb *= 1.0 - speck * 0.05 * u_dust;
    }

    // --- a hair in the gate ------------------------------------------------
    float hseed = hash1(frame * 4.451);
    if (hseed > 0.90 && u_hair > 0.0) {
        float side  = step(0.5, hash1(frame * 7.9));
        float reach = 0.12 + 0.30 * hash1(frame * 2.63);
        float depth = mix(cuv.y, 1.0 - cuv.y, side);
        float wob = sin(cuv.y * 17.0 + frame * 1.3) * 0.006
                  + sin(cuv.y * 41.0 + frame) * 0.002;
        float hd = abs(cuv.x - (0.08 + 0.84 * hash1(frame * 9.77)) - wob);
        float hair = (1.0 - smoothstep(0.0006, 0.0022, hd))
                   * (1.0 - smoothstep(reach * 0.6, reach, depth));
        t.rgb *= 1.0 - hair * 0.55 * u_hair;
    }

    // --- emulsion scratch: bursts a few seconds apart ----------------------
    float bwin = floor(time * 0.37);
    if (hash1(bwin * 17.3) > 0.72 && u_scratch > 0.0) {
        float sx = 0.1 + 0.8 * hash1(bwin * 5.1)
                 + (hash1(frame * 3.17) - 0.5) * 0.006;
        float scr = 1.0 - smoothstep(0.0004, 0.0014, abs(uv.x - sx));
        t.rgb = mix(t.rgb, t.rgb * 0.7 + vec3(0.12),
                    scr * 0.5 * (0.4 + 0.6 * hash1(frame * 8.13)) * u_scratch);
    }

    // --- light vignette ----------------------------------------------------
    vec2 vc = abs(cuv - 0.5) * 2.0;
    float vigDist = pow(vc.x, 2.5) + pow(vc.y, 2.5);
    float vig = clamp(1.0 - vigDist * 0.7, 0.0, 1.0);
    vig = vig * vig;
    t.rgb *= mix(mix(1.0, 0.50, u_vignette), 1.0, vig);

    // Master intensity: dial the whole post-process back toward the plain plate.
    t.rgb = mix(clean, t.rgb, u_intensity);

    a *= mix(1.0, 0.9, u_intensity);       // the plate's faint transparency
    f_color = vec4(t.rgb * a, a);          // premultiplied
}
"""

# Pass 2 -- the projector gate ("somber.gateclip") masking the plate, composited
# over the dark wall, finished with the full-frame grain ("somber.grain").
GATE_FRAG = """
#version 330

in vec2 v_uv;
out vec4 f_color;

uniform sampler2D tex0;        // pass 1, premultiplied
uniform vec2  u_out_size;
uniform vec2  u_ap;            // aperture HALF-size, px
uniform float u_time;
uniform vec3  u_wall;
uniform float u_grain;         // grain strength (0 disables)
uniform float u_grain_fps;
uniform int   u_gate_on;

float gate_h1(float n) { return fract(sin(n) * 43758.5453); }

// Dave Hoskins' sine-free hash: no spatial tiling/lattice.
float hash13(vec3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec2 tc = vec2(v_uv.x, 1.0 - v_uv.y);       // y down, like a_tex_coord
    vec2 frameSize = u_out_size;
    float frame = floor(u_time * 18.0);

    vec4 t = texture(tex0, v_uv);               // premultiplied plate

    float inside = 1.0;
    if (u_gate_on != 0) {
        // Rounded-rect signed distance in screen px, centred, half-size u_ap.
        vec2  p      = (tc - 0.5) * frameSize;
        vec2  halfSz = u_ap;
        float baseR  = 12.0;
        float soft   = 2.0;
        vec2  sgn    = step(0.0, p);
        float cid    = sgn.x * 2.0 + sgn.y;
        float rad    = baseR * (0.9 + 0.2 * fract(sin(cid * 91.7 + 3.1) * 43758.5453));
        vec2  q      = abs(p) - (halfSz - vec2(2.5) - vec2(soft) - vec2(rad));
        float dist   = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;

        vec2 np = p / max(halfSz, vec2(1.0));
        float ragged = sin(np.x * 5.0 + 1.7) + sin(np.y * 6.0 + 0.5)
                     + 0.4 * sin(np.x * 11.0 - 0.3) + 0.4 * sin(np.y * 9.0 + 2.1);
        dist += ragged * 0.6 + (gate_h1(frame * 5.71) - 0.5) * 0.55;

        inside = 1.0 - smoothstep(-soft, soft, dist);
    }

    // Un-premultiply onto the wall: wall * (1 - a) + rgb.
    float a   = t.a * inside;
    vec3  col = t.rgb * inside + u_wall * (1.0 - a);

    // --- full-frame film grain over everything -----------------------------
    if (u_grain > 0.0) {
        vec2  gpx  = floor(tc * u_out_size);
        float step_t = floor(u_time * u_grain_fps);
        float g = hash13(vec3(gpx, step_t)) - 0.5;          // -0.5 .. 0.5
        float ga = abs(g) * u_grain;
        vec3 gc = (g > 0.0) ? vec3(1.0) : vec3(0.0);
        col = col * (1.0 - ga) + gc * ga;
    }

    f_color = vec4(clamp(col, 0.0, 1.0), 1.0);
}
"""


# --------------------------------------------------------------------------- #
# ffmpeg helpers
# --------------------------------------------------------------------------- #

def need(exe: str) -> str:
    path = shutil.which(exe)
    if not path:
        sys.exit(f"{exe} not found on PATH")
    return path


def probe(path: str) -> dict:
    out = subprocess.run(
        [need("ffprobe"), "-v", "error", "-show_streams", "-show_format",
         "-of", "json", path],
        capture_output=True, text=True,
    )
    if out.returncode != 0:
        sys.exit(f"ffprobe failed on {path}:\n{out.stderr.strip()}")
    return json.loads(out.stdout)


def source_info(path: str) -> tuple[int, int, float, int | None, bool]:
    """Return (width, height, fps, nb_frames|None, has_audio)."""
    info = probe(path)
    vs = next((s for s in info["streams"] if s.get("codec_type") == "video"), None)
    if vs is None:
        sys.exit(f"no video/image stream in {path}")
    w, h = int(vs["width"]), int(vs["height"])

    num, _, den = str(vs.get("avg_frame_rate", "0/0")).partition("/")
    try:
        fps = float(num) / float(den) if float(den) else 0.0
    except ValueError:
        fps = 0.0

    nb = vs.get("nb_frames")
    nb = int(nb) if nb and nb.isdigit() else None
    if nb is None:
        dur = info.get("format", {}).get("duration")
        if dur and fps > 0:
            try:
                nb = int(round(float(dur) * fps))
            except ValueError:
                nb = None

    has_audio = any(s.get("codec_type") == "audio" for s in info["streams"])
    return w, h, fps, nb, has_audio


# --------------------------------------------------------------------------- #
# geometry
# --------------------------------------------------------------------------- #

def plate_rect(out_w: int, out_h: int, src_w: int, src_h: int,
               margin: float, plate_ar: float | None) -> tuple[float, float, float, float]:
    """Plate rect (x, y_top, w, h) in output px: source AR fitted inside the
    margin box and centred. The scene plate is a film slide sized to its own
    content aspect ratio."""
    box_w, box_h = out_w * margin, out_h * margin
    ar = plate_ar if plate_ar else (src_w / max(src_h, 1))
    w = box_w
    h = w / ar
    if h > box_h:
        h = box_h
        w = h * ar
    return (out_w - w) / 2.0, (out_h - h) / 2.0, w, h


def fit_uv(plate_w: float, plate_h: float, src_w: int, src_h: int,
           mode: str) -> tuple[tuple[float, float], tuple[float, float]]:
    """uv -> texture-uv transform (scale, offset) for cover / contain / stretch."""
    if mode == "stretch":
        return (1.0, 1.0), (0.0, 0.0)

    plate_ar = plate_w / max(plate_h, 1.0)
    src_ar = src_w / max(src_h, 1)
    sx = sy = 1.0
    if mode == "cover":
        if src_ar > plate_ar:          # source wider: crop its sides
            sx = plate_ar / src_ar
        else:                          # source taller: crop top/bottom
            sy = src_ar / plate_ar
    else:                              # contain: fit inside, pad (shows wall)
        if src_ar > plate_ar:
            sy = src_ar / plate_ar
        else:
            sx = plate_ar / src_ar
    return (sx, sy), ((1.0 - sx) / 2.0, (1.0 - sy) / 2.0)


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Apply the projector film look (plate shader + gate + grain) "
                    "to any image or video and render it out.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Examples")[-1],
    )
    p.add_argument("input", help="source image or video")
    p.add_argument("-o", "--output", required=True,
                   help="output file (.mp4/.mov/.mkv/.webm, or .png for a still)")
    p.add_argument("--res", type=parse_res, default="fhd",
                   help="output size: hd/fhd/2k/dci2k/4k/dci4k or WxH (default fhd)")
    p.add_argument("--fit", choices=("cover", "contain", "stretch"), default="cover",
                   help="how the source fills the plate (default cover)")
    p.add_argument("--margin", type=float, default=0.94,
                   help="plate size as a fraction of the frame (default 0.94)")
    p.add_argument("--plate-ar", type=float, default=None,
                   help="force the plate aspect ratio (e.g. 2.39); default = source AR")
    p.add_argument("--gate-inset", type=float, default=0.0,
                   help="shrink the gate aperture inside the plate by N px (default 0)")
    p.add_argument("--no-gate", action="store_true",
                   help="skip the ragged gate mask (hard plate edge)")
    p.add_argument("--intensity", type=float, default=1.0,
                   help="master strength of the plate post-process, 0..1 (default 1)")
    p.add_argument("--grain", type=float, default=0.16,
                   help="full-frame grain strength, 0 disables (default 0.16)")
    p.add_argument("--grain-fps", type=float, default=24.0,
                   help="grain step rate (default 24)")
    p.add_argument("--wall", type=parse_color, default="#000000",
                   help="colour behind the gate (default #000000)")
    p.add_argument("--fps", type=float, default=None,
                   help="output fps (default: source fps, or 24 for stills)")
    p.add_argument("--duration", type=float, default=None,
                   help="seconds to render (stills: default 5; videos: trim)")
    p.add_argument("--start", type=float, default=0.0,
                   help="seek this many seconds into the source first")
    for name, help_text in (("weave",      "gate weave and transport jitter"),
                            ("ripple",     "curtain ripple"),
                            ("aberration", "lens chromatic aberration"),
                            ("halation",   "ring blur, focus breathing, glow"),
                            ("flicker",    "lamp flicker"),
                            ("dust",       "gate dust specks"),
                            ("hair",       "hair in the gate"),
                            ("scratch",    "emulsion scratch"),
                            ("vignette",   "corner falloff")):
        p.add_argument("--" + name, type=float, default=1.0,
                       help=help_text + " amount, 1.0 = as designed (default 1.0)")
    p.add_argument("--time-offset", type=float, default=0.0,
                   help="shader clock offset in seconds (reroll the random events)")
    p.add_argument("--crf", type=int, default=16, help="x264 quality (default 16)")
    p.add_argument("--preset", default="slow", help="x264 preset (default slow)")
    p.add_argument("--audio", choices=("copy", "encode", "none"), default="encode",
                   help="what to do with the source audio (default encode)")
    p.add_argument("--no-progress", action="store_true", help="silence progress")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if isinstance(args.res, str):
        args.res = parse_res(args.res)
    if isinstance(args.wall, str):
        args.wall = parse_color(args.wall)

    src = args.input
    if not os.path.isfile(src):
        sys.exit(f"input not found: {src}")
    ffmpeg = need("ffmpeg")

    out_w, out_h = args.res
    if out_w % 2 or out_h % 2:
        sys.exit("output width and height must be even (h264 requirement)")

    is_still_in = os.path.splitext(src)[1].lower() in IMAGE_EXT
    still_out = os.path.splitext(args.output)[1].lower() in IMAGE_EXT

    src_w, src_h, src_fps, src_frames, has_audio = source_info(src)

    fps = args.fps or (src_fps if (src_fps and not is_still_in) else 24.0)
    if still_out:
        n_frames = 1
    elif is_still_in:
        n_frames = max(1, int(round((args.duration or 5.0) * fps)))
    else:
        if args.duration:
            n_frames = max(1, int(round(args.duration * fps)))
        elif src_frames:
            n_frames = max(1, int(round(src_frames * (fps / (src_fps or fps)))))
        else:
            n_frames = 0            # unknown: read until the decoder stops

    # ---- GL setup --------------------------------------------------------- #
    try:
        ctx = moderngl.create_standalone_context(require=330)
    except Exception as exc:                                   # pragma: no cover
        sys.exit(f"could not create an OpenGL 3.3 context: {exc}")

    quad = ctx.buffer(np.array([-1, -1, 3, -1, -1, 3], dtype="f4").tobytes())

    prog_plate = ctx.program(vertex_shader=VERTEX, fragment_shader=PLATE_FRAG)
    prog_gate = ctx.program(vertex_shader=VERTEX, fragment_shader=GATE_FRAG)
    vao_plate = ctx.vertex_array(prog_plate, [(quad, "2f", "in_pos")])
    vao_gate = ctx.vertex_array(prog_gate, [(quad, "2f", "in_pos")])

    src_tex = ctx.texture((src_w, src_h), 4)
    src_tex.filter = (moderngl.LINEAR, moderngl.LINEAR)         # plain upscale
    src_tex.repeat_x = src_tex.repeat_y = False

    plate_tex = ctx.texture((out_w, out_h), 4, dtype="f1")
    plate_tex.filter = (moderngl.NEAREST, moderngl.NEAREST)
    fbo_plate = ctx.framebuffer(color_attachments=[plate_tex])
    fbo_out = ctx.framebuffer(color_attachments=[ctx.texture((out_w, out_h), 3)])

    px, py, pw, ph = plate_rect(out_w, out_h, src_w, src_h, args.margin, args.plate_ar)
    scale, offset = fit_uv(pw, ph, src_w, src_h, args.fit)

    prog_plate["tex0"] = 0
    prog_plate["u_out_size"] = (float(out_w), float(out_h))
    prog_plate["u_plate_org"] = (px, py)
    prog_plate["u_plate_size"] = (pw, ph)
    prog_plate["u_src_scale"] = scale
    prog_plate["u_src_offset"] = offset
    prog_plate["u_intensity"] = float(np.clip(args.intensity, 0.0, 1.0))
    prog_plate["u_weave"]     = float(max(args.weave, 0.0))
    prog_plate["u_ripple"]    = float(max(args.ripple, 0.0))
    prog_plate["u_aber"]      = float(max(args.aberration, 0.0))
    prog_plate["u_halation"]  = float(max(args.halation, 0.0))
    prog_plate["u_flicker"]   = float(max(args.flicker, 0.0))
    prog_plate["u_dust"]      = float(max(args.dust, 0.0))
    prog_plate["u_hair"]      = float(max(args.hair, 0.0))
    prog_plate["u_scratch"]   = float(max(args.scratch, 0.0))
    prog_plate["u_vignette"]  = float(max(args.vignette, 0.0))

    ap_w = max(2.0, pw - 2.0 * args.gate_inset)
    ap_h = max(2.0, ph - 2.0 * args.gate_inset)
    prog_gate["tex0"] = 0
    prog_gate["u_out_size"] = (float(out_w), float(out_h))
    prog_gate["u_ap"] = (ap_w / 2.0, ap_h / 2.0)
    prog_gate["u_wall"] = args.wall
    prog_gate["u_grain"] = max(0.0, args.grain)
    prog_gate["u_grain_fps"] = args.grain_fps
    prog_gate["u_gate_on"] = 0 if args.no_gate else 1

    # ---- decoder ---------------------------------------------------------- #
    dec_cmd = [ffmpeg, "-v", "error", "-nostdin"]
    if args.start > 0:
        dec_cmd += ["-ss", f"{args.start}"]
    dec_cmd += ["-i", src]
    if not is_still_in and abs(fps - (src_fps or fps)) > 1e-6:
        dec_cmd += ["-vf", f"fps={fps}"]
    dec_cmd += ["-map", "0:v:0", "-f", "rawvideo", "-pix_fmt", "rgba", "-"]

    frame_bytes = src_w * src_h * 4
    dec = subprocess.Popen(dec_cmd, stdout=subprocess.PIPE, bufsize=frame_bytes * 2)

    # ---- encoder ---------------------------------------------------------- #
    if still_out:
        enc_cmd = [ffmpeg, "-v", "error", "-y",
                   "-f", "rawvideo", "-pix_fmt", "rgb24",
                   "-s", f"{out_w}x{out_h}", "-i", "-",
                   "-vf", "vflip", "-frames:v", "1", args.output]
    else:
        enc_cmd = [ffmpeg, "-v", "error", "-y",
                   "-f", "rawvideo", "-pix_fmt", "rgb24",
                   "-s", f"{out_w}x{out_h}", "-r", f"{fps}", "-i", "-"]
        want_audio = args.audio != "none" and has_audio and not is_still_in
        if want_audio:
            if args.start > 0:
                enc_cmd += ["-ss", f"{args.start}"]
            enc_cmd += ["-i", src, "-map", "0:v:0", "-map", "1:a:0?", "-shortest"]
            enc_cmd += ["-c:a", "copy"] if args.audio == "copy" else ["-c:a", "aac", "-b:a", "192k"]
        enc_cmd += ["-vf", "vflip", "-c:v", "libx264", "-preset", args.preset,
                    "-crf", str(args.crf), "-pix_fmt", "yuv420p", args.output]
    enc = subprocess.Popen(enc_cmd, stdin=subprocess.PIPE)

    # ---- render loop ------------------------------------------------------ #
    still_buf: bytes | None = None
    written = 0
    # One readback buffer for the whole run. fbo.read() hands back a fresh bytes
    # object every frame - at 1080p that is 6 MB of allocation and free per frame,
    # for pixels that go straight down the pipe.
    out_buf = bytearray(out_w * out_h * 3)
    out_view = memoryview(out_buf)
    try:
        while True:
            if n_frames and written >= n_frames:
                break

            if is_still_in and still_buf is not None:
                buf = still_buf
            else:
                buf = dec.stdout.read(frame_bytes)              # type: ignore[union-attr]
                if len(buf) < frame_bytes:
                    if is_still_in and still_buf is None:
                        sys.exit("could not decode the input image")
                    break
                if is_still_in:
                    still_buf = buf

            src_tex.write(buf)

            t = args.time_offset + written / fps

            src_tex.use(0)
            fbo_plate.use()
            ctx.clear(0.0, 0.0, 0.0, 0.0)
            prog_plate["u_time"] = t
            vao_plate.render(moderngl.TRIANGLES)

            plate_tex.use(0)
            fbo_out.use()
            prog_gate["u_time"] = t
            vao_gate.render(moderngl.TRIANGLES)

            fbo_out.read_into(out_view, components=3)
            enc.stdin.write(out_view)                          # type: ignore[union-attr]
            written += 1

            if not args.no_progress and written % 24 == 0:
                total = f"/{n_frames}" if n_frames else ""
                print(f"\r  {written}{total} frames", end="", flush=True)
    finally:
        if dec.stdout:
            dec.stdout.close()
        dec.wait()
        if enc.stdin:
            enc.stdin.close()
        enc.wait()

    if not args.no_progress:
        print(f"\r  {written} frames -> {args.output} "
              f"({out_w}x{out_h}, plate {pw:.0f}x{ph:.0f})    ")

    if enc.returncode != 0:
        sys.exit(f"ffmpeg encode failed (exit {enc.returncode})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
