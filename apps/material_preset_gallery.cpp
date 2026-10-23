// Reproducible CPU studio thumbnails and data assets for preset review.
#include "dve/editor_material_preview.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
namespace dve::editor {void render_material_panel(const IEditorCanvas&,NativeEditorController&);}
using namespace dve;using namespace dve::editor;
namespace {
std::string escape(std::string_view text){std::string out;for(char c:text){if(c=='&')out+="&amp;";else if(c=='<')out+="&lt;";else if(c=='>')out+="&gt;";else if(c=='\"')out+="&quot;";else out+=c;}return out;}
struct SvgCanvas: IEditorCanvas {
    mutable std::ostringstream svg;
    static std::string color(EditorColor c){std::ostringstream s;s<<'#'<<std::hex<<std::setw(6)<<std::setfill('0')<<c;return s.str();}
    void fill(UiRect r,EditorColor c)const override{svg<<"<rect x='"<<r.x<<"' y='"<<r.y<<"' width='"<<r.width<<"' height='"<<r.height<<"' fill='"<<color(c)<<"'/>\n";}
    void outline(UiRect r,EditorColor c)const override{svg<<"<rect x='"<<r.x<<"' y='"<<r.y<<"' width='"<<r.width<<"' height='"<<r.height<<"' fill='none' stroke='"<<color(c)<<"'/>\n";}
    void line(int x,int y,int x2,int y2,EditorColor c,int width)const override{svg<<"<path d='M "<<x<<' '<<y<<" L "<<x2<<' '<<y2<<"' stroke='"<<color(c)<<"' stroke-width='"<<width<<"'/>\n";}
    void text(int x,int y,std::string_view t,EditorColor c)const override{svg<<"<text x='"<<x<<"' y='"<<y<<"' fill='"<<color(c)<<"' font-family='monospace' font-size='11'>"<<escape(t)<<"</text>\n";}
    int text_width(std::string_view t)const override{return int(t.size())*7;}
};
std::shared_ptr<const MaterialPreviewImage> wait(MaterialPreviewWorker& worker){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);while(!worker.image()){if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("preview deadline");std::this_thread::sleep_for(std::chrono::milliseconds(10));}auto image=worker.image();if(!image->error.empty())throw std::runtime_error(image->error);return image;}
}
int main(int argc,char** argv){try{
    if(argc<2||argc>3)throw std::runtime_error("usage: dve_material_preset_gallery output-directory [--surfaces]");const bool surfaces=argc==3&&std::string_view(argv[2])=="--surfaces";const std::filesystem::path dir=argv[1];std::filesystem::create_directories(dir);auto catalog=MaterialPresetCatalog::starter();MaterialPreviewWorker worker;
    std::ofstream html(dir/"index.html");html<<"<!doctype html><meta charset='utf-8'><title>DVE material starter gallery</title><style>body{background:#0b0d12;color:#e2e9f2;font:16px system-ui;margin:36px}main{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:16px}article{background:#172133;padding:16px;border-radius:8px}img{width:128px;image-rendering:auto}small{color:#9eafc3}</style><h1>DVE starter materials</h1><p>CPU studio · fixed exposure · 64×64 · 32 paths · 8 bounces · 10 cm sphere. Artistic defaults. Sphere; in --surfaces mode also flat patch, rounded box, and patch with grain rotated 90 degrees. These are smoke-test previews; full image/backend acceptance remains separate.</p><main>";
    unsigned presetIndex=0,rendered=0;
    for(const auto& a:catalog.entries()){
        if(surfaces&&presetIndex++<19)continue;
        ++rendered;
        html<<"<article><h2>"<<escape(a.name)<<"</h2>";
        for(unsigned shape=0;shape<(surfaces?4U:1U);++shape){
        const auto stem=a.id+(shape?"-"+std::to_string(shape):"");
        auto material=a.defaults;
        if(shape==3)material.surfaceDetail.rotationRadians.z+=1.570796327F;
        worker.request(material,shape==0?MaterialPreviewShape::Sphere:shape==2?MaterialPreviewShape::RoundedBox:MaterialPreviewShape::Patch);auto image=wait(worker);std::ofstream ppm(dir/(stem+".ppm"),std::ios::binary);ppm<<"P6\n"<<image->size<<' '<<image->size<<"\n255\n";
        for(auto pixel:image->rgb){const char bytes[]{char(pixel>>16),char(pixel>>8),char(pixel)};ppm.write(bytes,3);}
        std::ofstream bmp(dir/(stem+".bmp"),std::ios::binary);
        const auto u16=[&](unsigned v){bmp.put(char(v));bmp.put(char(v>>8));};const auto u32=[&](unsigned v){u16(v);u16(v>>16);};
        bmp<<"BM";u32(54+image->size*image->size*3);u32(0);u32(54);u32(40);u32(image->size);u32(image->size);u16(1);u16(24);u32(0);u32(image->size*image->size*3);u32(2835);u32(2835);u32(0);u32(0);
        for(unsigned y=image->size;y>0;--y)for(unsigned x=0;x<image->size;++x){const auto pixel=image->rgb[(y-1)*image->size+x];bmp.put(char(pixel));bmp.put(char(pixel>>8));bmp.put(char(pixel>>16));}
        std::string error;
        if(!write_material_preset(dir/(a.id+".dvepreset"),a,&error))throw std::runtime_error(error);
        html<<"<img src='"<<stem<<".bmp' alt='"<<escape(a.name)<<" shape "<<shape<<"'>";
        }
        html<<"<p>"<<escape(a.description)<<"</p><small>"<<escape(a.provenance)<<"</small></article>";
    }html<<"</main>";
    NativeEditorController controller;controller.resize(1280,800);controller.open_material_panel();controller.material_panel_select("dve.glass.frosted");wait(*controller.material_panel().preview);
    const auto count=(std::min)(controller.material_panel_layout().rows.size(),controller.material_panel_entries().size());
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(controller.material_panel().thumbnailCache.size()<count&&std::chrono::steady_clock::now()<deadline){controller.material_panel_tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    SvgCanvas svg;render_material_panel(svg,controller);std::ofstream screenshot(dir/"material-panel.svg");screenshot<<"<svg xmlns='http://www.w3.org/2000/svg' width='1280' height='800'><rect width='1280' height='800' fill='#0b0d12'/>"<<svg.svg.str()<<"</svg>";
    controller.close_material_panel();std::cout<<"Rendered "<<rendered<<" presets and native panel SVG\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
