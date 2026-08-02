/**
 * BeamElement3D.js
 * Encapsula os resultados físicos e tensões de um elemento de viga 3D do riser.
 */
export class BeamElement3D {
    /**
     * @param {number} id - Identificador do elemento
     * @param {number} tensionEffectiveKn - Tração efetiva (kN)
     * @param {number} bendingMomentKnm - Momento fletor (kN.m)
     * @param {number} curvature - Curvatura (1/m)
     * @param {number} vonMisesMpa - Tensão combinada de von Mises (MPa)
     * @param {number} mbrSafetyFactor - Fator de segurança do Raio Mínimo de Curvatura
     */
    constructor(id, tensionEffectiveKn = 0, bendingMomentKnm = 0, curvature = 0, vonMisesMpa = 0, mbrSafetyFactor = 5.0) {
        this.id = id;
        this.tensionEffectiveKn = tensionEffectiveKn;
        this.bendingMomentKnm = bendingMomentKnm;
        this.curvature = curvature;
        this.vonMisesMpa = vonMisesMpa;
        this.mbrSafetyFactor = mbrSafetyFactor;
    }
}
