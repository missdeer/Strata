#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
int main() {
    std::cout << "READY 4" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "QUIT")
            break;
        std::istringstream in(line);
        std::string kind, input, output;
        in >> kind >> input >> output;
        if (kind != "ENC") {
            std::cout << "ERR expected ENC" << std::endl;
            continue;
        }
        if (const auto *capture = std::getenv("STRATA_TEST_IMAGE_CAPTURE")) {
            std::ofstream log(std::filesystem::u8path(std::string(capture)), std::ios::app);
            log << input << '\n';
        }
        std::ifstream image(input, std::ios::binary);
        if (!image) {
            std::cout << "ERR no image" << std::endl;
            continue;
        }
        int32_t header[]{0x31455653, 4, 2, 2, 4};
        float pixels[16]{};
        std::ofstream file(output, std::ios::binary);
        file.write(reinterpret_cast<char *>(header), sizeof(header));
        file.write(reinterpret_cast<char *>(pixels), sizeof(pixels));
        file.close();
        std::cout << "OK 4 2 2 1" << std::endl;
    }
}
