/**
 * @file model_builder.hpp
 * @brief Builds a RiserModel from a structured input JSON, mirroring real ANFLEX's
 * cModelBuilderDAT (owns a cDomain, has load_* methods that populate it from the AML/XML reader)
 * -- risersim's RiserModel/cDomain equivalent stays a pure data aggregate, with all parsing/
 * construction concerns living here instead.
 */
#ifndef RISERSIM_MODEL_BUILDER_HPP
#define RISERSIM_MODEL_BUILDER_HPP

#include "risersim/model.hpp"
#include "risersim/element_beam.hpp"
#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <vector>

namespace risersim {

/**
 * @brief One data-quality note: a physical/environmental value was defaulted because the input
 * JSON didn't provide it. Same {level, message} shape as the preprocessor's JS warnings panel
 * (tools/js/services/InputLoaderService.js), for possible future reuse -- not exported anywhere
 * yet, only printed to the console (see ModelBuilder::print_warnings()).
 */
struct ModelWarning {
    std::string level;
    std::string message;
};

/**
 * @brief Converts a `section_properties` JSON object into BeamMaterialProps, applying the same
 * derivation rules (rho from submerged weight, IY/IZ from EI, geometric J fallback) used by both
 * ModelBuilder::load_from_json() and tests/diag_isolated_segment.cpp -- the one piece of parsing
 * that was genuinely duplicated between the two before this was extracted.
 * @param missing_field_counts If non-null, incremented once per field that was absent from `sp`
 * and had to fall back to a default/derived value -- ModelBuilder passes a map to aggregate
 * across every element into a summary warning; diag_isolated_segment.cpp (a diagnostic tool with
 * its own console output) passes nullptr to skip counting.
 */
BeamMaterialProps parse_beam_section_properties(const nlohmann::json& sp, std::map<std::string, int>* missing_field_counts = nullptr);

/**
 * @brief Builds a RiserModel from a structured input JSON, mirroring ANFLEX's cModelBuilderDAT.
 *
 * Every value defaulted because a JSON field was missing is recorded in `warnings`.
 * Physical/environmental values are deliberately in scope (section properties, seabed, current,
 * wave); solver tuning knobs (static_steps, tolerances, etc.) are not -- those already have
 * intentional, documented defaults in AnalysisOptionsConfig and aren't "a physical value assumed
 * in place of missing data," they're algorithm configuration.
 */
class ModelBuilder {
public:
    ModelBuilder() = default;

    /** @brief Non-owning access to the model being built. */
    RiserModel* get_model() { return &model; }
    const RiserModel* get_model() const { return &model; }

    /**
     * @brief Loads nodes/elements/boundary-conditions/warm-start/environmental/analysis_options
     * from a structured input JSON into the owned model.
     * @return false (with an error already printed) if the file couldn't be opened or parsed.
     */
    bool load_from_json(const std::string& input_json_path);

    /** @brief Prints every accumulated warning to the console, prefixed by its level. */
    void print_warnings() const;

    std::vector<ModelWarning> warnings;

private:
    RiserModel model;
};

} // namespace risersim

#endif // RISERSIM_MODEL_BUILDER_HPP
