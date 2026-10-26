#include "dve/editor_prototype.hpp"
#include <filesystem>
#include <iostream>
int main(int argc,char** argv){
 if(argc!=2){std::cerr<<"Usage: dve_create_water NEW_PROJECT_DIRECTORY\n";return 2;}
 try{
  const auto document=dve::editor::make_water_starter();
  const auto materials=dve::editor::EditorMaterialLibrary::make_default();
  std::string error;
  if(!dve::editor::export_prototype_project(document,materials,
       std::filesystem::absolute(argv[1]),&error)){
     std::cerr<<error<<'\n';return 1;
  }
  std::cout<<"Created Water Test Range: "<<argv[1]<<'\n';return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
