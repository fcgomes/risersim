import { Node3D } from '../models/Node3D.js';
import { ElementSection } from '../models/ElementSection.js';

/**
 * InputLoaderService.js
 * Loads and validates the INPUT JSON consumed by risersim::ModelBuilder (not the results JSON
 * that DataLoaderService.js reads) -- model.nodes/elements, boundary_conditions, environmental,
 * analysis_options. See risersim/src/model_builder.cpp, ModelBuilder::load_from_json(), for the
 * exact schema this mirrors.
 */
export class InputLoaderService {
    /**
     * @param {string|File} fileOrUrl
     * @returns {Promise<object>} see parse()
     */
    static async load(fileOrUrl) {
        let jsonObject;
        if (typeof fileOrUrl === 'string') {
            const cacheBusted = fileOrUrl + (fileOrUrl.includes('?') ? '&' : '?') + 'v=' + Date.now();
            const res = await fetch(cacheBusted, { cache: 'no-cache' });
            if (!res.ok) throw new Error(`HTTP ${res.status} ao carregar ${fileOrUrl}`);
            jsonObject = await res.json();
        } else {
            const text = await fileOrUrl.text();
            jsonObject = JSON.parse(text);
        }
        return InputLoaderService.parse(jsonObject);
    }

    /**
     * Angle (degrees) above which two consecutive segments are reported as a possible "kink" in
     * the input geometry -- inspired by the 2D prototype the user had already built by hand
     * (scratchpad ex02a_geometry.html) to find this kind of issue.
     */
    static KINK_ANGLE_THRESHOLD_DEG = 15.0;

    /**
     * @param {object} j - Already-parsed JSON.
     * @returns {{
     *   valid: boolean, warnings: Array<{level:'error'|'warning'|'info', message:string}>,
     *   nodes?: Node3D[], elements?: ElementSection[], nodeIds?: Set<number>,
     *   prescribedIds?: number[], restrainedIds?: number[],
     *   seabed?: object, current?: object, wave?: object,
     *   staticOptions?: object, dynamicOptions?: object, raw: object
     * }}
     */
    static parse(j) {
        const warnings = [];
        const hasModel = j && j.model
            && Array.isArray(j.model.nodes) && j.model.nodes.length > 0
            && Array.isArray(j.model.elements) && j.model.elements.length > 0;

        if (!hasModel) {
            // Known keys of the legacy schema produced by aml_reader.py when there's no real
            // XML+H5 model -- ModelBuilder doesn't understand this schema and silently falls
            // back to the default synthetic model (a 40-element catenary).
            const legacyKeys = ['beam_props', 'geometry', 'offsets', 'rayleigh', 'simulation_options']
                .filter(k => j && j[k] !== undefined);
            warnings.push({
                level: 'error',
                message: legacyKeys.length > 0
                    ? `Schema incompatível detectado (chaves do formato legado: ${legacyKeys.join(', ')}). ` +
                      `Este arquivo NÃO será reconhecido por risersim (ModelBuilder) -- ele vai cair silenciosamente ` +
                      `no modelo sintético padrão (uma catenária de 40 elementos), ignorando os dados reais.`
                    : `"model.nodes"/"model.elements" ausentes ou vazios. Este arquivo vai cair no modelo ` +
                      `sintético padrão do ModelBuilder em vez de usar os dados deste arquivo.`
            });
            return { valid: false, warnings, raw: j };
        }

        const nodes = j.model.nodes.map(n => new Node3D(n.id, n.coords[0], n.coords[1], n.coords[2]));
        const nodeIds = new Set(nodes.map(n => n.id));
        const nodeById = new Map(nodes.map(n => [n.id, n]));

        const elements = j.model.elements.map(e =>
            new ElementSection(e.id, e.node1_id, e.node2_id, e.section_properties || {}));

        elements.forEach(e => {
            if (!nodeIds.has(e.node1Id) || !nodeIds.has(e.node2Id)) {
                warnings.push({
                    level: 'error',
                    message: `Elemento ${e.id} referencia nó inexistente em model.nodes ` +
                              `(node1_id=${e.node1Id}, node2_id=${e.node2Id}).`
                });
            }
        });

        // "Kink" detection: angle between the direction of consecutive elements sharing a node.
        // Assumes elements are ordered along the line, as ModelBuilder implicitly assumes (no
        // explicit connectivity graph is checked).
        const dirs = elements.map(e => {
            const n1 = nodeById.get(e.node1Id), n2 = nodeById.get(e.node2Id);
            if (!n1 || !n2) return null;
            const dx = n2.x - n1.x, dy = n2.y - n1.y, dz = n2.z - n1.z;
            const len = Math.sqrt(dx * dx + dy * dy + dz * dz);
            return len > 1.0e-9 ? { x: dx / len, y: dy / len, z: dz / len } : null;
        });
        for (let i = 1; i < dirs.length; i++) {
            const a = dirs[i - 1], b = dirs[i];
            if (!a || !b) continue;
            const dot = Math.max(-1, Math.min(1, a.x * b.x + a.y * b.y + a.z * b.z));
            const angleDeg = Math.acos(dot) * 180.0 / Math.PI;
            if (angleDeg > InputLoaderService.KINK_ANGLE_THRESHOLD_DEG) {
                warnings.push({
                    level: 'warning',
                    message: `Possível dobra (kink) entre os elementos ${elements[i - 1].id} e ` +
                              `${elements[i].id}: ${angleDeg.toFixed(1)}° entre segmentos consecutivos.`
                });
            }
        }

        const bc = j.boundary_conditions || {};
        const prescribedIds = (bc.prescribed_dofs || []).map(p => p.node_id);
        const restrainedIds = (bc.restrained_dofs || []).map(r => r.node_id);

        const env = j.environmental || {};
        const seabed = env.seabed || {};
        const current = env.current || {};
        const wave = env.wave || {};
        const opts = j.analysis_options || {};

        if (env.seabed && seabed.axial_friction === undefined && seabed.friction_coeff !== undefined) {
            warnings.push({
                level: 'info',
                message: `environmental.seabed.axial_friction/lateral_friction ausentes -- ` +
                          `ModelBuilder vai usar friction_coeff=${seabed.friction_coeff} isotrópico ` +
                          `para as duas direções em vez de valores axial/lateral distintos.`
            });
        }
        if (env.seabed && seabed.soil_model === undefined) {
            warnings.push({
                level: 'info',
                message: `environmental.seabed.soil_model ausente -- ModelBuilder assume "uncoupled" por padrão.`
            });
        }

        return {
            valid: true,
            warnings,
            nodes,
            elements,
            nodeIds,
            prescribedIds,
            restrainedIds,
            seabed,
            current,
            wave,
            staticOptions: opts.static || {},
            dynamicOptions: opts.dynamic || {},
            raw: j
        };
    }
}
