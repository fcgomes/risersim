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

    /**
     * Carrega e parseia arquivos HDF5 (.h5) via h5wasm
     */
    static async loadHDF5(fileOrUrl) {
        if (typeof h5wasm === 'undefined') {
            throw new Error("h5wasm não está disponível");
        }

        const { FS } = await h5wasm.ready;
        let buffer;

        if (typeof fileOrUrl === 'string') {
            const res = await fetch(fileOrUrl);
            if (!res.ok) throw new Error(`HTTP ${res.status} ao carregar ${fileOrUrl}`);
            buffer = await res.arrayBuffer();
        } else {
            buffer = await fileOrUrl.arrayBuffer();
        }

        FS.writeFile("temp_render.h5", new Uint8Array(buffer));
        const h5file = new h5wasm.File("temp_render.h5", "r");

        try {
            const posDS = h5file.get("node_positions");
            const tensDS = h5file.get("element_tensions_kN");
            const momentDS = h5file.get("element_bending_moments_kNm");
            const vmDS = h5file.get("element_von_mises_MPa");

            const posData = posDS.value;
            const tensData = tensDS.value;
            const momentData = momentDS ? momentDS.value : null;
            const vmData = vmDS ? vmDS.value : null;

            const dimsPos = posDS.shape;
            const dimsTens = tensDS.shape;

            const numSteps = dimsPos[0];
            const numNodes = dimsPos[1];
            const numElems = dimsTens[1];

            const simulationSteps = [];

            for (let s = 0; s < numSteps; ++s) {
                const nodesList = [];
                for (let n = 0; n < numNodes; ++n) {
                    const idx = (s * numNodes + n) * 3;
                    nodesList.push(new Node3D(
                        n + 1,
                        posData[idx],
                        posData[idx + 1],
                        posData[idx + 2]
                    ));
                }

                const elementsList = [];
                for (let e = 0; e < numElems; ++e) {
                    const idxTens = s * numElems + e;
                    elementsList.push(new BeamElement3D(
                        e + 1,
                        tensData[idxTens],
                        momentData ? momentData[idxTens] : 0.0,
                        0.0,
                        vmData ? vmData[idxTens] : 0.0,
                        5.0
                    ));
                }

                const loadFactor = s / Math.max(1, numSteps - 1);
                simulationSteps.push(new SimulationStep(s, loadFactor, nodesList, elementsList));
            }

            return new FEASimulation("riserSim HDF5 Native Binary", -100.0, simulationSteps);
        } finally {
            h5file.close();
        }
    }

    /**
     * Carrega e parseia arquivos JSON (.json)
     */
    static async loadJSON(fileOrUrl) {
        let jsonObject;
        if (typeof fileOrUrl === 'string') {
            const res = await fetch(fileOrUrl);
            if (!res.ok) throw new Error(`HTTP ${res.status} ao carregar ${fileOrUrl}`);
            jsonObject = await res.json();
        } else {
            const text = await fileOrUrl.text();
            jsonObject = JSON.parse(text);
        }

        const rawSteps = jsonObject.steps || [jsonObject];
        const simulationSteps = [];

        rawSteps.forEach((rawStep, s) => {
            const nodesList = (rawStep.nodes || []).map(n => new Node3D(n.id, n.x, n.y, n.z));
            const elementsList = (rawStep.elements || []).map(e => new BeamElement3D(
                e.id,
                e.tension_effective_kN || 0,
                e.bending_moment_kNm || 0,
                e.curvature || 0,
                e.von_mises_MPa || 0,
                e.mbr_safety_factor || 1.0
            ));

            const loadFactor = rawStep.load_factor !== undefined ? rawStep.load_factor : (s / Math.max(1, rawSteps.length - 1));
            simulationSteps.push(new SimulationStep(s, loadFactor, nodesList, elementsList));
        });

        return new FEASimulation(
            jsonObject.simulation_type || "riserSim JSON Results",
            jsonObject.seabed_depth || -100.0,
            simulationSteps
        );
    }
}
