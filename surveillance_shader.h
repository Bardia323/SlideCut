R"SIGNAL(
// Shared HLSL body; the offline renderer translates these simple expressions to GLSL.
// Only this body reaches the GLSL export path, so every helper it uses lives here.
float signalHash(float2 p) {
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}
float signalHash2(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}
// A 525-line set carries 480 active lines with SQUARE pixels, so the horizontal
// sample count follows the glass's own aspect: 640x480 on 4:3, 853x480 on 16:9.
// Deriving it keeps sensor pixels and beam smear from stretching on a wide set.
float2 signalGrid(float2 size) {
    return float2(480.0 * (size.x / size.y), 480.0);
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
float3 signalSample(float2 uv, bool camera, float2 size) {
    float2 grid = signalGrid(size);
    float tx = 1.0 / grid.x;
    float3 c = float3(0, 0, 0);
    if (camera) {
        // A security camera resolves maybe 330 lines and rings at every edge on its
        // way down a composite cable: the picture is soft AND overshoots, not one or
        // the other.
        uv = (floor(uv * grid) + 0.5) / grid;
        float3 soft = tex0.Sample(samp, uv).rgb * 0.34;
        soft += (tex0.Sample(samp, uv - float2(tx, 0)).rgb
               + tex0.Sample(samp, uv + float2(tx, 0)).rgb) * 0.22;
        soft += (tex0.Sample(samp, uv - float2(tx * 2.0, 0)).rgb
               + tex0.Sample(samp, uv + float2(tx * 2.0, 0)).rgb) * 0.11;
        float3 wide = (tex0.Sample(samp, uv - float2(tx * 3.5, 0)).rgb
                     + tex0.Sample(samp, uv + float2(tx * 3.5, 0)).rgb) * 0.5;
        float3 ring = soft + (soft - wide) * 0.35;
        float y = dot(ring, float3(0.299, 0.587, 0.114));
        // Cheap sensor, cheap line: the blacks never get down, the whites clip early.
        y = pow(saturate((y - 0.02) * 1.10), 0.94);
        y = 0.045 + y * 0.90;
        c = float3(y * 0.94, y, y * 0.96);
    } else {
        // The tube's own beam spot: a narrow smear, nothing more.
        c = tex0.Sample(samp, uv).rgb * 0.60;
        c += tex0.Sample(samp, uv - float2(tx, 0)).rgb * 0.20;
        c += tex0.Sample(samp, uv + float2(tx, 0)).rgb * 0.20;
    }
    return c;
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
    // deliberately NOT normalised the way the camera lens below is. Nothing gets cut by
    // it because the glass outline is built in this warped space further down: the edge
    // of the tube is the edge of the bulge.
    float2 bow = float2(0.023, 0.037);
    if (crt) q = (p * (1.0 + bow * dot(p, p))) * 0.5 + 0.5;

    // Fine scan structure has to be band-limited: at small preview sizes 486 lines
    // land under one output pixel each and beat into vertical moire.
    float scanWeight = smoothstep(360.0, 960.0, tubeSize.y);

    // Scan geometry: 486 visible lines carried as two fields half a line apart, so
    // fine vertical detail twitters at 60 Hz the way a live broadcast does.
    float LINES = 486.0;
    float parity = fmod(field, 2.0);
    float ly = q.y * LINES + parity * 0.5;
    float scanLine = floor(ly);
    float dl = frac(ly) - 0.5;

    if (crt) {
        // Time-base error: every line lands a whisker off, and once in a while the
        // whole picture skips sideways a beat.
        float tbe = (signalHash2(float2(scanLine * 0.7, field)) - 0.5) * 0.00035
                  + step(0.992, signalHash(float2(field * 3.7, 11.0)))
                  * (signalHash(float2(field * 9.1, 23.0)) - 0.5) * 0.004;
        // Sample at the scanline centre, so vertical detail is honestly quantised to
        // what 486 lines can carry -- but only once the output can resolve them.
        float2 snapped = float2(q.x + tbe, (scanLine + 0.5 - parity * 0.5) / LINES);
        q = lerp(q, snapped, scanWeight);
    }

    // The raster lives on the glass; the signal it draws lives in the picture area,
    // so sampling moves to its own coordinate here.
    float2 suv = (q - 0.5) / picFit + 0.5;
    float tear = 0.0;

    if (camera) {
        // A security camera is a wide-angle lens, so the picture bulges before it ever
        // reaches a screen. The lens fills its sensor, though: the distortion belongs
        // in the image, not in the shape of the frame. Normalising by the corner term
        // keeps the corners pinned and the edges straight, so this reads as a wide
        // lens rather than as curved glass.
        float bulge = 0.22;
        float2 lp = suv * 2.0 - 1.0;
        suv = (lp * (1.0 + bulge * dot(lp, lp)) / (1.0 + 2.0 * bulge)) * 0.5 + 0.5;
    }

    // Where the picture ends, geometrically. Taken before the signal is shaken about,
    // so a torn or jittered line does not drag the edge feather with it.
    float inPic = boxMask(suv);

    if (camera) {

        // Head switching: the last few lines of a tape field are torn sideways where
        // the heads change over, and never quite line up with the picture above.
        tear = smoothstep(0.972, 0.998, suv.y);
        suv.x += tear * (signalHash(float2(floor(suv.y * LINES), field)) - 0.5) * 0.09
               + tear * 0.012;

        float jitter = signalHash(float2(scanLine, field)) - 0.5;
        suv.x += jitter * 0.0012;
        suv.x += 0.0025 * sin(suv.y * 35.0 + time * 1.4);
    }

    float3 col = signalSample(suv, camera, tubeSize);

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
        float noise = signalHash(floor(suv * grid) + float2(field * 17.0, field * 7.0)) - 0.5;
        float hum = exp(-pow((frac(suv.y - time * 0.075) - 0.5) / 0.065, 2.0));
        col = col * (0.96 - 0.09 * hum) + noise * (0.055 + 0.06 * (1.0 - col.g));
        float dropout = step(0.998, signalHash(float2(scanLine, floor(field / 2.0))));
        col += dropout * (signalHash(float2(floor(suv.x * grid.x * 0.25), field)) - 0.5) * 0.22;
        col *= 1.0 - 0.035 * fmod(scanLine + field, 2.0);

        // The torn band is mistracked tape: noisy, desaturated, and dimmer.
        float tearNoise = signalHash(float2(floor(suv.x * grid.x), field * 31.0)) - 0.5;
        col = lerp(col, float3(0.20, 0.21, 0.21) + tearNoise * 0.34, tear * 0.85);
    }

    if (crt) {
        // A whisker of red/blue convergence error at the shadow mask.
        float2 conv = float2(1.1 * texel.x, 0.0);
        col.r = lerp(col.r, signalSample(suv - conv, camera, tubeSize).r, 0.6 * scanWeight);
        col.b = lerp(col.b, signalSample(suv + conv, camera, tubeSize).b, 0.6 * scanWeight);

        // The beam is a gaussian spot that gets FATTER where the picture is bright,
        // so highlights bloom and nearly close the line gap while shadows stay ribbed.
        float lum = dot(col, float3(0.299, 0.587, 0.114));
        float bw = lerp(0.32, 0.62, lum);
        float scan = exp(-(dl * dl) / (2.0 * bw * bw));
        col *= lerp(1.0, lerp(0.30, 1.15, scan), scanWeight);

        // Aperture grille: RGB phosphor stripes fixed to the glass, so they key off
        // the undistorted glass coordinate rather than the warped one.
        float gx = tuv.x * tubeSize.x / 3.0;
        float3 grille = 0.78 + 0.36 * cos((gx + float3(0.0, 0.3333, 0.6667)) * 6.2831853);
        col *= lerp(float3(1, 1, 1), grille, smoothstep(640.0, 1280.0, tubeSize.x));

        // Make-up gain for the scan and grille losses.
        col *= lerp(1.0, 1.30, scanWeight);

        // Halation: highlights bleed into the glass around them.
        float3 halo = float3(0, 0, 0);
        for (int i = 0; i < 6; i++) {
            float ang = float(i) * 1.0471976;
            halo += tex0.Sample(samp, suv + float2(cos(ang), sin(ang)) * 6.5 * texel).rgb;
        }
        halo /= 6.0;
        col += halo * halo * float3(0.16, 0.17, 0.20);

        // A slow mains hum bar rolling up, plus per-field brightness twitter.
        col *= 1.0 - 0.035 * (0.5 + 0.5 * sin((tuv.y - time * 0.061) * 6.2831853));
        col *= 0.972 + 0.028 * signalHash(float2(field * 12.9898, 5.0));

        // Phosphor floor: the tube never shows true black while the set is on.
        col += float3(0.016, 0.019, 0.023);

        // Phosphor grain: the coating is not smooth and the beam lighting it is a
        // stream of electrons, so a lit screen always fizzes a little. Shot noise, so
        // it grows where the beam works hardest rather than sitting flat over the top.
        // The grain is a touch coarser than one output pixel, the way real grains are.
        float2 gpx = floor(tuv * tubeSize / 1.5);
        float grain = signalHash2(gpx + float2(field * 3.1, field * 7.7)) - 0.5;
        float glow = dot(col, float3(0.299, 0.587, 0.114));
        col += grain * 0.077 * (0.30 + 0.70 * sqrt(saturate(glow)));
    }

    // The tube itself: rounded glass with a dark rim where the beam lands shallow at
    // the edge. This is what stops a set reading as a plain rectangle of scanlines.
    // Only a tube has one -- a camera feed on its own is just the signal.
    float glass = 1.0;
    if (crt) {
        // Measured in the bulged picture space, so the rim and the rounded corners
        // follow the dome round instead of cropping a curved picture with a straight
        // rectangle -- which is what flattened it and clipped the top.
        float2 gp = (q - 0.5) * tubeSize;
        float shortSide = min(tubeSize.x, tubeSize.y);
        float corner = 0.028 * shortSide;
        float2 halfSz = tubeSize * 0.5 - 3.0;
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
        // Antialias the edge against however many output pixels one glass pixel spans,
        // so the curved top and bottom do not staircase.
        float aa = max(fwidth(dist), 0.0001);
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
