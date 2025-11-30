#ifndef WFX_INC_ASYNC_BUILTINS_HPP
#define WFX_INC_ASYNC_BUILTINS_HPP

#include "runtime.hpp"

namespace Async {

// IMPORTANT: THIS FUNCTION EXPECTS POINTER TO CONNECTION CONTEXT BE SET VIA 'SetGlobalPtrData' BEFORE BEING INVOKED
inline AsyncPtr SleepFor(std::uint32_t delayMs) noexcept
{
    return Async::MakeAsync(
        [](AsyncPtr self, std::uint32_t delayMs) {
            switch(self->GetState())
            {
                // This will be called only once, to schedule a delay
                // If registering fails, it will just finish on the spot, no async is generated, error set
                // Error is propagated to the caller via 'Await'
                case 0:
                {
                    if(!__WFXApi->GetAsyncAPIV1()->RegisterAsyncTimer(
                        __WFXApi->GetHttpAPIV1()->GetGlobalPtrData(), delayMs
                    )) {
                        self->SetError(Error::TIMER_FAILURE);
                        self->Finish();
                        return;
                    }

                    // Succeeded, in next call, we will be in default state
                    self->IncState();
                    break;
                }
                default:
                    self->Finish();
                    break;
            }
        },
        delayMs
    );
}

} // namespace Async

#endif // WFX_INC_ASYNC_BUILTINS_HPP