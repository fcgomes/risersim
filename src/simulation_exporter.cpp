/**
 * @file simulation_exporter.cpp
 * @brief HDF5 serialization of Analysis::history for post-processing/visualization.
 */
#include "risersim/simulation_exporter.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>

#ifdef RISERSIM_HAS_HDF5
#include <H5Cpp.h>
#endif

namespace risersim {

/** @brief Replaces NaN/Inf with 0.0 so exported HDF5 stays valid for downstream consumers. */
static double safe_num(double val) {
    if (std::isnan(val) || std::isinf(val)) return 0.0;
    return val;
}

#ifdef RISERSIM_HAS_HDF5

/**
 * @brief Writes one gzip-compressed, chunked dataset. Chunked along the step axis (dims[0]) --
 * up to 50 steps per chunk, or the whole extent if smaller -- since every consumer today reads a
 * dataset's full `.value` at once (see DataLoaderService.js::parseHDF5Group), so chunk size
 * mainly affects compression ratio (each chunk compresses independently; ~50 steps' worth of a
 * smooth engineering time series compresses well) rather than partial-read performance.
 *
 * Templated on the HDF5 location type (`H5::Group` or `H5::H5File`, both expose the same
 * `createDataSet` -- both derive from `H5::CommonFG` in this HDF5 C++ API version) so the same
 * function writes per-analysis-group datasets (node_positions/element_tensions_kN/...) AND the
 * file-root connectivity datasets (write_connectivity(), below).
 */
template <typename Loc>
static void write_compressed_dataset(Loc& loc, const std::string& name, const std::vector<double>& buf,
                                      int rank, const hsize_t* dims) {
    H5::DataSpace space(rank, dims);
    std::vector<hsize_t> chunk_dims(dims, dims + rank);
    chunk_dims[0] = std::min<hsize_t>(dims[0], 50);

    H5::DSetCreatPropList plist;
    plist.setChunk(rank, chunk_dims.data());
    plist.setDeflate(6);

    H5::DataSet dataset = loc.createDataSet(name, H5::PredType::NATIVE_DOUBLE, space, plist);
    dataset.write(buf.data(), H5::PredType::NATIVE_DOUBLE);
}

/**
 * @brief Writes the model's node/element connectivity once, at the file root (topology is the
 * same for both the static and dynamic groups -- same `model`, see Simulation::run()) --
 * `node_ids` (real id per `node_positions` array index) and `element_node1_ids`/
 * `element_node2_ids` (real node id pair per `element_*` array index).
 *
 * Without this, a consumer has no way to know which two nodes an element actually connects other
 * than ASSUMING element `i` connects nodes `i`/`i+1` -- true for a single riser line, but wrong
 * for a multi-line model (each extra line's node/element arrays are appended after the previous
 * one's, so every element past the first line's boundary is off by however many "line start"
 * nodes came before it -- found and fixed on the unsolved-preview path in
 * `Riser3DRenderer.js::renderStep()`; this closes the same gap for solved HDF5 results, which
 * never had real connectivity to fall back on before now).
 *
 * No-op (writes nothing) if `model` is null -- `Analysis::model` is only ever unset for a
 * default-constructed `Analysis` never passed through `Simulation::run()` (e.g. the Python
 * binding's `SimulationExporter.export_hdf5`, unused by the actual run pipeline, which always
 * goes through the compiled `risersim_test_main`/`Simulation::export_results()`).
 */
static void write_connectivity(H5::H5File& file, const RiserModel* model) {
    if (!model) return;
    const auto& nodes = model->nodes();
    const auto& elements = model->elements();
    if (nodes.empty() || elements.empty()) return;

    std::vector<double> node_ids(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) node_ids[i] = static_cast<double>(nodes[i]->id);
    hsize_t dims_nodes[1] = { nodes.size() };
    write_compressed_dataset(file, "node_ids", node_ids, 1, dims_nodes);

    std::vector<double> node1_ids(elements.size()), node2_ids(elements.size());
    for (size_t e = 0; e < elements.size(); ++e) {
        node1_ids[e] = static_cast<double>(elements[e]->node(0)->id);
        node2_ids[e] = static_cast<double>(elements[e]->node(1)->id);
    }
    hsize_t dims_elem[1] = { elements.size() };
    write_compressed_dataset(file, "element_node1_ids", node1_ids, 1, dims_elem);
    write_compressed_dataset(file, "element_node2_ids", node2_ids, 1, dims_elem);
}

/**
 * @brief Writes a step history to an HDF5 group: `node_positions` (steps x nodes x 3) and, per
 * element (steps x elements), `element_tensions_kN`/`element_bending_moments_kNm`/
 * `element_curvatures`/`element_von_mises_MPa`/`element_mbr_safety_factors` -- full parity with
 * what the JSON exporter used to write (see git history), so nothing the post-processor reads
 * (envelope/detail-panel/time-history) silently defaults to 0.0/5.0 anymore. All datasets
 * chunked+gzip-compressed (see write_compressed_dataset).
 */
static void write_hdf5_group(H5::H5File& file, const std::string& group_name, const std::vector<StepSnapshot>& history) {
    if (history.empty()) return;

    H5::Group group = file.createGroup(group_name);
    size_t num_steps = history.size();
    size_t num_nodes = history[0].node_coords.size();
    size_t num_elements = history[0].element_tensions_kN.size();

    // Node Positions Dataset (num_steps x num_nodes x 3)
    hsize_t dims_pos[3] = { num_steps, num_nodes, 3 };
    std::vector<double> buf_pos(num_steps * num_nodes * 3);
    for (size_t s = 0; s < num_steps; ++s) {
        for (size_t n = 0; n < num_nodes; ++n) {
            size_t idx = (s * num_nodes + n) * 3;
            buf_pos[idx + 0] = safe_num(history[s].node_coords[n].x());
            buf_pos[idx + 1] = safe_num(history[s].node_coords[n].y());
            buf_pos[idx + 2] = safe_num(history[s].node_coords[n].z());
        }
    }
    write_compressed_dataset(group, "node_positions", buf_pos, 3, dims_pos);

    // Per-element datasets (num_steps x num_elements), one buffer/dataset per field.
    hsize_t dims_elem[2] = { num_steps, num_elements };
    struct ElementField {
        const char* name;
        std::vector<double> StepSnapshot::* member;
    };
    static const ElementField fields[] = {
        { "element_tensions_kN", &StepSnapshot::element_tensions_kN },
        { "element_bending_moments_kNm", &StepSnapshot::element_bending_moments_kNm },
        { "element_curvatures", &StepSnapshot::element_curvatures },
        { "element_von_mises_MPa", &StepSnapshot::element_von_mises_MPa },
        { "element_mbr_safety_factors", &StepSnapshot::element_mbr_safety_factors },
    };
    for (const auto& field : fields) {
        std::vector<double> buf(num_steps * num_elements);
        for (size_t s = 0; s < num_steps; ++s) {
            const std::vector<double>& src = history[s].*(field.member);
            for (size_t e = 0; e < num_elements; ++e) {
                buf[s * num_elements + e] = safe_num(src[e]);
            }
        }
        write_compressed_dataset(group, field.name, buf, 2, dims_elem);
    }
}

bool SimulationExporter::export_hdf5(const Analysis& static_analysis, const Analysis& dynamic_analysis,
                                      double seabed_depth, double water_surface_z, const std::string& filename) {
    try {
        H5::H5File file(filename, H5F_ACC_TRUNC);

        H5::DataSpace scalar_space(H5S_SCALAR);
        double sd = safe_num(seabed_depth), wsz = safe_num(water_surface_z);
        file.createAttribute("seabed_depth", H5::PredType::NATIVE_DOUBLE, scalar_space).write(H5::PredType::NATIVE_DOUBLE, &sd);
        file.createAttribute("water_surface_z", H5::PredType::NATIVE_DOUBLE, scalar_space).write(H5::PredType::NATIVE_DOUBLE, &wsz);

        write_hdf5_group(file, "/static_analysis", static_analysis.history);
        write_hdf5_group(file, "/dynamic_analysis", dynamic_analysis.history);
        write_connectivity(file, static_analysis.model ? static_analysis.model : dynamic_analysis.model);

        std::cout << "✅ Binary HDF5 simulation history successfully exported to: " << filename
                  << " (Static: " << static_analysis.history.size()
                  << " steps, Dynamic: " << dynamic_analysis.history.size() << " steps)" << std::endl;
        return true;
    } catch (const H5::Exception& e) {
        std::cerr << "❌ HDF5 Export Exception: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}

#else // RISERSIM_HAS_HDF5 not defined

bool SimulationExporter::export_hdf5(const Analysis& /*static_analysis*/, const Analysis& /*dynamic_analysis*/,
                                      double /*seabed_depth*/, double /*water_surface_z*/, const std::string& /*filename*/) {
    std::cerr << "⚠️ HDF5 support not available (built without RISERSIM_HAS_HDF5). Skipping HDF5 export." << std::endl;
    return false;
}

#endif // RISERSIM_HAS_HDF5

} // namespace risersim
