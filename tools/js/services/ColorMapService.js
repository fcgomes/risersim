/**
 * ColorMapService.js
 * Service responsible for interpolating scientific color palettes (Jet, Viridis, Plasma, Turbo, Coolwarm).
 *
 * Viridis/Plasma/Turbo/Coolwarm are linearly interpolated between control points reproducing the
 * real palettes (matplotlib/Google Turbo/Kenneth Moreland Coolwarm), instead of the ad-hoc
 * trigonometric formulas used before (which were only visually similar, with no real relation to
 * the palettes whose name they used). A handful of control points (8-9), not the full 256-color
 * LUT -- enough for a continuous color legend, without embedding a large table in the code.
 */

// Control points [t, r, g, b] (r,g,b in 0-255), t from 0.0 to 1.0.
const STOPS = {
    Viridis: [
        [0.00, 68, 1, 84], [0.13, 72, 40, 120], [0.25, 62, 74, 137], [0.38, 49, 104, 142],
        [0.50, 38, 130, 142], [0.63, 31, 158, 137], [0.75, 53, 183, 121], [0.88, 109, 205, 89],
        [1.00, 253, 231, 37],
    ],
    Plasma: [
        [0.000, 13, 8, 135], [0.111, 70, 3, 159], [0.222, 114, 1, 168], [0.333, 156, 23, 158],
        [0.444, 189, 55, 134], [0.556, 216, 87, 107], [0.667, 237, 121, 83], [0.778, 251, 159, 58],
        [0.889, 253, 202, 64], [1.000, 240, 249, 33],
    ],
    Turbo: [
        [0.00, 48, 18, 59], [0.13, 70, 107, 227], [0.25, 42, 165, 224], [0.38, 55, 201, 152],
        [0.50, 151, 216, 65], [0.63, 222, 189, 46], [0.75, 245, 136, 41], [0.88, 213, 63, 21],
        [1.00, 122, 4, 3],
    ],
    Coolwarm: [
        [0.00, 59, 76, 192], [0.25, 124, 159, 249], [0.50, 221, 221, 221],
        [0.75, 220, 130, 100], [1.00, 180, 4, 38],
    ],
};

export class ColorMapService {
    /**
     * Interpolates an RGB color based on a normalized value (0.0 to 1.0).
     * @param {string} colormapName - Colormap name
     * @param {number} t - Value between 0.0 and 1.0
     * @returns {{r: number, g: number, b: number}} Normalized components from 0.0 to 1.0
     */
    static getColor(colormapName, t) {
        t = Math.max(0.0, Math.min(1.0, t));

        const stops = STOPS[colormapName];
        if (!stops) {
            // Jet (classic engineering default) -- a standard analytic formula, not a LUT.
            const r = Math.min(1.0, Math.max(0.0, 1.5 - Math.abs(t * 4.0 - 3.0)));
            const g = Math.min(1.0, Math.max(0.0, 1.5 - Math.abs(t * 4.0 - 2.0)));
            const b = Math.min(1.0, Math.max(0.0, 1.5 - Math.abs(t * 4.0 - 1.0)));
            return { r, g, b };
        }

        let i = 0;
        while (i < stops.length - 2 && t > stops[i + 1][0]) i++;
        const [t0, r0, g0, b0] = stops[i];
        const [t1, r1, g1, b1] = stops[i + 1];
        const span = t1 - t0;
        const f = span > 0 ? (t - t0) / span : 0;

        return {
            r: (r0 + (r1 - r0) * f) / 255,
            g: (g0 + (g1 - g0) * f) / 255,
            b: (b0 + (b1 - b0) * f) / 255,
        };
    }

    /**
     * Builds a CSS `linear-gradient(to top, ...)` string from the same control points getColor()
     * interpolates from, so the flat colorbar legend widget always matches what's actually
     * rendered on the 3D tubes/data -- rather than a second, independently hand-picked gradient
     * that could silently drift out of sync with it.
     * @param {string} colormapName
     * @returns {string} CSS gradient value.
     */
    static getCssGradient(colormapName) {
        const stops = STOPS[colormapName];
        if (!stops) {
            // Jet: no control-point table (analytic formula), keep its known-good literal gradient.
            return 'linear-gradient(to top, #0000ff, #00ffff, #00ff00, #ffff00, #ff0000)';
        }
        const toHex = (v) => v.toString(16).padStart(2, '0');
        const stopStrings = stops.map(([t, r, g, b]) => `#${toHex(r)}${toHex(g)}${toHex(b)} ${(t * 100).toFixed(0)}%`);
        return `linear-gradient(to top, ${stopStrings.join(', ')})`;
    }
}
