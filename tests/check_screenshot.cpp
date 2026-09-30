// Reads a PPM written by `dualscreen --screenshot` and asserts that particular
// regions of the output show particular screens. The mock core paints the top
// screen blue-dominant and the bottom screen green-dominant, so checking which
// hue lands where verifies the framebuffer split, the layout placement and the
// compositor together.
//
// Usage: check_screenshot <file.ppm> <region>=<colour> [...]
//   regions: upper lower left right center topleft
//   colours: blue green black orange bezel shell
//
// `text=<x0>,<y0>,<x1>,<y1>` checks a band holds readable glyphs, not a block.
// `at=<x>,<y>,<colour>` checks one point given as fractions of the window, for
// the layouts that put a screen somewhere none of the named regions land.
//
// `same=<other.ppm>` is the inverse of `differs`, for checking that something
// which should have had no effect had none.
//
// `differs=<other.ppm>` asserts that two runs did not produce the same picture.
// That is how the smoothing toggle is checked: nearest and linear sampling of
// the same frame differ in thousands of pixels along every edge, and comparing
// two whole runs needs no guess about which pixel to look at.
//
// It also understands `crosshair=<x>,<y>`, which finds the mock core's bright
// yellow touch crosshair and checks it landed at that normalized position in
// the window. Because the mock draws the crosshair at the DS coordinate the
// frontend reported, and the compositor draws that screen back into the same
// place, the crosshair should appear exactly where the touch was: that round
// trip is the whole touch-mapping path in one assertion.

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
    bool ok = false;
};

int read_ppm_token(std::istream& stream) {
    int value = 0;
    char character = 0;
    // Skip whitespace and '#' comments.
    while (stream.get(character)) {
        if (character == '#') {
            while (stream.get(character) && character != '\n') {
            }
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(character))) {
            stream.unget();
            break;
        }
    }
    stream >> value;
    return value;
}

Image load_ppm(const std::string& path) {
    Image image;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::fprintf(stderr, "check_screenshot: cannot open %s\n", path.c_str());
        return image;
    }
    std::string magic;
    file >> magic;
    if (magic != "P6") {
        std::fprintf(stderr, "check_screenshot: %s is not a P6 PPM\n", path.c_str());
        return image;
    }
    image.width = read_ppm_token(file);
    image.height = read_ppm_token(file);
    const int max_value = read_ppm_token(file);
    file.get();  // the single whitespace byte before the pixel data
    if (image.width <= 0 || image.height <= 0 || max_value != 255) {
        std::fprintf(stderr, "check_screenshot: bad PPM header in %s\n", path.c_str());
        return image;
    }
    image.rgb.resize(static_cast<std::size_t>(image.width) * image.height * 3);
    file.read(reinterpret_cast<char*>(image.rgb.data()), static_cast<std::streamsize>(image.rgb.size()));
    image.ok = file.gcount() == static_cast<std::streamsize>(image.rgb.size());
    if (!image.ok) {
        std::fprintf(stderr, "check_screenshot: %s is truncated\n", path.c_str());
    }
    return image;
}

// Averages a patch around a point, so a single stray pixel cannot decide the test.
void average_patch(const Image& image, float cx, float cy, double& r, double& g, double& b) {
    const int radius = 12;
    const int center_x = static_cast<int>(cx * image.width);
    const int center_y = static_cast<int>(cy * image.height);
    double sum_r = 0, sum_g = 0, sum_b = 0;
    int samples = 0;
    for (int y = center_y - radius; y <= center_y + radius; ++y) {
        if (y < 0 || y >= image.height) continue;
        for (int x = center_x - radius; x <= center_x + radius; ++x) {
            if (x < 0 || x >= image.width) continue;
            const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 3;
            sum_r += image.rgb[offset + 0];
            sum_g += image.rgb[offset + 1];
            sum_b += image.rgb[offset + 2];
            ++samples;
        }
    }
    if (samples == 0) samples = 1;
    r = sum_r / samples;
    g = sum_g / samples;
    b = sum_b / samples;
}

