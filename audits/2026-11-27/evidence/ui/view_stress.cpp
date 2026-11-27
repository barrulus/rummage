#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <sys/resource.h>
#include <vector>
#include <unistd.h>
#include "rummage/core/event_loop.hpp"
#include "rummage/core/log.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/sort.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/grid_view.hpp"
#include "rummage/views/details_view.hpp"
#include "rummage/views/column_view.hpp"
using Clock=std::chrono::steady_clock;
using namespace rummage;
using namespace std::chrono_literals;
double ms(Clock::time_point a){return std::chrono::duration<double,std::milli>(Clock::now()-a).count();}
long rss(){std::ifstream f("/proc/self/statm");long a,b;f>>a>>b;return b*sysconf(_SC_PAGESIZE)/1024;}
void require(bool yes,const char* s){if(!yes)throw std::runtime_error(s);}
void report(const std::string& name,size_t n,std::vector<double> v){std::sort(v.begin(),v.end());std::cout<<name<<","<<n<<","<<v[v.size()/2]<<","<<v[std::min(v.size()-1,v.size()*95/100)]<<","<<v.back()<<","<<rss()<<"\n";}
template<class F>void bench(const std::string& name,size_t n,int count,F&& f){std::vector<double> v;for(int i=0;i<count;++i){auto t=Clock::now();f(i);v.push_back(ms(t));}report(name,n,std::move(v));}
bool until(core::EventLoop& loop,const std::function<bool()>& done){bool ok=false;auto p=loop.add_timer(1ms,1ms,[&]{if(done()){ok=true;loop.quit(0);}});auto w=loop.add_timer(10s,0ms,[&]{loop.quit(1);});loop.run();loop.remove(p);loop.remove(w);return ok;}
int main(int argc,char**argv){
 try{
  const size_t n=argc>1?std::stoull(argv[1]):10000;
  log::set_level(log::Level::off);
  core::EventLoop loop;core::WorkerPool pool(loop,2);fs::DirectoryWatcher watcher(loop);auto mime=fs::MimeDatabase::load();
  ui::Offscreen gl(1100,820);ui::FontSystem fonts;ui::Renderer renderer(fonts);ui::IconSet icons(fonts);theme::Palette palette;
  auto folder=std::make_shared<views::Folder>(loop,pool,watcher,mime,[]{});
  std::vector<fs::Entry> rows;rows.reserve(n);
  for(size_t i=0;i<n;++i){fs::Entry e;e.name="document-"+std::to_string(i)+".txt";e.identity="row:"+std::to_string(i);e.location=std::filesystem::path("/nonexistent-audit-fixture")/e.name;e.kind=fs::EntryKind::file;e.mime=mime->find("text/plain");e.available=false;e.size=i*10;e.modified_ns=1234000000000;rows.push_back(std::move(e));}
  folder->replace_collection(rows,"Generated fixture",n);folder->set_cursor(n-1);
  std::cout<<"operation,rows,median_ms,p95_ms,max_ms,rss_kib\n";
  bench("select_all",n,7,[&](int){folder->set_cursor(n-1);folder->select_all();require(folder->selected_count()==n,"select all");});
  bench("selected_names",n,7,[&](int){require(folder->selected_names().size()==n,"selected names");});
  bench("refresh_preserve_selection",n,7,[&](int){folder->replace_collection(rows,"Generated fixture",n);require(folder->selected_count()==n,"refresh selection");require(folder->cursor_entry()&&folder->cursor_entry()->key()=="row:"+std::to_string(n-1),"refresh cursor");});
  folder->set_cursor(n-1);
  fs::Listing listing;bench("sort_initial",n,1,[&](int){listing.add(rows);});
  bench("sort_change",n,4,[&](int i){listing.set_order({.key=i%2?fs::SortKey::name:fs::SortKey::size,.descending=i%2==0});});
  views::GridView grid(loop,pool,watcher,mime,[]{},[](ui::CursorShape){});
  views::DetailsView details(loop,pool,watcher,mime,[]{},[](ui::CursorShape){});
  views::ColumnView columns(loop,pool,watcher,mime,[]{},[](ui::CursorShape){});columns.set_info_visible(false);
  const auto run=[&](auto& view,const std::string& name){view.show(folder);auto draw=[&]{renderer.begin_frame(1100,820,1,{});view.draw(renderer,{0,0,1100,820},palette,icons,Clock::now());renderer.end_frame();glFinish();};for(int i=0;i<10;++i)draw();bench(name+"_draw_end",n,60,[&](int){draw();});bench(name+"_arrow_draw",n,30,[&](int i){folder->set_cursor(n-1-static_cast<size_t>(i));draw();});};
  run(grid,"grid");run(details,"details");run(columns,"columns");
  bench("replace_200_append",n,5,[&](int){auto copy=rows;for(size_t i=0;i<200;++i){auto e=rows[i%rows.size()];e.identity="append:"+std::to_string(i);copy.push_back(std::move(e));}folder->replace_collection(std::move(copy),"Generated fixture",n+200);require(folder->entries().size()==n+200,"append size");});
  long before=rss();for(int i=0;i<100;++i){folder->replace_collection(rows,"Generated fixture",n);folder->set_cursor(n-1);grid.show(folder);details.show(folder);columns.show(folder);}std::cout<<"churn_rss_delta_kib,"<<n<<","<<rss()-before<<",0,0,"<<rss()<<"\n";
  // Collection rows never touch originals. Ordinary browser navigation uses only generated dirs.
  std::filesystem::path root=argc>2?argv[2]:"/tmp/rummage-branch-audit-2026-11-27/ui/nav-fixture";
  std::filesystem::create_directories(root);
  for(int d=0;d<20;++d){auto dir=root/std::to_string(d);std::filesystem::create_directories(dir);for(int i=0;i<40;++i)std::ofstream(dir/(std::to_string(i)+".txt"))<<"fixture";}
  views::Browser browser(loop,pool,watcher,mime,[]{},[](ui::CursorShape){});browser.set_photos_enabled(false);
  for(int i=0;i<200;++i)browser.open(root/std::to_string(i%20));
  require(until(loop,[&]{auto f=browser.columns().focused_folder();return f&&!f->loading()&&f->entries().size()==40;}),"navigation settle");
  require(browser.columns().focused_folder()->path()==root/"19","stale navigation result");
  std::cout<<"navigation_churn_pass,200,0,0,0,"<<rss()<<"\n";
 return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
