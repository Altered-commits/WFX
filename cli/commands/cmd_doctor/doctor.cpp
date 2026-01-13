#include "doctor.hpp"

#include "utils/logger/logger.hpp"

namespace WFX::CLI {

int WFXDoctor()
{
    auto& logger = WFX::Utils::Logger::GetInstance();
    logger.Info("-----------------------------------------------------");
    logger.Info("[Doctor]: Deprecated for now, might be used in future");
    logger.Info("-----------------------------------------------------");

    return 0;
}

} // namespace WFX::CLI