// The one place a colour name is decided, so a named region and an explicit
// point can never disagree about what "blue" means.
bool matches_colour(const std::string& expected, double r, double g, double b) {
    if (expected == "blue") {
        return (b > g + 8.0) && (b > r + 8.0);
    }
    if (expected == "green") {
        return (g > b + 8.0) && (g > r + 8.0);
    }
    if (expected == "orange") {
        // The mock core paints this band only when it found save data.
        return (r > 150.0) && (g > 60.0) && (g < 200.0) && (b < 80.0);
    }
    if (expected == "dialog") {
        // A popup's box: the Switch's dark grey, #252628 (see ui/popup.hpp),
        // grey enough that no dimmed colour a mock core draws matches it.
        return std::fabs(r - 37.0) < 6.0 && std::fabs(g - 38.0) < 6.0 && std::fabs(b - 40.0) < 6.0;
    }
    if (expected == "black") {
        // The letterbox around the screens has to be genuinely black, not
        // merely dark: a lifted background reads as a frame on an OLED.
        return (r < 2.0) && (g < 2.0) && (b < 2.0);
    }
    if (expected == "bezel") {
        // The surround the Standard layout uses: the console artwork's own
        // background colour, #252525, rather than the black everything else
        // sits on.
        return std::fabs(r - 37.0) < 6.0 && std::fabs(g - 37.0) < 6.0 &&
               std::fabs(b - 37.0) < 6.0;
    }
    if (expected == "shell") {
        // The Standard layout's console body. Near enough grey that no screen of
        // the mock core's could be mistaken for it, and clearly brighter than
        // the flat #252525 the artwork sits on -- which is what separates "the
        // console is drawn here" from "this is the surround". A DS's shell is
        // light plastic and a 3DS's is charcoal, so the test is the shape of the
        // colour rather than how bright it is.
        return (r > 42.0) && (g > 42.0) && (b > 42.0) && std::fabs(r - b) < 30.0 &&
               std::fabs(r - g) < 30.0;
    }
    std::fprintf(stderr, "check_screenshot: unknown colour '%s'\n", expected.c_str());
    return false;
}

bool region_center(const std::string& region, float& x, float& y) {
    if (region == "topleft"){ x = 0.03f; y = 0.06f; return true; }
    if (region == "upper")  { x = 0.5f; y = 0.25f; return true; }
    if (region == "lower")  { x = 0.5f; y = 0.75f; return true; }
    if (region == "left")   { x = 0.25f; y = 0.5f; return true; }
    if (region == "right")  { x = 0.75f; y = 0.5f; return true; }
    if (region == "center") { x = 0.5f; y = 0.5f; return true; }
    return false;
}

