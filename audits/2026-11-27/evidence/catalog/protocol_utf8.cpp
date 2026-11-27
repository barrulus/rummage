#include <iostream>
#include <nlohmann/json.hpp>
#include "protocol.hpp"
int main() {
    rummage::picture::PhotoInfo input;
    for (int index = 0; index < 128; ++index) input.camera += "\xC3\xA9";
    auto decoded = rummage::isolate::protocol::decode_info(rummage::isolate::protocol::encode_info(input));
    if (!decoded) return 2;
    std::cout << "Input camera bytes: " << input.camera.size() << "\nDecoded camera bytes: " << decoded->camera.size() << '\n';
    try {
        std::cout << nlohmann::json{{"op","metadata"},{"camera", decoded->camera}}.dump() << '\n';
        return 1;
    } catch (const nlohmann::json::exception& error) {
        std::cout << "Confirmed JSON boundary failure: " << error.what() << '\n';
    }
}
