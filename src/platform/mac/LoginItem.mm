#include "platform/mac/LoginItem.h"

#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>

namespace fixomic::mac {

namespace {

juce::String describe(NSError* error)
{
    return error != nil ? juce::String::fromUTF8(error.localizedDescription.UTF8String) : juce::String("errore sconosciuto");
}

} // namespace

bool launchAtLoginEnabled()
{
    return SMAppService.mainAppService.status == SMAppServiceStatusEnabled;
}

bool launchAtLoginRequiresApproval()
{
    return SMAppService.mainAppService.status == SMAppServiceStatusRequiresApproval;
}

bool setLaunchAtLogin(bool enabled, juce::String& error)
{
    SMAppService* service = SMAppService.mainAppService;
    NSError* failure = nil;
    if (enabled) {
        if (service.status != SMAppServiceStatusEnabled && ![service registerAndReturnError:&failure]) {
            error = "Impossibile attivare l'avvio all'accesso: " + describe(failure);
            return false;
        }
        if (service.status == SMAppServiceStatusRequiresApproval) {
            error = juce::String::fromUTF8("Approva fixomic in Impostazioni di Sistema → Generali → Elementi login ed estensioni");
            return false;
        }
        return true;
    }
    if (service.status == SMAppServiceStatusNotRegistered || service.status == SMAppServiceStatusNotFound)
        return true;
    if (![service unregisterAndReturnError:&failure] && failure.code != kSMErrorJobNotFound) {
        error = "Impossibile disattivare l'avvio all'accesso: " + describe(failure);
        return false;
    }
    return true;
}

} // namespace fixomic::mac
