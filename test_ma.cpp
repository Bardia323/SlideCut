#include <iostream>
#include <string>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    std::string path = argv[1];
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 2, 48000);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &dc, &dec) != MA_SUCCESS) {
        std::cout << "Failed to decode" << std::endl;
        return 1;
    }
    ma_uint64 len = 0;
    ma_decoder_get_length_in_pcm_frames(&dec, &len);
    std::cout << "Success! Frames: " << len << std::endl;
    ma_decoder_uninit(&dec);
    return 0;
}
