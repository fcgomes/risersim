/**
 * Node3D.js
 * Encapsula as coordenadas e propriedades tridimensionais de um nó da malha FEA.
 */
export class Node3D {
    /**
     * @param {number} id - Identificador único do nó (1-indexed)
     * @param {number} x - Coordenada X (m)
     * @param {number} y - Coordenada Y (m)
     * @param {number} z - Coordenada Z (m / profundidade)
     */
    constructor(id, x, y, z) {
        this.id = id;
        this.x = x;
        this.y = y;
        this.z = z;
    }

    /**
     * Calcula a distância euclidiana até outro nó
     * @param {Node3D} other 
     * @returns {number} Distância em metros
     */
    distanceTo(other) {
        const dx = this.x - other.x;
        const dy = this.y - other.y;
        const dz = this.z - other.z;
        return Math.sqrt(dx * dx + dy * dy + dz * dz);
    }
}
