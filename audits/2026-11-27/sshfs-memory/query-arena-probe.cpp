#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "rummage/core/memory.hpp"
#include "rummage/photos/client.hpp"

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 2;
    const int worker_count=argc==3 ? std::atoi(argv[2]) : 12;
    if (worker_count!=1 && worker_count!=12) return 2;
    rummage::core::give_back_large_blocks();
    rummage::photos::Client client{rummage::photos::Client::installed_path(), argv[1]};
    std::mutex mutex;
    std::condition_variable cv;
    int next=-1, finished=-1;
    bool stop=false;
    std::vector<std::thread> workers;
    const auto start=std::chrono::steady_clock::now();
    auto sample=[&](const char* phase,int worker) {
        const auto m=rummage::core::read_memory_report();
        nlohmann::json row{{"phase",phase},{"worker",worker},
          {"seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()},
          {"rss",m.rss},{"live",m.heap_in_use},{"free",m.heap_free},{"arenas",m.heap_arena},{"mmapped",m.heap_mmapped}};
        std::puts(row.dump().c_str());std::fflush(stdout);
    };
    sample("baseline",-1);
    for(int i=0;i<worker_count;++i) workers.emplace_back([&,i] {
        std::unique_lock lock{mutex};
        for(int query=i;query<12;query+=worker_count) {
        cv.wait(lock,[&]{return next==query;});
        lock.unlock();
        {
            nlohmann::json input{{"op","query"}},collected=nlohmann::json::array(),response;
            do {
                input["offset"]=collected.size();input["limit"]=1000;
                response=client.request(input);
                if (!response.value("ok",false)) {std::fprintf(stderr,"query failed\n");std::abort();}
                const auto& batch=response.at("photos");
                if(batch.empty()) break;
                for(const auto& row:batch) collected.push_back(row);
            }while(collected.size()<response.value("total",std::size_t{}));
            response["photos"]=std::move(collected);
            sample("held",i);
        }
        lock.lock();finished=query;cv.notify_all();
        }
        cv.wait(lock,[&]{return stop;});
    });
    for(int i=0;i<12;++i) {
        std::unique_lock lock{mutex};next=i;cv.notify_all();
        cv.wait(lock,[&]{return finished==i;});
        lock.unlock();sample("released",i%worker_count);
    }
    sample("before_trim",-1);
    rummage::core::give_back_free_memory();sample("after_trim",-1);
    {std::lock_guard lock{mutex};stop=true;cv.notify_all();}
    for(auto& t:workers)t.join();
    sample("threads_joined",-1);
}
