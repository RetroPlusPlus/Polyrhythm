// A program that constructs a SNES machine from its platform value.
//
// The constructor resolves the platform's core where this program writes it, so the SNES core is in
// this binary. vm_linkage.cmake reads this binary's strings and finds the core's literal there.

#include <retropp/version.h>
#include <retropp/vm.h>

#include <cstdio>
#include <string_view>

int main() {
    retropp::Vm vm{retropp::VMPlatform::Snes};

    // The version string is what every linkage control prints, so the engine archive is on this link
    // line for a reason besides the machine.
    const std::string_view v = retropp::version();
    std::printf("%.*s\n", static_cast<int>(v.size()), v.data());
    return 0;
}
