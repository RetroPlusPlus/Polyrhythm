// A program that links the platform, calls into it, and declares no transport.
//
// The transport builds as its own static library, and an archive member reaches a link only to satisfy
// an undefined symbol. This program names the platform's version string and names nothing under
// retropp::net, so the linker has no reason to pull a socket into the binary it produces.
//
// net_linkage.cmake reads this binary's symbols alongside the test binary's, which does name the seam.
// The transport's symbols belong in that one and stay out of this one; asserting both directions is
// what separates a real measurement from a grep that happens to find nothing.

#include <retropp/version.h>

#include <cstdio>
#include <string_view>

int main() {
    // Calling the platform is what gives the measurement its meaning: the engine archive is on this
    // link line either way, so an absent socket symbol is the linker's decision rather than an absent
    // library.
    const std::string_view v = retropp::version();
    std::printf("%.*s\n", static_cast<int>(v.size()), v.data());
    return 0;
}
