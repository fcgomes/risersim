/**
 * SimulationStep.js
 * Wraps the mesh state (Nodes and Elements) at a specific load/offset step.
 */
export class SimulationStep {
    /**
     * @param {number} stepIndex - Step index (0-indexed)
     * @param {number} loadFactor - Load factor from 0.0 to 1.0
     * @param {Node3D[]} nodes - This step's node list
     * @param {BeamElement3D[]} elements - This step's element list
     */
    constructor(stepIndex, loadFactor, nodes = [], elements = []) {
        this.stepIndex = stepIndex;
        this.loadFactor = loadFactor;
        this.nodes = nodes;
        this.elements = elements;
    }

    /**
     * Returns the maximum tension at the riser's top node in this step.
     * @returns {number} Tension in kN
     */
    getTopTension() {
        return this.elements.length > 0 ? this.elements[0].tensionEffectiveKn : 0.0;
    }

    /**
     * Returns the riser's maximum depth (minimum Z) in this step.
     * @returns {number} Minimum Z in meters
     */
    getMaxDepth() {
        if (this.nodes.length === 0) return 0.0;
        return Math.min(...this.nodes.map(n => n.z));
    }
}
