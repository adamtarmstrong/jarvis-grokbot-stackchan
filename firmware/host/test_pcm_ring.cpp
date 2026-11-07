#include "pcm_ring.hpp"
#include <cassert>
#include <cstdint>

int main() {
    jarvis::PcmRing ring;
    std::int16_t out[8]{};
    assert(ring.empty());
    assert(!ring.push(1));
    assert(ring.pop(out, 8) == 0);
    std::int16_t data[4]{};
    ring.bind(data, 4);
    for (int i = 0; i < 4; ++i) assert(ring.push(i));
    assert(!ring.push(99)); // Full must not silently delete earlier words.
    assert(ring.pop(out, 2) == 2 && out[0] == 0 && out[1] == 1);
    assert(ring.push(4) && ring.push(5));
    assert(ring.pop(out, 8) == 4);
    for (int i = 0; i < 4; ++i) assert(out[i] == i + 2);
    for (int pass = 0; pass < 1000; ++pass) {
        assert(ring.push(-123));
        assert(ring.pop(out, 1) == 1 && out[0] == -123);
    }
    assert(ring.push(7));
    ring.clear();
    assert(ring.empty() && ring.pop(out, 8) == 0);
}
