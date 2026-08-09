/**
 * Node3D.js
 * Wraps the coordinates and 3D properties of an FEA mesh node.
 */
export class Node3D {
    /**
     * @param {number} id - Unique node identifier (1-indexed)
     * @param {number} x - X coordinate (m)
     * @param {number} y - Y coordinate (m)
     * @param {number} z - Z coordinate (m / depth)
     */
    constructor(id, x, y, z) {
        this.id = id;
        this.x = x;
        this.y = y;
        this.z = z;
    }

    /**
     * Computes the Euclidean distance to another node.
     * @param {Node3D} other
     * @returns {number} Distance in meters
     */
    distanceTo(other) {
        const dx = this.x - other.x;
        const dy = this.y - other.y;
        const dz = this.z - other.z;
        return Math.sqrt(dx * dx + dy * dy + dz * dz);
    }
}
