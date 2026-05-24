#include "doctor.hpp"

#include "utils/diagnostics/logger.hpp"

namespace WFX::CLI {

int WFXDoctor()
{
    auto& logger = Utils::GetLogger();
    logger.Info("-----------------------------------------------------");
    logger.Info("[Doctor]: Deprecated for now, might be used in future");
    logger.Info("-----------------------------------------------------");

    return 0;
}

} // namespace WFX::CLI