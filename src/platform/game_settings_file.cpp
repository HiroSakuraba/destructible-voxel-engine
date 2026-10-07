#include "dve/game_ui.hpp"
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dve::ui {
bool GameSettings::save(const std::filesystem::path& path,std::string* error) const {
    if(error)error->clear();
    const auto fail=[&](const char* message){if(error)*error=message;return false;};
    if(!validate(error))return false;
    std::error_code ec;
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path(),ec);
    if(ec)return fail("could not create game-settings directory");
    auto temporary=path;temporary+=".tmp";
    {
        std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
        const auto text=serialize();out.write(text.data(),static_cast<std::streamsize>(text.size()));out.close();
        if(!out){std::filesystem::remove(temporary,ec);return fail("could not write game settings");}
    }
#ifdef _WIN32
    const bool replaced=MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    std::filesystem::rename(temporary,path,ec);const bool replaced=!ec;
#endif
    if(!replaced){std::filesystem::remove(temporary,ec);return fail("could not replace game settings");}
    return true;
}
} // namespace dve::ui
