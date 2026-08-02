/**
 * SimulationStep.js
 * Encapsula o estado da malha (Nós e Elementos) em um passo específico de carga / offset.
 */
export class SimulationStep {
    /**
     * @param {number} stepIndex - Índice do passo (0-indexed)
     * @param {number} loadFactor - Fator de carga de 0.0 a 1.0
     * @param {Node3D[]} nodes - Lista de nós do passo
     * @param {BeamElement3D[]} elements - Lista de elementos do passo
     */
    constructor(stepIndex, loadFactor, nodes = [], elements = []) {
        this.stepIndex = stepIndex;
        this.loadFactor = loadFactor;
        this.nodes = nodes;
        this.elements = elements;
    }

    /**
     * Retorna a tração máxima no topo do riser neste passo
     * @returns {number} Tração em kN
     */
    getTopTension() {
        return this.elements.length > 0 ? this.elements[0].tensionEffectiveKn : 0.0;
    }

    /**
     * Retorna a profundidade máxima (mínimo Z) do riser neste passo
     * @returns {number} Z mínimo em metros
     */
    getMaxDepth() {
        if (this.nodes.length === 0) return 0.0;
        return Math.min(...this.nodes.map(n => n.z));
    }
}
