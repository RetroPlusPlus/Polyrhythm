// A program that constructs a pre-bound SNES machine.
//
// Vm::SNES names the SNES core in its own constructor, so the core is in this binary under every
// configuration. vm_linkage.cmake reads this binary's strings and finds the core's literal there.

#include <retropp/version.h>
#include <retropp/vm.h>

#include <cstdio>
#include <string_view>

int main() {
    retropp::Vm::SNES vm;

    // The version string is what every linkage control prints, so the engine archive is on this link
    // line for a reason besides the machine.
    const std::string_view v = retropp::version();
    std::printf("%.*s\n", static_cast<int>(v.size()), v.data());
    return 0;
}
