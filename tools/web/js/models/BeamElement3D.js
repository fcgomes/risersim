/**
 * BeamElement3D.js
 * Wraps the physical results and stresses of a 3D riser beam element.
 */
export class BeamElement3D {
    /**
     * @param {number|null} node1Id - Real node id this element connects (1-indexed, from the HDF5's
     * `element_node1_ids` -- absent/null for an older results file exported before that dataset
     * existed, or a synthetic/test step). `Riser3DRenderer.js::renderStep()` falls back to
     * positional indexing when null -- correct only for a single-line model, see its own comment.
     * @param {number|null} node2Id - Same, the element's other node.
     */
    constructor(id, tensionEffectiveKn = 0, bendingMomentKnm = 0, curvature = 0, vonMisesMpa = 0, mbrSafetyFactor = 5.0, node1Id = null, node2Id = null) {
        this.id = id;
        this.node1_id = node1Id;
        this.node2_id = node2Id;
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
