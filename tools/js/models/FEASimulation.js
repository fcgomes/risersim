/**
 * FEASimulation.js
 * Agregador principal de simulação FEA, mantendo o histórico de passos e estatísticas.
 */
export class FEASimulation {
    /**
     * @param {string} simulationType - Nome/Descrição da Simulação
     * @param {number} seabedDepth - Profundidade do Fundo do Mar (-m)
     * @param {SimulationStep[]} steps - Coleção de passos incrementais
     */
    constructor(simulationType = "riserSim Equilibrium", seabedDepth = -100.0, steps = []) {
        this.simulationType = simulationType;
        this.seabedDepth = seabedDepth;
        this.steps = steps;
    }

    /**
     * Retorna a quantidade total de passos salvos
     * @returns {number}
     */
    get totalSteps() {
        return this.steps.length;
    }

    /**
     * Retorna um passo por seu índice
     * @param {number} index 
     * @returns {SimulationStep|null}
     */
    getStep(index) {
        if (index < 0 || index >= this.steps.length) return null;
        return this.steps[index];
    }

    /**
     * Retorna o último passo de equilíbrio final
     * @returns {SimulationStep|null}
     */
    getFinalStep() {
        return this.steps.length > 0 ? this.steps[this.steps.length - 1] : null;
    }

    /**
     * Calcula os limites mínimo e máximo de tração efetiva de toda a simulação
     * @returns {{min: number, max: number}}
     */
    getTensionRange() {
        let min = Infinity, max = -Infinity;
        this.steps.forEach(step => {
            step.elements.forEach(elem => {
                min = Math.min(min, elem.tensionEffectiveKn);
                max = Math.max(max, elem.tensionEffectiveKn);
            });
        });
        if (min === Infinity) return { min: 0, max: 10 };
        if (min === max) max = min + 1.0;
        return { min, max };
    }
}
