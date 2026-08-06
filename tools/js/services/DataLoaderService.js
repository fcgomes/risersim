import { Node3D } from '../models/Node3D.js';
import { BeamElement3D } from '../models/BeamElement3D.js';
import { SimulationStep } from '../models/SimulationStep.js';
import { FEASimulation } from '../models/FEASimulation.js';

/**
 * DataLoaderService.js
 * Serviço unificado de I/O capaz de carregar arquivos de resultados em formato HDF5 (.h5) e JSON (.json).
 */
export class DataLoaderService {
    /**
     * Carregador autodetector de formato (HDF5 ou JSON)
     * @param {string|File} fileOrUrl 
     * @returns {Promise<FEASimulation>}
     */
    static async load(fileOrUrl) {
        let isHDF5 = false;
        if (typeof fileOrUrl === 'string') {
            isHDF5 = fileOrUrl.endsWith('.h5') || fileOrUrl.endsWith('.hdf5');
        } else if (fileOrUrl && fileOrUrl.name) {
            isHDF5 = fileOrUrl.name.endsWith('.h5') || fileOrUrl.name.endsWith('.hdf5');
        }

        if (isHDF5) {
            try {
                return await DataLoaderService.loadHDF5(fileOrUrl);
            } catch (err) {
                console.warn("HDF5 loader falhou, tentando fallback para JSON: ", err);
                return await DataLoaderService.loadJSON('../catenary_results.json');
            }
        } else {
            return await DataLoaderService.loadJSON(fileOrUrl);
        }
    }

    static parseHDF5Group(group) {
        if (!group) return [];
        try {
            const posDS = group.get("node_positions");
            const tensDS = group.get("element_tensions_kN");
            if (!posDS || !tensDS) return [];

            const momentDS = group.get("element_bending_moments_kNm");
            const curvDS = group.get("element_curvatures");
            const vmDS = group.get("element_von_mises_MPa");
            const mbrDS = group.get("element_mbr_safety_factors");

            const posData = posDS.value;
            const tensData = tensDS.value;
            const momentData = momentDS ? momentDS.value : null;
            const curvData = curvDS ? curvDS.value : null;
            const vmData = vmDS ? vmDS.value : null;
            const mbrData = mbrDS ? mbrDS.value : null;

            const dimsPos = posDS.shape;
            const dimsTens = tensDS.shape;

            const numSteps = dimsPos[0];
            const numNodes = dimsPos[1];
            const numElems = dimsTens[1];

            const stepsList = [];

            for (let s = 0; s < numSteps; ++s) {
                const nodesList = [];
                for (let n = 0; n < numNodes; ++n) {
                    const idx = (s * numNodes + n) * 3;
                    nodesList.push(new Node3D(n + 1, posData[idx], posData[idx + 1], posData[idx + 2]));
                }

                const elementsList = [];
                for (let e = 0; e < numElems; ++e) {
                    const idxTens = s * numElems + e;
                    elementsList.push(new BeamElement3D(
                        e + 1,
                        tensData[idxTens],
                        momentData ? momentData[idxTens] : 0.0,
                        curvData ? curvData[idxTens] : 0.0,
                        vmData ? vmData[idxTens] : 0.0,
                        mbrData ? mbrData[idxTens] : 5.0
                    ));
                }

                const loadFactor = s / Math.max(1, numSteps - 1);
                stepsList.push(new SimulationStep(s, loadFactor, nodesList, elementsList));
            }

            return stepsList;
        } catch (err) {
            return [];
        }
    }

    static async loadHDF5(fileOrUrl) {
        if (typeof h5wasm === 'undefined') {
            throw new Error("h5wasm não está disponível");
        }

        const { FS } = await h5wasm.ready;
        let buffer;

        if (typeof fileOrUrl === 'string') {
            const cacheBusted = fileOrUrl + (fileOrUrl.includes('?') ? '&' : '?') + 'v=' + Date.now();
            const res = await fetch(cacheBusted, { cache: 'no-cache' });
            if (!res.ok) throw new Error(`HTTP ${res.status} ao carregar ${fileOrUrl}`);
            buffer = await res.arrayBuffer();
        } else {
            buffer = await fileOrUrl.arrayBuffer();
        }

        FS.writeFile("temp_render.h5", new Uint8Array(buffer));
        const h5file = new h5wasm.File("temp_render.h5", "r");

        try {
            const defaultSteps = DataLoaderService.parseHDF5Group(h5file);
            let staticSteps = [];
            let dynamicSteps = [];

            try {
                const staticGroup = h5file.get("static_analysis");
                if (staticGroup) staticSteps = DataLoaderService.parseHDF5Group(staticGroup);
            } catch(e) {}

            try {
                const dynamicGroup = h5file.get("dynamic_analysis");
                if (dynamicGroup) dynamicSteps = DataLoaderService.parseHDF5Group(dynamicGroup);
            } catch(e) {}

            // File-level attributes (see SimulationExporter::export_hdf5) -- defensive, since
            // older exported files won't have them and h5wasm's attrs API can vary by version.
            let seabedDepth = -100.0, waterSurfaceZ = 0.0;
            try {
                if (h5file.attrs) {
                    if (h5file.attrs.seabed_depth !== undefined) seabedDepth = h5file.attrs.seabed_depth.value ?? h5file.attrs.seabed_depth;
                    if (h5file.attrs.water_surface_z !== undefined) waterSurfaceZ = h5file.attrs.water_surface_z.value ?? h5file.attrs.water_surface_z;
                }
            } catch (e) {}

            return new FEASimulation("riserSim HDF5 Native Binary", seabedDepth, waterSurfaceZ, defaultSteps, staticSteps, dynamicSteps);
        } finally {
            h5file.close();
        }
    }

    static parseRawStepsArray(rawSteps) {
        if (!rawSteps || !Array.isArray(rawSteps)) return [];
        return rawSteps.map((rawStep, s) => {
            const nodesList = (rawStep.nodes || []).map(n => new Node3D(n.id, n.x, n.y, n.z));
            const elementsList = (rawStep.elements || []).map(e => new BeamElement3D(
                e.id,
                e.tension_effective_kN !== undefined ? e.tension_effective_kN : (e.tensionEffectiveKn || 0),
                e.bending_moment_kNm !== undefined ? e.bending_moment_kNm : (e.bendingMomentKnm || 0),
                e.curvature || 0,
                e.von_mises_MPa !== undefined ? e.von_mises_MPa : (e.vonMisesMpa || 0),
                e.mbr_safety_factor !== undefined ? e.mbr_safety_factor : (e.mbrSafetyFactor || 1.0)
            ));
            const loadFactor = rawStep.load_factor !== undefined ? rawStep.load_factor : (s / Math.max(1, rawSteps.length - 1));
            return new SimulationStep(s, loadFactor, nodesList, elementsList);
        });
    }

    /**
     * Carrega e parseia arquivos JSON (.json)
     */
    static async loadJSON(fileOrUrl) {
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

        const defaultSteps = DataLoaderService.parseRawStepsArray(jsonObject.steps || (Array.isArray(jsonObject) ? jsonObject : [jsonObject]));
        const staticSteps = DataLoaderService.parseRawStepsArray(jsonObject.static_steps || []);
        const dynamicSteps = DataLoaderService.parseRawStepsArray(jsonObject.dynamic_steps || []);

        return new FEASimulation(
            jsonObject.simulation_type || "riserSim JSON Results",
            jsonObject.seabed_depth ?? -100.0,
            jsonObject.water_surface_z ?? 0.0,
            defaultSteps,
            staticSteps,
            dynamicSteps
        );
    }
}
