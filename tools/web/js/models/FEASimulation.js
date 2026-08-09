/**
 * FEASimulation.js
 * Main FEA simulation aggregator, holding the step history and statistics.
 */
export class FEASimulation {
    /**
     * @param {string} simulationType - Simulation name/description
     * @param {number} seabedDepth - Seabed depth (m)
     * @param {number} waterSurfaceZ - Water surface Z elevation (m)
     * @param {SimulationStep[]} steps - Default incremental step collection
     * @param {SimulationStep[]} staticSteps - Static steps
     * @param {SimulationStep[]} dynamicSteps - Time-domain dynamic steps
     */
    constructor(simulationType = "riserSim Equilibrium", seabedDepth = -100.0, waterSurfaceZ = 0.0, steps = [], staticSteps = [], dynamicSteps = []) {
        this.simulationType = simulationType;
        this.seabedDepth = seabedDepth;
        this.waterSurfaceZ = waterSurfaceZ;
        this.steps = steps;

        if (staticSteps && staticSteps.length > 0 && staticSteps.length <= 30) {
            this.staticSteps = staticSteps;
        } else if (staticSteps && staticSteps.length > 30) {
            this.staticSteps = staticSteps.slice(0, 21);
        } else if (steps.length > 30) {
            this.staticSteps = steps.slice(0, 21);
        } else {
            this.staticSteps = steps;
        }

        if (dynamicSteps && dynamicSteps.length > 0) {
            this.dynamicSteps = dynamicSteps;
        } else if (steps.length > 30) {
            this.dynamicSteps = steps;
        } else {
            this.dynamicSteps = [];
        }

        this.mode = this.dynamicSteps.length > 0 ? 'dynamic' : 'static';
    }

    get activeSteps() {
        if (this.mode === 'static' && this.staticSteps.length > 0) return this.staticSteps;
        if (this.mode === 'dynamic' && this.dynamicSteps.length > 0) return this.dynamicSteps;
        return this.steps;
    }

    get totalSteps() {
        return this.activeSteps.length;
    }

    getStep(index) {
        const steps = this.activeSteps;
        if (index < 0 || index >= steps.length) return null;
        return steps[index];
    }

    getFinalStep() {
        const steps = this.activeSteps;
        return steps.length > 0 ? steps[steps.length - 1] : null;
    }

    getElementScalar(elem, field = 'tension') {
        if (!elem) return 0.0;
        switch (field) {
            case 'moment':
                return elem.bendingMomentKnm !== undefined ? elem.bendingMomentKnm : (elem.bending_moment_kNm || 0.0);
            case 'curvature':
                return elem.curvature !== undefined ? elem.curvature : 0.0;
            case 'vonmises':
                return elem.vonMisesMpa !== undefined ? elem.vonMisesMpa : (elem.von_mises_MPa || 0.0);
            case 'mbr':
                return elem.mbrSafetyFactor !== undefined ? elem.mbrSafetyFactor : (elem.mbr_safety_factor || 1.0);
            case 'tension':
            default:
                return elem.tensionEffectiveKn !== undefined ? elem.tensionEffectiveKn : (elem.tension_effective_kN || 0.0);
        }
    }

    getTensionRange() {
        return this.getScalarRange('tension');
    }

    getScalarRange(field = 'tension') {
        let min = Infinity, max = -Infinity;
        this.activeSteps.forEach(step => {
            step.elements.forEach(elem => {
                const val = this.getElementScalar(elem, field);
                min = Math.min(min, val);
                max = Math.max(max, val);
            });
        });
        if (min === Infinity) return { min: 0, max: 10 };
        if (min === max) max = min + 1.0;
        return { min, max };
    }
}
