R"SIGNAL(
// Shared HLSL body; the offline renderer translates these simple expressions to GLSL.
// Only this body reaches the GLSL export path, so every helper it uses lives here.
// PCG3D integer hash. The float sin/frac hashes this replaced ran out of precision on
// large inputs (pixel cells plus a seed near a thousand, field counts that grow with
// time) and folded back onto themselves, which is what made the noise visibly repeat.
// Integers wrap exactly on every GPU, so D3D and GL agree bit for bit.
uint3 signalPcg(uint3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
float signalCell(float3 p) {
    return float(signalPcg(uint3(int3(floor(p)))).x) / 4294967295.0;
}
// Callers pass fractional multiples too, so the fraction is folded into the third
// axis instead of being truncated away.
float signalHash(float2 p) {
    float z = floor(frac(p.x) * 2048.0) + floor(frac(p.y) * 2048.0) * 2053.0;
    return signalCell(float3(floor(p), z));
}
// Hashed value noise with a smooth blend between cells: grain with a size of its own
// instead of a fizz exactly one output pixel wide, which is what an encoder smears.
// The seed is a hash axis, never an offset added to the coordinate.
float valueNoise(float2 p, float seed) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = signalCell(float3(i, seed));
    float b = signalCell(float3(i + float2(1.0, 0.0), seed));
    float c = signalCell(float3(i + float2(0.0, 1.0), seed));
    float d = signalCell(float3(i + float2(1.0, 1.0), seed));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
// Two octaves on rotated lattices: a single value-noise lattice shows its axis-aligned
// cell grid, a second one at an unrelated angle and scale hides it. Rescaled so the
// spread matches one octave.
float signalNoise(float2 p, float field) {
    float seed = fmod(field, 65536.0);
    float2 r = float2(0.8 * p.x - 0.6 * p.y, 0.6 * p.x + 0.8 * p.y) * 1.93 + 31.7;
    float n = valueNoise(p, seed) * 0.62 + valueNoise(r, seed + 70001.0) * 0.38;
    return 0.5 + (n - 0.5) * 1.35;
}
// A 525-line set carries 480 active lines with SQUARE pixels, so the horizontal
// sample count follows the glass's own aspect: 640x480 on 4:3, 853x480 on 16:9.
// Deriving it keeps sensor pixels and beam smear from stretching on a wide set.
float2 signalGrid(float2 size) {
    return float2(480.0 * (size.x / size.y), 480.0);
}
// The look is measured on the preview's 540p raster: the frame scaled until its short
// side is 540 pixels. Every size that would otherwise be counted in output pixels - the
// beam's blur, the grain, the rim - is counted on this raster instead, so a 1080p or 4K
// render is the set the preview shows drawn with more pixels, not a finer set with
// thinner lines of its own. The picture it carries keeps the file's full detail.
float2 lookRaster(float2 size) {
    return size * (540.0 / min(size.x, size.y));
}
// A one-pixel feather on the unit box, so cutting a coordinate off at its edge does
// not staircase. The width comes from how fast the coordinate moves per output pixel,
// which is what keeps curved and cropped edges smooth at any resolution.
float boxMask(float2 c) {
    float2 d = min(c, 1.0 - c);
    float2 w = max(fwidth(c), float2(0.00001, 0.00001));
    float2 m = saturate(d / w + 0.5);
    return m.x * m.y;
}
// The source through a small vertical low-pass half a signal line tall, so no single
// point of a sharp source picture can decide what an analog line carries.
float3 signalTapV(float2 uv, float dy) {
    return tex0.Sample(samp, uv - float2(0, dy)).rgb * 0.25
         + tex0.Sample(samp, uv).rgb * 0.50
         + tex0.Sample(samp, uv + float2(0, dy)).rgb * 0.25;
}
float3 signalSample(float2 uv, bool camera, float2 size) {
    float2 grid = signalGrid(size);
    float tx = 1.0 / grid.x;
    float3 c = float3(0, 0, 0);
    if (camera) {
        // A security camera is soft AND rings at every edge on its way down a
        // composite cable. It is an analog picture, not a grid of sensor squares: it
        // is filtered continuously, never snapped to cells, so it stays smooth when
        // the file is shown larger than it was made. It resolves a little more than
        // the tube's own raster (about 600 lines), soft rather than coarse.
        float cx = tx / 1.25;
        float dy = 0.5 / (grid.y * 1.25);
        float3 soft = signalTapV(uv, dy) * 0.34;
        soft += (signalTapV(uv - float2(cx, 0), dy) + signalTapV(uv + float2(cx, 0), dy)) * 0.22;
        soft += (signalTapV(uv - float2(cx * 2.0, 0), dy) + signalTapV(uv + float2(cx * 2.0, 0), dy)) * 0.11;
        float3 wide = (tex0.Sample(samp, uv - float2(cx * 3.5, 0)).rgb
                     + tex0.Sample(samp, uv + float2(cx * 3.5, 0)).rgb) * 0.5;
        float3 ring = soft + (soft - wide) * 0.35;
        float y = dot(ring, float3(0.299, 0.587, 0.114));
        // Cheap sensor, cheap line: the blacks never get down, the whites clip early.
        y = pow(saturate((y - 0.02) * 1.10), 0.94);
        y = 0.045 + y * 0.90;
        c = float3(y * 0.94, y, y * 0.96);
    } else {
        // The tube's own beam spot: a narrow horizontal smear, taken at half-sample
        // steps so a sharp source is low-passed rather than skipped over. Skipping is
        // what left edges crawling and stepped.
        c = tex0.Sample(samp, uv).rgb * 0.28;
        c += (tex0.Sample(samp, uv - float2(tx * 0.6, 0)).rgb
            + tex0.Sample(samp, uv + float2(tx * 0.6, 0)).rgb) * 0.22;
        c += (tex0.Sample(samp, uv - float2(tx * 1.3, 0)).rgb
            + tex0.Sample(samp, uv + float2(tx * 1.3, 0)).rgb) * 0.14;
    }
    return c;
}
// A security camera is a wide-angle lens, so the picture bulges before it ever
// reaches a screen. The lens fills its sensor, though: the distortion belongs in the
// image, not in the shape of the frame. Normalising by the corner term keeps the
// corners pinned and the edges straight, so this reads as a wide lens rather than as
// curved glass.
float2 cameraLens(float2 suv) {
    float bulge = 0.12;                    // wide, not fish-eye: at 0.22 it read as a warp
    float2 lp = suv * 2.0 - 1.0;
    return (lp * (1.0 + bulge * dot(lp, lp)) / (1.0 + 2.0 * bulge)) * 0.5 + 0.5;
}
// Tape and line faults, all horizontal: head switching tears the last few lines of a
// field sideways, every line jitters a little, and the whole picture sways.
float2 cameraShake(float2 suv, float lineIdx, float field) {
    float tear = smoothstep(0.972, 0.998, suv.y);
    suv.x += tear * (signalHash(float2(floor(suv.y * 486.0), field)) - 0.5) * 0.09
           + tear * 0.012;
    suv.x += (signalHash(float2(lineIdx, field)) - 0.5) * 0.0006;
    suv.x += 0.0010 * sin(suv.y * 35.0 + time * 1.4);
    return suv;
}
// How much of a harmonic at x cycles per output pixel may be drawn: the pixel's box
// footprint (a sinc), and a fade to nothing before Nyquist. A box-filtered stripe that
// is still point-sampled past half a cycle per pixel aliases into moire.
float bandLimit(float x) {
    float px = 3.14159265 * x + 0.00001;
    return (sin(px) / px) * (1.0 - smoothstep(0.25, 0.5, x));
}
// The raster, drawn the way a tube draws it. A row of gaussian beams one line apart is
// exactly a Fourier series, 1 + sum 2*exp(-2 pi^2 n^2 sigma^2) cos(2 pi n y), so the
// lines are drawn from that series with every harmonic band-limited to what the pixel
// can carry. The beams fatten where the picture is bright, closing the gaps: dark lines
// in the shadows, a solid glow in the highlights. The picture itself is sampled
// continuously and softened over a line's height, never snapped to line centres - the
// snapping is what stepped diagonals and the stripes are what aliased.
float3 crtRaster(float2 q, float2 picFit, float ly, float linePx, float LINES,
                 float field, bool camera, float2 size) {
    float li = floor(ly);
    // Time-base error: every line lands a whisker off, and once in a while the whole
    // picture skips sideways a beat.
    float tbe = (signalHash(float2(li, field)) - 0.5) * 0.00035
              + step(0.992, signalHash(float2(field, 11.0)))
              * (signalHash(float2(field, 23.0)) - 0.5) * 0.004;
    float2 s = (float2(q.x + tbe, q.y) - 0.5) / picFit + 0.5;
    if (camera) s = cameraShake(cameraLens(s), li, field);
    float dyv = (0.5 / LINES) / picFit.y;
    float3 mid = signalSample(s, camera, size);
    float3 c = signalSample(s - float2(0, dyv), camera, size) * 0.25 + mid * 0.50
             + signalSample(s + float2(0, dyv), camera, size) * 0.25;
    // A whisker of red/blue convergence error, measured in signal samples rather than
    // output pixels so it is the same size at every resolution.
    float2 conv = float2(0.5 / signalGrid(size).x, 0.0);
    c.r += (signalSample(s - conv, camera, size).r - mid.r) * 0.6;
    c.b += (signalSample(s + conv, camera, size).b - mid.b) * 0.6;

    // Beam sigma in line units: 0.30 leaves soft gaps in the shadows, 0.45 closes them
    // in the highlights, so bright areas glow solid rather than striping.
    float luma = sqrt(saturate(dot(c, float3(0.299, 0.587, 0.114))));
    float bw = lerp(0.30, 0.45, luma);
    float mask = 1.0;
    for (int n = 1; n <= 2; n++) {
        float fn = float(n);
        float amp = 2.0 * exp(-19.7392088 * fn * fn * bw * bw);
        mask += amp * bandLimit(fn * linePx) * cos(6.2831853 * fn * (ly - 0.5));
    }
    c *= max(mask, 0.0);

    // Aperture grille: vertical red, green and blue phosphor stripes, one triad about
    // as wide as a line is tall. Each channel is a cosine a third of a triad apart,
    // band-limited like the lines, so a 1080p render shows crisp stripes and a smaller
    // preview melts them into flat colour instead of moire. Mean stays 1, so the grille
    // costs no brightness; it relaxes in the highlights where the phosphor blooms.
    float gx = q.x * LINES * 0.7 * (size.x / size.y);
    float gridAmp = lerp(0.85, 0.45, luma) * bandLimit(fwidth(gx));
    c.r *= 1.0 + gridAmp * cos(6.2831853 * gx);
    c.g *= 1.0 + gridAmp * cos(6.2831853 * (gx - 0.33333));
    c.b *= 1.0 + gridAmp * cos(6.2831853 * (gx - 0.66667));
    return max(c, 0.0);
}
float4 Surveillance(float2 uv) {
    bool camera = lookMode > 1.5;
    bool crt = lookMode < 1.5 || lookMode > 2.5;
    float field = floor(time * 59.94);

    // The set fills the frame unless lookPillar asks for real 4:3 glass, which keeps
    // the full frame height and crops the frame to a centred 4:3 window. At full
    // height that is a straight horizontal crop: no rescaling, pixels stay 1:1.
    float2 winScale = float2(1.0, 1.0);
    if (lookPillar > 0.5) {
        float target = 4.0 / 3.0;
        float frameAR = outSize.x / outSize.y;
        winScale = float2(min(1.0, target / frameAR), min(1.0, frameAR / target));
    }
    // A set is an object in the frame, not a crop of it: hold the glass off the frame
    // edge so its whole rim and rounded corners are visible instead of running off.
    // A raw camera feed is not an object, so it keeps the full frame.
    if (crt) winScale *= 0.94;
    float2 tubeSize = outSize * winScale;
    float2 refTube = lookRaster(outSize) * winScale;     // the same glass, on the raster
    float2 tuv = (uv - 0.5) / winScale + 0.5;
    float inTube = boxMask(tuv);

    // The picture covers the glass rather than sitting in bars inside it, so a frame
    // wider than the set loses its side edges instead of shrinking.
    float2 picFit = float2(1.0, 1.0);
    if (lookPillar > 0.5) {
        float glassAR = tubeSize.x / tubeSize.y;
        float srcAR = outSize.x / outSize.y;
        picFit = float2(max(1.0, srcAR / glassAR), max(1.0, glassAR / srcAR));
    }

    float2 texel = 1.0 / tubeSize;
    float2 p = tuv * 2.0 - 1.0;
    float2 q = tuv;

    // The picture bulges outward on the curved glass, wider than it is tall. That dome
    // is the whole reason a tube reads as an object rather than a flat panel, so it is
    // deliberately NOT normalised the way the camera lens is. Nothing gets cut by it
    // because the glass outline is built in this warped space further down: the edge
    // of the tube is the edge of the bulge.
    float2 bow = float2(0.023, 0.037);
    if (crt) q = (p * (1.0 + bow * dot(p, p))) * 0.5 + 0.5;

    // Scan geometry: one 243-line field, the progressive raster of a console or a
    // computer on a good set, about four pixels a line on 1080p glass. A camera feed on
    // its own has no visible raster.
    float LINES = crt ? 243.0 : 486.0;
    float parity = LINES > 300.0 ? fmod(field, 2.0) : 0.0;
    float ly = q.y * LINES + parity * 0.5;
    float scanLine = floor(ly);
    // How many lines one output pixel spans. The lines and grille are band-limited by
    // it, so a 1080p or 4K render resolves them fully and a preview drawn smaller
    // softens them away rather than aliasing.
    float linePx = fwidth(ly);

    // The raster lives on the glass; the signal it draws lives in the picture area,
    // so sampling moves to its own coordinate here.
    float2 suv = (q - 0.5) / picFit + 0.5;
    if (camera) suv = cameraLens(suv);

    // Where the picture ends, geometrically. Taken before the signal is shaken about,
    // so a torn or jittered line does not drag the edge feather with it.
    float inPic = boxMask(suv);

    float tear = 0.0;
    if (camera) {
        tear = smoothstep(0.972, 0.998, suv.y);
        suv = cameraShake(suv, scanLine, field);
    }

    float3 col = crt ? crtRaster(q, picFit, ly, linePx, LINES, field, camera, tubeSize)
                     : signalSample(suv, camera, tubeSize);

    if (camera) {
        // CCD vertical smear: a genuinely blown-out spot -- a lamp, a window, a
        // reflection -- bleeds a pale column up and down the frame. It is a highlight
        // artefact, so the threshold sits near clipping: an ordinary bright sky must
        // not trip it, or every column smears at once and the frame just gets banded.
        // It also only shows where it has something darker to bleed over.
        float peak = 0.0;
        for (int s = 0; s < 8; s++) {
            float3 cs = tex0.Sample(samp, float2(suv.x, (float(s) + 0.5) / 8.0)).rgb;
            peak = max(peak, dot(cs, float3(0.299, 0.587, 0.114)));
        }
        float local = dot(col, float3(0.299, 0.587, 0.114));
        col += saturate((peak - 0.94) * 12.0) * 0.07 * saturate(1.0 - local * 1.4);

        // Auto-gain hunts over the whole picture, not column by column, so it is a
        // property of time alone here. Driving it from the column peak is what put
        // wide vertical bands across the frame.
        col *= 1.0 + 0.055 * sin(time * 0.9) + 0.035 * sin(time * 0.37 + 1.7);

        float2 grid = signalGrid(tubeSize);
        // Sensor noise at the sensor's own scale, blended between cells: hashed per cell
        // it came out as hard squares that a bigger screen turned into mosaic.
        float noise = (signalNoise(suv * grid * 1.25, field) - 0.5) * 1.3;
        float hum = exp(-pow((frac(suv.y - time * 0.075) - 0.5) / 0.065, 2.0));
        col = col * (0.96 - 0.09 * hum) + noise * (0.055 + 0.06 * (1.0 - col.g));
        float dropout = step(0.998, signalHash(float2(scanLine, floor(field / 2.0))));
        col += dropout * (signalHash(float2(floor(suv.x * grid.x * 0.25), field)) - 0.5) * 0.22;
        // Interlace twitter: alternate lines a touch dimmer, swapping every field. A
        // two-line stripe, so it is band-limited like the tube's lines or it moires.
        col *= 1.0 - 0.035 * bandLimit(0.5 * linePx)
                   * (0.5 + 0.5 * cos(3.14159265 * (ly - 0.5 + field)));

        // The torn band is mistracked tape: noisy, desaturated, and dimmer.
        float tearNoise = signalHash(float2(floor(suv.x * grid.x), field * 31.0)) - 0.5;
        col = lerp(col, float3(0.20, 0.21, 0.21) + tearNoise * 0.34, tear * 0.85);
    }

    if (crt) {
        // No shadow mask: a triad needs three pixels to exist at all, and the raster
        // fits about 360 of them down 500 pixels of glass, so it only averages out flat.

        // Glow: light scatters in the phosphor and the thick faceplate, so everything
        // bright wears a soft halo that fills the gaps between lines and stripes. Taps
        // on a golden-angle spiral fill a disk evenly, weighted by a gaussian. Every tap
        // sits at its own radius, so unlike fixed rings they never stack into ghost
        // outlines, and with no per-pixel rotation there is no speckle either.
        // Taken in linear light (squared) so only real highlights bloom. The radius is
        // in signal samples, the same on 1080p and 4K.
        float2 halR = 20.0 / signalGrid(tubeSize);
        float3 bloom = float3(0, 0, 0);
        float wsum = 0.0;
        for (int i = 0; i < 64; i++) {
            float fi = float(i) + 0.5;
            float r = sqrt(fi / 64.0);
            float ang = fi * 2.3999632;
            float w = exp(-r * r * 4.0);
            float3 t = tex0.Sample(samp, suv + float2(cos(ang), sin(ang)) * halR * r).rgb;
            bloom += t * t * w;
            wsum += w;
        }
        bloom *= 0.85 / wsum;
        // A camera feed is monochrome by the time it reaches the tube, so its glow is too.
        if (camera) bloom = dot(bloom, float3(0.299, 0.587, 0.114)) * float3(1, 1, 1);
        col += bloom * float3(1.0, 0.97, 0.95);

        // A slow mains hum bar rolling up, plus per-field brightness twitter.
        col *= 1.0 - 0.035 * (0.5 + 0.5 * sin((tuv.y - time * 0.061) * 6.2831853));
        col *= 0.972 + 0.028 * signalHash(float2(field * 12.9898, 5.0));

        // Phosphor floor: the tube never shows true black while the set is on.
        col += float3(0.008, 0.009, 0.011);

        // Phosphor grain: the coating is not smooth and the beam lighting it is a
        // stream of electrons, so a lit screen always fizzes a little. Shot noise, so
        // it grows where the beam works hardest. Unlike the rest of the set the grain
        // is a texture of the screen it is watched on, so it stays about two output
        // pixels across at any size - grown with the picture it turns into blotches.
        // The grains blend into each other, so they read as a surface rather than as
        // digital noise - and survive the encode instead of turning to mush.
        float cell = max(2.0, tubeSize.y / 480.0);
        float grain = signalNoise(tuv * tubeSize / cell, field + 7919.0) - 0.5;
        float glow = dot(col, float3(0.299, 0.587, 0.114));
        col += grain * 0.028 * (0.30 + 0.70 * sqrt(saturate(glow)));

        // The room in the glass: a broad, faint reflection high on the dome. It is what
        // puts the picture BEHIND a surface instead of painted on the frame.
        float2 hp = p - float2(-0.42, -0.58);
        col += exp(-dot(hp, hp) * 2.4) * 0.030 + (1.0 - dot(p, p) * 0.5) * 0.006;
    }

    // The tube itself: rounded glass with a dark rim where the beam lands shallow at
    // the edge. This is what stops a set reading as a plain rectangle of scanlines.
    // Only a tube has one -- a camera feed on its own is just the signal.
    float glass = 1.0;
    if (crt) {
        // Measured in the bulged picture space, so the rim and the rounded corners
        // follow the dome round instead of cropping a curved picture with a straight
        // rectangle -- which is what flattened it and clipped the top.
        // In raster pixels, so the rim's wander has the preview's size at any output.
        float2 gp = (q - 0.5) * refTube;
        float shortSide = min(refTube.x, refTube.y);
        float corner = 0.028 * shortSide;
        float2 halfSz = refTube * 0.5 - 3.0;
        float2 gd = abs(gp) - (halfSz - corner);
        float dist = length(max(gd, 0.0)) + min(max(gd.x, gd.y), 0.0) - corner;

        // No real tube has a mathematically exact edge, and an exact edge is precisely
        // what stairsteps. A pixel or so of wander breaks the staircase up. It has to
        // be a slow undulation rather than noise, though -- hashing along the rim just
        // trades a jagged edge for a fizzing one. Three long waves at unrelated periods
        // never quite repeat, so the edge reads as imperfect glass and not as a pattern.
        float wob = sin(gp.x * 0.013 + time * 0.50) * 0.50
                  + sin(gp.y * 0.011 - time * 0.40) * 0.50
                  + sin(gp.x * 0.031 - gp.y * 0.017 + time * 0.27) * 0.35;
        dist += wob;

        // The rim is a wide, shallow falloff, not a dark band: the beam only loses a
        // little as it reaches the edge of the glass.
        float rim = 0.16 * shortSide;
        col *= lerp(0.62, 1.0, 1.0 - smoothstep(-rim, 0.0, dist));
        // Antialias the edge over a raster pixel, or over however many one output pixel
        // spans when that is more, so the curved top and bottom do not staircase.
        float aa = max(fwidth(dist), 1.0);
        glass = 1.0 - smoothstep(-aa, aa, dist);
    }

    // Off the glass there is no set at all, so the surround stays empty and whatever
    // the clip sits on shows through.
    float edge = inTube * inPic * glass;
    float4 original = tex0.Sample(samp, uv);
    float alpha = max(tex0.Sample(samp, saturate(suv)).a * inPic,
                      step(0.5, lookPillar)) * inTube * glass;
    return float4(lerp(original.rgb, saturate(col) * edge, intensity), lerp(original.a, alpha, intensity));
}
)SIGNAL"
