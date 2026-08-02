import { Node3D } from '../models/Node3D.js';
import { BeamElement3D } from '../models/BeamElement3D.js';
import { SimulationStep } from '../models/SimulationStep.js';
import { FEASimulation } from '../models/FEASimulation.js';

/**
 * HDF5LoaderService.js
 * Serviço de I/O de alta performance responsável pelo parseamento binário de arquivos .h5 via h5wasm.
 */
export class HDF5LoaderService {
    /**
     * Carrega um arquivo HDF5 a partir de uma URL ou objeto File HTML5
     * @param {string|File} fileOrUrl 
     * @returns {Promise<FEASimulation>} Instância OO da simulação
     */
    static async load(fileOrUrl) {
        if (typeof h5wasm === 'undefined') {
            throw new Error("Módulo WebAssembly h5wasm não carregado.");
        }

        const { FS } = await h5wasm.ready;
        let buffer;

        if (typeof fileOrUrl === 'string') {
            const res = await fetch(fileOrUrl);
            if (!res.ok) throw new Error(`Falha HTTP ao buscar ${fileOrUrl}: status ${res.status}`);
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

            const dimsPos = posDS.shape;   // [num_steps, num_nodes, 3]
            const dimsTens = tensDS.shape; // [num_steps, num_elems]

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
}
