#include "Beatmap/KSH2Bridge.hpp"
#include <kson/kson.hpp>
#include <sstream>
#include <exception>

bool ConvertKSH2(const std::string& input, std::string& output, std::string& diagnostics)
{
    output.clear();
    diagnostics.clear();
    try {
        std::istringstream in(input);
        kson::KshLoadingDiag diag;
        auto chart = kson::LoadKshChartData(in, &diag);
        for (const auto& warning : diag.warnings)
            diagnostics += "line " + std::to_string(warning.lineNo) + ": " + warning.message + "\n";
        if (chart.error != kson::ErrorType::None) {
            diagnostics += kson::GetErrorString(chart.error);
            return false;
        }
        std::ostringstream out;
        const auto error = kson::SaveKsonChartData(out, chart);
        if (error != kson::ErrorType::None) {
            diagnostics += kson::GetErrorString(error);
            return false;
        }
        output = out.str();
        return true;
    } catch (const std::exception& e) {
        diagnostics += e.what();
        return false;
    }
}