// Finds the centroid of the mock core's crosshair colour, rgb(255, 240, 80).
bool find_crosshair(const Image& image, double& out_x, double& out_y) {
    double sum_x = 0;
    double sum_y = 0;
    long samples = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 3;
            const int r = image.rgb[offset + 0];
            const int g = image.rgb[offset + 1];
            const int b = image.rgb[offset + 2];
            if (r > 200 && g > 190 && b < 140 && g < r + 20) {
                sum_x += x;
                sum_y += y;
                ++samples;
            }
        }
    }
    if (samples < 20) {
        return false;
    }
    out_x = sum_x / samples / image.width;
    out_y = sum_y / samples / image.height;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: check_screenshot <file.ppm> <region>=<colour> [...]\n");
        return 2;
    }

    const Image image = load_ppm(argv[1]);
    if (!image.ok) {
        return 1;
    }

    int failures = 0;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        const std::size_t equals = argument.find('=');
        if (equals == std::string::npos) {
            std::fprintf(stderr, "check_screenshot: bad expectation '%s'\n", argument.c_str());
            ++failures;
            continue;
        }
        const std::string region = argument.substr(0, equals);
        const std::string expected = argument.substr(equals + 1);

        if (region == "at") {
            const std::size_t first = expected.find(',');
            const std::size_t second = expected.find(',', first + 1);
            if (first == std::string::npos || second == std::string::npos) {
                std::fprintf(stderr, "check_screenshot: at wants x,y,colour\n");
                ++failures;
                continue;
            }
            const float x = std::strtof(expected.substr(0, first).c_str(), nullptr);
            const float y = std::strtof(expected.substr(first + 1, second - first - 1).c_str(),
                                        nullptr);
            const std::string colour = expected.substr(second + 1);
            double r = 0, g = 0, b = 0;
            average_patch(image, x, y, r, g, b);
            const bool ok = matches_colour(colour, r, g, b);
            std::printf("%-9s expected %-6s at %.2f,%.2f  got rgb(%3.0f, %3.0f, %3.0f) -> %s\n",
                        "at", colour.c_str(), x, y, r, g, b, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
            continue;
        }

        // `text=<x0>,<y0>,<x1>,<y1>`: the given band has to hold readable glyphs
        // rather than a solid block. Text is drawn from a coverage mask, so it
        // only looks like letters while alpha blending is on -- with blending
        // that ignores source alpha, every glyph cell fills solid and the band
        // becomes one flat colour. Checking that light and dark pixels are both
        // present is what tells those two apart.
        if (region == "text") {
            float bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            std::size_t start = 0;
            bool parsed = true;
            for (int part = 0; part < 4; ++part) {
                const std::size_t comma = expected.find(',', start);
                if (part < 3 && comma == std::string::npos) {
                    parsed = false;
                    break;
                }
                bounds[part] = std::strtof(expected.substr(start, comma - start).c_str(), nullptr);
                start = comma + 1;
            }
            if (!parsed) {
                std::fprintf(stderr, "check_screenshot: text wants x0,y0,x1,y1\n");
                ++failures;
                continue;
            }
            const int x0 = static_cast<int>(bounds[0] * image.width);
            const int y0 = static_cast<int>(bounds[1] * image.height);
            const int x1 = static_cast<int>(bounds[2] * image.width);
            const int y1 = static_cast<int>(bounds[3] * image.height);
            long light = 0;
            long dark = 0;
            for (int y = y0; y < y1 && y < image.height; ++y) {
                for (int x = x0; x < x1 && x < image.width; ++x) {
                    const std::size_t offset =
                        (static_cast<std::size_t>(y) * image.width + x) * 3;
                    const int luma = (image.rgb[offset] + image.rgb[offset + 1] +
                                      image.rgb[offset + 2]) / 3;
                    if (luma > 140) ++light;
                    if (luma < 70) ++dark;
                }
            }
            // The strokes of a 5x7 glyph light up a small share of the band they
            // sit in -- around a tenth, for these labels. When blending ignores
            // source alpha, every glyph's whole cell fills with the text colour
            // instead, roughly tripling that share. So the test is that the lit
            // area stays well under what solid cells would produce, while still
            // being enough to be text at all rather than an empty strip.
            const long total = light + dark;
            const double lit = total > 0 ? static_cast<double>(light) / total : 0.0;
            const bool ok = total > 0 && lit > 0.01 && lit < 0.18;
            std::printf("%-9s expected %-21s got %.0f%% of the band lit -> %s\n", "text",
                        "glyphs, not blocks", lit * 100.0, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
            continue;
        }

        if (region == "differs" || region == "same") {
            const bool want_difference = (region == "differs");
            const Image other = load_ppm(expected);
            if (!other.ok) {
                ++failures;
                continue;
            }
            long differing = 0;
            if (other.width == image.width && other.height == image.height) {
                for (std::size_t byte = 0; byte < image.rgb.size(); byte += 3) {
                    if (image.rgb[byte] != other.rgb[byte] ||
                        image.rgb[byte + 1] != other.rgb[byte + 1] ||
                        image.rgb[byte + 2] != other.rgb[byte + 2]) {
                        ++differing;
                    }
                }
            } else {
                std::fprintf(stderr, "check_screenshot: %s and %s are different sizes\n", argv[1],
                             expected.c_str());
                ++failures;
                continue;
            }
            // Smoothing repaints every scaled edge, so the difference is in the
            // thousands. A threshold well above stray noise keeps this from
            // passing on an accidental single-pixel wobble -- and, the other way
            // round, lets "same" tolerate the mock core's own moving parts.
            const bool ok = want_difference ? (differing > 1000) : (differing <= 1000);
            std::printf("%-9s expected %-21s got %-16ld -> %s\n", region.c_str(),
                        want_difference ? "a different picture" : "the same picture", differing,
                        ok ? "ok" : "FAIL");
            if (!ok) ++failures;
            continue;
        }

        if (region == "nocrosshair") {
            double got_x = 0;
            double got_y = 0;
            const bool found = find_crosshair(image, got_x, got_y);
            std::printf("%-9s expected none                 got %-16s -> %s\n", "crosshair",
                        found ? "a crosshair" : "none", found ? "FAIL" : "ok");
            if (found) {
                std::fprintf(stderr,
                             "a touch was passed through on a layout that shows no bottom screen "
                             "(crosshair at %.3f, %.3f)\n",
                             got_x, got_y);
                ++failures;
            }
            continue;
        }

        if (region == "crosshair") {
            const std::size_t comma = expected.find(',');
            if (comma == std::string::npos) {
                std::fprintf(stderr, "check_screenshot: crosshair wants x,y\n");
                ++failures;
                continue;
            }
            const double want_x = std::atof(expected.substr(0, comma).c_str());
            const double want_y = std::atof(expected.substr(comma + 1).c_str());
            double got_x = 0;
            double got_y = 0;
            if (!find_crosshair(image, got_x, got_y)) {
                std::fprintf(stderr, "crosshair expected at (%.3f, %.3f) but none was drawn\n",
                             want_x, want_y);
                ++failures;
                continue;
            }
            // One DS pixel is ~0.4%% of a screen's width, and the crosshair is
            // a few pixels wide, so 1.5%% of the window is a tight bound.
            const bool passed = std::fabs(got_x - want_x) < 0.015 && std::fabs(got_y - want_y) < 0.015;
            std::printf("%-9s expected (%.3f, %.3f) got (%.3f, %.3f) -> %s\n", "crosshair",
                        want_x, want_y, got_x, got_y, passed ? "ok" : "FAIL");
            if (!passed) ++failures;
            continue;
        }

        float x = 0.0f;
        float y = 0.0f;
        if (!region_center(region, x, y)) {
            std::fprintf(stderr, "check_screenshot: unknown region '%s'\n", region.c_str());
            ++failures;
            continue;
        }

        double r = 0, g = 0, b = 0;
        average_patch(image, x, y, r, g, b);

        const bool passed = matches_colour(expected, r, g, b);
        std::printf("%-7s expected %-5s got rgb(%.0f, %.0f, %.0f) -> %s\n", region.c_str(),
                    expected.c_str(), r, g, b, passed ? "ok" : "FAIL");
        if (!passed) {
            ++failures;
        }
    }

    if (failures > 0) {
        std::fprintf(stderr, "check_screenshot: %d expectation(s) failed for %s\n", failures, argv[1]);
        return 1;
    }
    return 0;
}
