/**
 * BeamElement3D.js
 * Wraps the physical results and stresses of a 3D riser beam element.
 */
export class BeamElement3D {
    constructor(id, tensionEffectiveKn = 0, bendingMomentKnm = 0, curvature = 0, vonMisesMpa = 0, mbrSafetyFactor = 5.0) {
        this.id = id;
        this.tensionEffectiveKn = Number(tensionEffectiveKn) || 0;
        this.tension_effective_kN = this.tensionEffectiveKn;

        this.bendingMomentKnm = Number(bendingMomentKnm) || 0;
        this.bending_moment_kNm = this.bendingMomentKnm;

        this.curvature = Number(curvature) || 0;

        this.vonMisesMpa = Number(vonMisesMpa) || 0;
        this.von_mises_MPa = this.vonMisesMpa;

        this.mbrSafetyFactor = Number(mbrSafetyFactor) || 5.0;
        this.mbr_safety_factor = this.mbrSafetyFactor;
    }
}
