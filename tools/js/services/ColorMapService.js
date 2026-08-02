/**
 * ColorMapService.js
 * Serviço responsável por interpolar paletas de cores científicas (Jet, Viridis, Plasma, Turbo, Coolwarm).
 */
export class ColorMapService {
    /**
     * Interpola uma cor RGB baseada em um valor normalizado (0.0 a 1.0)
     * @param {string} colormapName - Nome do mapa de cores
     * @param {number} t - Valor entre 0.0 e 1.0
     * @returns {{r: number, g: number, b: number}} Componentes normalizados de 0.0 a 1.0
     */
    static getColor(colormapName, t) {
        t = Math.max(0.0, Math.min(1.0, t));

        if (colormapName === 'Viridis') {
            return {
                r: 0.28 + 0.70 * t,
                g: 0.14 + 0.75 * Math.sin(t * Math.PI),
                b: 0.47 + 0.40 * (1.0 - t)
            };
        } else if (colormapName === 'Plasma') {
            return {
                r: 0.05 + 0.90 * Math.pow(t, 0.8),
                g: 0.0 + 0.95 * Math.pow(t, 2.0),
                b: 0.53 + 0.40 * (1.0 - t)
            };
        } else if (colormapName === 'Turbo') {
            return {
                r: Math.sin(t * Math.PI * 0.9 + 0.1),
                g: Math.sin(t * Math.PI * 0.8 + 0.3),
                b: Math.cos(t * Math.PI * 0.5)
            };
        } else if (colormapName === 'Coolwarm') {
            return {
                r: t,
                g: 0.3 + 0.4 * (1.0 - Math.abs(t - 0.5) * 2),
                b: 1.0 - t
            };
        } else {
            // Jet (Default Classic Engineering)
            let r = Math.min(1.0, Math.max(0.0, 1.5 - Math.abs(t * 4.0 - 3.0)));
            let g = Math.min(1.0, Math.max(0.0, 1.5 - Math.abs(t * 4.0 - 2.0)));
            let b = Math.min(1.0, Math.max(0.0, 1.5 - Math.abs(t * 4.0 - 1.0)));
            return { r, g, b };
        }
    }
}
