// A program that constructs a machine for a platform with no core built.
//
// The machine's host is in this binary, because the program constructs a Vm; no core is, because the
// constructor it writes resolves to none, and constructing the machine throws. vm_linkage.cmake reads
// this binary's strings and finds no core's literal there. That holds in an optimized build, where the
// constant platform folds at the call site.

#include <retropp/version.h>
#include <retropp/vm.h>

#include <cstdio>
#include <stdexcept>
#include <string_view>

int main() {
    try {
        retropp::Vm vm{retropp::VMPlatform::Nes};
    } catch (const std::runtime_error& e) {
        std::printf("%s\n", e.what());
    }

    // The version string is what every linkage control prints, so the engine archive is on this link
    // line for a reason besides the machine.
    const std::string_view v = retropp::version();
    std::printf("%.*s\n", static_cast<int>(v.size()), v.data());
    return 0;
}
