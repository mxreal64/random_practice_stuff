// Copyright (C) 2026 mxreal64
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://gnu.org>.

import std;
import DrmDisplay;

int main() {
    DrmScreen screen;
    
    if (!screen.init()) {
        std::print(std::cerr, "Error: Could not grab control of /dev/dri/card0 hardware context.\n");
        return 1;
    }

    auto [width, height] = screen.get_resolution();
    std::println("Direct Hardware Buffer Active: {}x{} Resolution.", width, height);

    screen.clear(0x00330066); 

    uint32_t mid_y = height / 2;
    for (uint32_t x = 0; x < width; ++x) {
        screen.set_pixel(x, mid_y, 0x00FFFFFF);
    }

    std::println("Display updated. Suspending application execution thread for 5 seconds...");
    std::this_thread::sleep_for(std::chrono::seconds(5));

    return 0;
}
