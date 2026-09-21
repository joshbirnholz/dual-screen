// Which GL context each platform is asked for.
//
// This is the sort of thing that cannot be noticed from the machine it is
// written on, and getting it wrong does not misbehave subtly: a platform
// offered only contexts it does not have gets no context at all, which is an
// app that will not start. So gl_context_attempts() answers for a platform
// rather than for *this* platform -- see GlFamily -- and this asks it about
// all three from Linux, which is the only one of them this can be run on.
//
// The immediate reason it exists: the Apple branch of that list said "desktop
// GL, no ES", which is true of macOS and is the exact opposite of the truth on
// iOS and tvOS. Apple is two answers.

#if defined(__APPLE__)
// TARGET_OS_IPHONE and TARGET_OS_TV are used below to check that this build
// answers for the platform it is. Undefined they are zero, which would quietly
// assert the macOS answer on an iPhone -- so the header is included rather than
// left to arrive through SDL, which is where it happens to come from today.
#include <TargetConditionals.h>
#endif

#include <string>
#include <vector>

#include "render/gl_window.hpp"
#include "test_support.hpp"

using namespace ds;

namespace {

bool offers_es(const std::vector<GlContextAttempt>& attempts) {
    for (const GlContextAttempt& attempt : attempts) {
        if (attempt.profile == SDL_GL_CONTEXT_PROFILE_ES) return true;
    }
    return false;
}

bool offers_desktop(const std::vector<GlContextAttempt>& attempts) {
    for (const GlContextAttempt& attempt : attempts) {
        if (attempt.profile == SDL_GL_CONTEXT_PROFILE_CORE ||
            attempt.profile == SDL_GL_CONTEXT_PROFILE_COMPATIBILITY) {
            return true;
        }
    }
    return false;
}

// Every list has to end somewhere a driver cannot refuse.
void test_every_list_ends_with_the_drivers_own_default() {
    for (GlFamily family :
         {GlFamily::EitherKind, GlFamily::DesktopOnly, GlFamily::EmbeddedOnly}) {
        const std::vector<GlContextAttempt> attempts =
            gl_context_attempts(family, GlContextNeed::Any);
        CHECK(!attempts.empty());
        CHECK_EQ(attempts.back().profile, 0);
    }
}

// The one this was written for. iOS and tvOS have OpenGL ES and no desktop GL
// whatsoever; macOS has desktop GL and no ES whatsoever. Offering a platform
// the other one's list is not a slow fallback, it is every attempt failing.
void test_a_platform_is_never_offered_a_context_it_does_not_have() {
    const std::vector<GlContextAttempt> embedded =
        gl_context_attempts(GlFamily::EmbeddedOnly, GlContextNeed::Any);
    CHECK_MSG(offers_es(embedded), "iOS and tvOS have ES and it was not offered");
    CHECK_MSG(!offers_desktop(embedded),
              "iOS and tvOS have no desktop GL, so asking for it is a guaranteed failure");

    const std::vector<GlContextAttempt> desktop =
        gl_context_attempts(GlFamily::DesktopOnly, GlContextNeed::Any);
    CHECK_MSG(offers_desktop(desktop), "macOS has desktop GL and it was not offered");
    CHECK_MSG(!offers_es(desktop),
              "macOS has no ES at all, so asking for it is two refusals before the real attempt");

    // And the platforms that really do have both are still offered both.
    const std::vector<GlContextAttempt> either =
        gl_context_attempts(GlFamily::EitherKind, GlContextNeed::Any);
    CHECK(offers_es(either) && offers_desktop(either));
}

// Which comes first is not arbitrary either: ES 2.0 is the common subset the
// renderer is written against, so it leads wherever it exists -- except on
// macOS, where the legacy profile is a museum piece and core is the real one.
void test_the_order_puts_the_platforms_own_first_choice_first() {
    const std::vector<GlContextAttempt> either =
        gl_context_attempts(GlFamily::EitherKind, GlContextNeed::Any);
    CHECK_EQ(either.front().profile, SDL_GL_CONTEXT_PROFILE_ES);
    CHECK(either.front().gles);

    const std::vector<GlContextAttempt> desktop =
        gl_context_attempts(GlFamily::DesktopOnly, GlContextNeed::Any);
    CHECK_EQ(desktop.front().profile, SDL_GL_CONTEXT_PROFILE_CORE);
    CHECK_EQ(desktop.front().major, 3);
    CHECK_EQ(desktop.front().minor, 3);
    CHECK(!desktop.front().gles);

    // Every ES entry has to be marked as one: the flag is what picks the
    // shader preamble, and a context drawn with the wrong one compiles
    // nothing.
    for (GlFamily family :
         {GlFamily::EitherKind, GlFamily::DesktopOnly, GlFamily::EmbeddedOnly}) {
        for (const GlContextAttempt& attempt :
             gl_context_attempts(family, GlContextNeed::Any)) {
            if (attempt.profile == SDL_GL_CONTEXT_PROFILE_ES) {
                CHECK_MSG(attempt.gles, "an ES context was not marked as one");
            }
            if (attempt.profile == SDL_GL_CONTEXT_PROFILE_CORE ||
                attempt.profile == SDL_GL_CONTEXT_PROFILE_COMPATIBILITY) {
                CHECK_MSG(!attempt.gles, "a desktop context was marked as ES");
            }
        }
    }
}

// A core that renders on the GPU needs desktop 3.3, so the ES entries drop out
// -- an ES context there would only mean starting up and then failing to open
// the game. On a platform with no desktop GL the honest answer is that there
// is no such context, which is not the same as offering one that cannot.
void test_a_core_that_needs_desktop_gl_is_not_offered_es_instead() {
    for (GlFamily family : {GlFamily::EitherKind, GlFamily::DesktopOnly}) {
        const std::vector<GlContextAttempt> attempts =
            gl_context_attempts(family, GlContextNeed::OpenGl33Core);
        CHECK(!attempts.empty());
        CHECK_MSG(!offers_es(attempts), "a GPU core was offered an ES context it cannot use");
        CHECK_EQ(attempts.front().major, 3);
        CHECK_EQ(attempts.front().minor, 3);
    }
    // No host runs on iOS or tvOS and none is built for one, so this is a
    // question that should never be asked there -- and an empty list says so
    // where a context that cannot run a core would not.
    CHECK(gl_context_attempts(GlFamily::EmbeddedOnly, GlContextNeed::OpenGl33Core).empty());
}

// And the machine this is running on gets the answer for the machine it is.
void test_this_build_knows_which_platform_it_is() {
#if defined(__APPLE__)
#if TARGET_OS_IPHONE
    CHECK(native_gl_family() == GlFamily::EmbeddedOnly);
#else
    CHECK(native_gl_family() == GlFamily::DesktopOnly);
#endif
#else
    CHECK(native_gl_family() == GlFamily::EitherKind);
#endif
}

}  // namespace

int main() {
    test_every_list_ends_with_the_drivers_own_default();
    test_a_platform_is_never_offered_a_context_it_does_not_have();
    test_the_order_puts_the_platforms_own_first_choice_first();
    test_a_core_that_needs_desktop_gl_is_not_offered_es_instead();
    test_this_build_knows_which_platform_it_is();
    return test::finish("gl-window");
}
