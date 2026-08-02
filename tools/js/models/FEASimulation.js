/**
 * FEASimulation.js
 * Agregador principal de simulação FEA, mantendo o histórico de passos e estatísticas.
 */
export class FEASimulation {
    /**
     * @param {string} simulationType - Nome/Descrição da Simulação
     * @param {number} seabedDepth - Profundidade do Fundo do Mar (-m)
     * @param {SimulationStep[]} steps - Coleção de passos incrementais padrão
     * @param {SimulationStep[]} staticSteps - Passos estáticos
     * @param {SimulationStep[]} dynamicSteps - Passos dinâmicos temporais
     */
    constructor(simulationType = "riserSim Equilibrium", seabedDepth = -100.0, steps = [], staticSteps = [], dynamicSteps = []) {
        this.simulationType = simulationType;
        this.seabedDepth = seabedDepth;
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

    getTensionRange() {
        let min = Infinity, max = -Infinity;
        this.activeSteps.forEach(step => {
            step.elements.forEach(elem => {
                const val = elem.tensionEffectiveKn !== undefined ? elem.tensionEffectiveKn : (elem.tension_effective_kN || 0);
                min = Math.min(min, val);
                max = Math.max(max, val);
            });
        });
        if (min === Infinity) return { min: 0, max: 10 };
        if (min === max) max = min + 1.0;
        return { min, max };
    }
}
