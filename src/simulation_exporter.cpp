#include "risersim/simulation_exporter.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cmath>

#ifdef RISERSIM_HAS_HDF5
#include <H5Cpp.h>
#endif

namespace risersim {

static double safe_num(double val) {
    if (std::isnan(val) || std::isinf(val)) return 0.0;
    return val;
}

static void write_snapshots_json_array(std::ofstream& ofs, const std::vector<StepSnapshot>& history) {
    ofs << "[";
    for (size_t s = 0; s < history.size(); ++s) {
        const auto& snap = history[s];
        ofs << "{\"step\":" << snap.step_index << ",\"load_factor\":" << safe_num(snap.load_factor) << ",\"nodes\":[";
        for (size_t i = 0; i < snap.node_coords.size(); ++i) {
            ofs << "{\"id\":" << (i + 1)
                << ",\"x\":" << safe_num(snap.node_coords[i].x())
                << ",\"y\":" << safe_num(snap.node_coords[i].y())
                << ",\"z\":" << safe_num(snap.node_coords[i].z()) << "}";
            if (i + 1 < snap.node_coords.size()) ofs << ",";
        }
        ofs << "],\"elements\":[";
        for (size_t e = 0; e < snap.element_tensions_kN.size(); ++e) {
            ofs << "{\"id\":" << (e + 1)
                << ",\"tension_effective_kN\":" << safe_num(snap.element_tensions_kN[e])
                << ",\"bending_moment_kNm\":" << safe_num(snap.element_bending_moments_kNm[e])
                << ",\"curvature\":" << safe_num(snap.element_curvatures[e])
                << ",\"von_mises_MPa\":" << safe_num(snap.element_von_mises_MPa[e])
                << ",\"mbr_safety_factor\":" << safe_num(snap.element_mbr_safety_factors[e]) << "}";
            if (e + 1 < snap.element_tensions_kN.size()) ofs << ",";
        }
        ofs << "]}";
        if (s + 1 < history.size()) ofs << ",";
    }
    ofs << "]";
}

bool SimulationExporter::export_json(const Analysis& static_analysis, const Analysis& dynamic_analysis, const std::string& filename) {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) return false;

    ofs << std::fixed << std::setprecision(5);
    ofs << "{\n  \"static_steps\": ";
    write_snapshots_json_array(ofs, static_analysis.history);

    ofs << ",\n  \"dynamic_steps\": ";
    write_snapshots_json_array(ofs, dynamic_analysis.history);

    // Backward Compatibility Fallback Array
    ofs << ",\n  \"steps\": ";
    if (!dynamic_analysis.history.empty()) {
        write_snapshots_json_array(ofs, dynamic_analysis.history);
    } else {
        write_snapshots_json_array(ofs, static_analysis.history);
    }

    ofs << "\n}\n";
    ofs.close();

    std::cout << "✅ Full simulation history exported to JSON: " << filename
              << " (Static: " << static_analysis.history.size() 
              << " steps, Dynamic: " << dynamic_analysis.history.size() << " steps)" << std::endl;
    return true;
}

#ifdef RISERSIM_HAS_HDF5

static void write_hdf5_group(H5::H5File& file, const std::string& group_name, const std::vector<StepSnapshot>& history) {
    if (history.empty()) return;

    H5::Group group = file.createGroup(group_name);
    size_t num_steps = history.size();
    size_t num_nodes = history[0].node_coords.size();
    size_t num_elements = history[0].element_tensions_kN.size();

    // Node Positions Dataset (num_steps x num_nodes x 3)
    hsize_t dims_pos[3] = { num_steps, num_nodes, 3 };
    H5::DataSpace space_pos(3, dims_pos);
    std::vector<double> buf_pos(num_steps * num_nodes * 3);

    for (size_t s = 0; s < num_steps; ++s) {
        for (size_t n = 0; n < num_nodes; ++n) {
            size_t idx = (s * num_nodes + n) * 3;
            buf_pos[idx + 0] = safe_num(history[s].node_coords[n].x());
            buf_pos[idx + 1] = safe_num(history[s].node_coords[n].y());
            buf_pos[idx + 2] = safe_num(history[s].node_coords[n].z());
        }
    }

    H5::DataSet dataset_pos = group.createDataSet("node_positions", H5::PredType::NATIVE_DOUBLE, space_pos);
    dataset_pos.write(buf_pos.data(), H5::PredType::NATIVE_DOUBLE);

    // Tension Dataset (num_steps x num_elements)
    hsize_t dims_elem[2] = { num_steps, num_elements };
    H5::DataSpace space_elem(2, dims_elem);
    std::vector<double> buf_tens(num_steps * num_elements);

    for (size_t s = 0; s < num_steps; ++s) {
        for (size_t e = 0; e < num_elements; ++e) {
            buf_tens[s * num_elements + e] = safe_num(history[s].element_tensions_kN[e]);
        }
    }

    H5::DataSet dataset_tens = group.createDataSet("element_tensions_kN", H5::PredType::NATIVE_DOUBLE, space_elem);
    dataset_tens.write(buf_tens.data(), H5::PredType::NATIVE_DOUBLE);
}

bool SimulationExporter::export_hdf5(const Analysis& static_analysis, const Analysis& dynamic_analysis, const std::string& filename) {
    try {
        H5::H5File file(filename, H5F_ACC_TRUNC);

        write_hdf5_group(file, "/static_analysis", static_analysis.history);
        write_hdf5_group(file, "/dynamic_analysis", dynamic_analysis.history);

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

bool SimulationExporter::export_hdf5(const Analysis& /*static_analysis*/, const Analysis& /*dynamic_analysis*/, const std::string& /*filename*/) {
    std::cerr << "⚠️ HDF5 support not available (built without RISERSIM_HAS_HDF5). Skipping HDF5 export." << std::endl;
    return false;
}

#endif // RISERSIM_HAS_HDF5

} // namespace risersim
