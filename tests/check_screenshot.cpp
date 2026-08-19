// Reads a PPM written by `dualscreen --screenshot` and asserts that particular
// regions of the output show particular screens. The mock core paints the top
// screen blue-dominant and the bottom screen green-dominant, so checking which
// hue lands where verifies the framebuffer split, the layout placement and the
// compositor together.
//
// Usage: check_screenshot <file.ppm> <region>=<colour> [...]
//   regions: upper lower left right center topleft
//   colours: blue green black orange
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

        if (region == "differs") {
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
            // passing on an accidental single-pixel wobble.
            const bool ok = differing > 1000;
            std::printf("%-9s expected a different picture  got %-16ld -> %s\n", "differs",
                        differing, ok ? "ok" : "FAIL");
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

        bool passed = false;
        if (expected == "blue") {
            passed = (b > g + 8.0) && (b > r + 8.0);
        } else if (expected == "green") {
            passed = (g > b + 8.0) && (g > r + 8.0);
        } else if (expected == "orange") {
            // The mock core paints this band only when it found save data.
            passed = (r > 150.0) && (g > 60.0) && (g < 200.0) && (b < 80.0);
        } else if (expected == "black") {
            // The letterbox around the screens has to be genuinely black, not
            // merely dark: a lifted background reads as a frame on an OLED.
            passed = (r < 2.0) && (g < 2.0) && (b < 2.0);
        } else {
            std::fprintf(stderr, "check_screenshot: unknown colour '%s'\n", expected.c_str());
            ++failures;
            continue;
        }

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
