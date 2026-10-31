#include "dve/player/indirect_lighting_ownership.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace dve::player;
#define CHECK(c) do{if(!(c))throw std::runtime_error("indirect ownership: " #c);}while(false)
int main(){
    try{
        for(int enabled=0;enabled<2;++enabled)
            for(int requested=0;requested<2;++requested)
                for(int available=0;available<2;++available)
                    for(int experiment=0;experiment<2;++experiment){
                        const auto owner=choose_indirect_diffuse_owner(enabled,requested,available,experiment);
                        const bool split=uses_split_cascades(owner);
                        const bool screen=uses_screen_cascades(owner);
                        CHECK(!(split&&screen));
                        CHECK(uses_traced_diffuse(owner)==(enabled!=0));
                        if(!enabled)CHECK(owner==IndirectDiffuseOwner::EnvironmentIbl);
                        else if(!requested||!available)CHECK(owner==IndirectDiffuseOwner::TracedOneBounce);
                        else if(experiment)CHECK(split);
                        else CHECK(screen);
                    }
        // A globally set experimental flag MUST NOT override a one-bounce GI
        // request or an inactive GI mode.
        CHECK(choose_indirect_diffuse_owner(true,false,true,true)==IndirectDiffuseOwner::TracedOneBounce);
        CHECK(choose_indirect_diffuse_owner(false,true,true,true)==IndirectDiffuseOwner::EnvironmentIbl);
        std::cout<<"exclusive diffuse indirect lighting policy PASS\n";
        return EXIT_SUCCESS;
    }catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return EXIT_FAILURE;}
}
