#define main generic_view_stress_main
#include "view_stress.cpp"
#undef main
#include "rummage/views/photo_library.hpp"
#include "rummage/views/state.hpp"
int main(int argc,char**argv){try{
 const size_t n=argc>1?std::stoull(argv[1]):1000;
 log::set_level(log::Level::off);
 core::EventLoop loop;core::WorkerPool pool(loop,2);fs::DirectoryWatcher watcher(loop);auto mime=fs::MimeDatabase::load();
 ui::Offscreen gl(1100,820);ui::FontSystem fonts;ui::Renderer renderer(fonts);ui::IconSet icons(fonts);theme::Palette palette;
 views::Browser browser(loop,pool,watcher,mime,[]{},[](ui::CursorShape){});
 views::State state;state.photo_library=nlohmann::json{{"version",2},{"filter",nlohmann::json::object()},{"loaded",n},{"cursor",n},{"selected",nlohmann::json::array({n})}}.dump();browser.apply(state);browser.set_info_visible(false);core::MotionConfig motion;motion.enabled=false;browser.set_motion_config(motion);browser.set_effects_active(false);
 auto started=Clock::now();browser.open_photo_library();
 auto wait=[&](auto predicate){bool ok=false;auto p=loop.add_timer(1ms,1ms,[&]{if(predicate()){ok=true;loop.quit(0);}});auto w=loop.add_timer(90s,0ms,[&]{loop.quit(1);});loop.run();loop.remove(p);loop.remove(w);return ok;};
 require(wait([&]{return browser.photo_library()&&!browser.photo_library()->loading()&&browser.photo_library()->rows().size()==n;}),"full prefix load");
 auto& library=*browser.photo_library();auto folder=library.folder();
 std::cout<<"operation,rows,median_ms,p95_ms,max_ms,rss_kib\n";report("controller_load",n,{ms(started)});
 folder->set_cursor(n-1);
 bench("properties_focus_churn",n,30,[&](int i){folder->set_cursor(n-1-static_cast<size_t>(i%2));auto p=library.properties(true);require(p.ids.size()==1,"focus churn scope");});
 folder->set_cursor(n-1);
 bench("properties_focused",n,30,[&](int){auto p=library.properties(true);require(p.ids.size()==1,"focused properties");});
 const auto draw=[&]{renderer.begin_frame(1100,820,1,{});browser.draw(renderer,{0,0,1100,820},palette,icons,Clock::now());renderer.end_frame();glFinish();};
 for(auto mode:{views::Browser::Mode::grid,views::Browser::Mode::details,views::Browser::Mode::columns}){browser.set_mode(mode);for(int i=0;i<5;++i)draw();bench("browser_hidden_info_draw_"+std::to_string(static_cast<int>(mode)),n,30,[&](int){draw();});}
 browser.set_mode(views::Browser::Mode::grid);browser.set_info_visible(true);for(int i=0;i<5;++i)draw();bench("browser_visible_info_grid_draw",n,15,[&](int){draw();});browser.set_info_visible(false);
 folder->select_all();started=Clock::now();auto ids=library.selected_ids();report("controller_select_all_sync",n,{ms(started)});require(ids.size()==n,"selected scope");
 bench("properties_bulk",n,1,[&](int){auto p=library.properties();require(p.ids.size()==n,"bulk scope");});
 folder->set_cursor(std::nullopt);
 std::vector<size_t> band(n);std::iota(band.begin(),band.end(),0);folder->select_band({},band);
 require(!folder->cursor_entry(),"rubber band cursor unexpectedly set");
 bench("properties_focused_no_cursor_band",n,1,[&](int){auto p=library.properties(true);require(p.ids.empty(),"no focused metadata without a cursor");});
 browser.set_mode(views::Browser::Mode::grid);browser.set_info_visible(false);
 bench("browser_hidden_info_band_draw",n,1,[&](int){draw();});
 bench("browser_hidden_info_band_steady_draw",n,30,[&](int){draw();});
 if(const char* target=std::getenv("AUDIT_FRAME_PATH")){std::vector<unsigned char> rgba(1100*820*4);glReadPixels(0,0,1100,820,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());std::ofstream out(target,std::ios::binary);out<<"P6\n1100 820\n255\n";for(int y=819;y>=0;--y)for(int x=0;x<1100;++x)out.write(reinterpret_cast<const char*>(rgba.data()+(y*1100+x)*4),3);}

 folder->set_cursor(n-1);(void)library.selected_ids();
 started=Clock::now();for(int i=0;i<30;++i)library.refresh();require(wait([&]{return !library.loading();}),"coalesced refresh");report("controller_refresh30_coalesced",n,{ms(started)});require(library.rows().size()==n,"refresh truncated");require(folder->cursor_entry()&&folder->cursor_entry()->key()=="photo:"+std::to_string(n),"refresh cursor lost");
 for(int i=0;i<30;++i)library.set_query("photo-"+std::to_string(i));library.set_query("photo-0000000.jpg");require(wait([&]{return !library.loading()&&library.rows().size()==1;}),"filter churn settle");require(library.rows()[0].value("id",0)==1,"stale query won");
 std::cout<<"filter_churn_pass,30,0,0,0,"<<rss()<<"\n";
 }catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
