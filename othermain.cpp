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
