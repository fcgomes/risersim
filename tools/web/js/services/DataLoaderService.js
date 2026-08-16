import { Node3D } from '../models/Node3D.js';
import { BeamElement3D } from '../models/BeamElement3D.js';
import { SimulationStep } from '../models/SimulationStep.js';
import { FEASimulation } from '../models/FEASimulation.js';

/**
 * DataLoaderService.js
 * Loads simulation results from the sole results format, HDF5 (.h5) -- see
 * SimulationExporter::export_hdf5.
 */
export class DataLoaderService {
    /**
     * @param {string|File} fileOrUrl
     * @returns {Promise<FEASimulation>}
     */
    static async load(fileOrUrl) {
        return await DataLoaderService.loadHDF5(fileOrUrl);
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
}
